#!/usr/bin/env python3
"""OM 模型摄像头推理入口。

这个脚本可以理解成“总装配入口”：
1. 解析命令行参数，例如模型路径、摄像头编号、是否启用串口。
2. 加载 Ascend OM 模型，并创建 YoloDetector 检测器。
3. 如果传入 --serial-port，就创建 Stm32SerialController 串口控制器。
4. 调用 infer_camera() 启动摄像头循环。
5. 把 stm32.update 作为 detection_callback 传给 infer_camera()。

关键数据流：
摄像头画面 -> YOLO 检测结果 detections -> stm32.update() -> 串口字符 '0'/'1'/'2'/'3' -> STM32 设置 PWM。
"""

import argparse
import sys
import time
from pathlib import Path
import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
DEFAULT_MODEL = ROOT / "models" / "YOLOv10n_gestures.om"

from hagrid_yolo.backends.acl_backend import AclModel
from hagrid_yolo.camera import infer_camera, infer_image
from hagrid_yolo.detector import YoloDetector
from hagrid_yolo.metadata import load_labels, resolve_imgsz
from hagrid_yolo.stm32_serial import Stm32SerialController


def parse_args():
    """解析命令行参数。

    这里大部分参数是原来的模型推理参数。
    新增的串口参数都以 --serial 开头；不传 --serial-port 时，串口控制不会启用。
    """
    parser = argparse.ArgumentParser(description="Run YOLO OM inference with Ascend ACL and OpenCV.")
    parser.add_argument("-m", "--model", default=DEFAULT_MODEL, type=Path)
    parser.add_argument("-l", "--labels", default=None, type=Path)
    parser.add_argument("-s", "--source", default="/dev/video0", type=str)
    parser.add_argument("--imgsz", default=0, type=int, help="Use 0 to read metadata, then fallback to 640.")
    parser.add_argument("--conf", default=0.25, type=float)
    parser.add_argument("--iou", default=0.45, type=float)
    parser.add_argument("--device-id", default=0, type=int)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--save", default=None, type=Path)
    parser.add_argument("--camera-width", default=640, type=int)
    parser.add_argument("--camera-height", default=480, type=int)
    parser.add_argument("--camera-fps", default=30, type=int)
    parser.add_argument("--infer-every-n", default=1, type=int)
    parser.add_argument("--sync-infer", action="store_true")
    parser.add_argument("--max-frames", default=0, type=int)
    parser.add_argument("--no-window", action="store_true")
    parser.add_argument("--opencv-threads", default=1, type=int)
    parser.add_argument("--benchmark-runs", default=0, type=int)
    parser.add_argument("--warmup-runs", default=2, type=int)
    parser.add_argument("--print-model-info", action="store_true")
    # STM32 串口控制参数：
    # --serial-port 指定串口设备，例如 /dev/ttyACM0。
    # --serial-dry-run 不打开真实串口，只打印将要发送的命令，适合先测试映射关系。
    parser.add_argument("--serial-port", default=None, help="STM32 USB CDC device, for example /dev/ttyACM0.")
    parser.add_argument("--serial-baudrate", default=115200, type=int)
    parser.add_argument("--serial-min-interval", default=0.2, type=float, help="Minimum seconds between repeated sends.")
    parser.add_argument("--serial-dry-run", action="store_true", help="Print STM32 commands instead of opening the serial port.")
    return parser.parse_args()


def run_benchmark(args, backend):
    tensor = np.zeros((1, 3, args.imgsz, args.imgsz), dtype=np.float32)

    for _ in range(max(0, args.warmup_runs)):
        backend.infer(tensor)

    runs = max(1, args.benchmark_runs)
    latencies = []
    last_outputs = None
    for _ in range(runs):
        start_t = time.time()
        last_outputs = backend.infer(tensor)
        latencies.append((time.time() - start_t) * 1000.0)

    latency_array = np.asarray(latencies, dtype=np.float32)
    print(
        f"[OM] benchmark runs={runs}, "
        f"avg={latency_array.mean():.2f} ms, "
        f"min={latency_array.min():.2f} ms, "
        f"max={latency_array.max():.2f} ms"
    )
    if last_outputs:
        for index, output in enumerate(last_outputs):
            print(f"[OM] output[{index}] shape={output.shape} dtype={output.dtype}")


def main():
    args = parse_args()
    # 支持两种模型路径写法：
    # 1. 绝对路径，例如 /home/.../YOLOv10n_gestures.om
    # 2. 相对 case8/ 的路径，例如 models/YOLOv10n_gestures.om
    if not args.model.is_absolute() and not args.model.exists():
        candidate = ROOT / args.model
        if candidate.exists():
            args.model = candidate
    # imgsz=0 时从模型 metadata 中读取输入尺寸，读取不到再走默认逻辑。
    args.imgsz = resolve_imgsz(args.model, args.imgsz)
    cv2.setNumThreads(max(1, args.opencv_threads))

    if not args.model.exists():
        raise FileNotFoundError(f"OM model not found: {args.model}")

    labels = None
    if args.benchmark_runs <= 0:
        # labels 是 class_id 到手势名字的表。
        # 例如检测结果里只有 class_id=24，labels 可以把它翻译成 "stop"。
        labels = load_labels(args.model, args.labels)

    # backend 负责调用昇腾 OM 模型做原始推理。
    # detector 在 backend 外面包了一层，负责图片预处理、后处理、NMS，并返回 detections。
    backend = AclModel(args.model, device_id=args.device_id)
    detector = YoloDetector(backend, imgsz=args.imgsz, conf=args.conf, iou=args.iou)
    stm32 = None

    if args.serial_port or args.serial_dry_run:
        # 只有显式启用串口时才创建 STM32 控制器。
        # 这样平时只跑识别时，不会打开串口，也不会影响原来的功能。
        stm32 = Stm32SerialController(
            port=args.serial_port or "dry-run",
            baudrate=args.serial_baudrate,
            min_interval=args.serial_min_interval,
            dry_run=args.serial_dry_run,
        )
        print(f"[STM32] serial control enabled port={args.serial_port or 'dry-run'} baudrate={args.serial_baudrate}")

    try:
        if args.print_model_info:
            backend.print_model_info()
        if args.benchmark_runs > 0:
            run_benchmark(args, backend)
            return
        if args.once:
            infer_image(args.source, detector, labels, save=args.save, no_window=args.no_window, window_title="OM YOLO")
        else:
            infer_camera(
                args.source,
                detector,
                labels,
                camera_width=args.camera_width,
                camera_height=args.camera_height,
                camera_fps=args.camera_fps,
                infer_every_n=args.infer_every_n,
                sync_infer=args.sync_infer,
                max_frames=args.max_frames,
                no_window=args.no_window,
                window_title="OM YOLO",
                latency_label="NPU",
                # 这是“识别结果 -> 串口发送”的连接点。
                # infer_camera 每得到一次新的 detections，就会调用 stm32.update(detections, labels)。
                # 如果 stm32 为 None，说明没有启用串口，回调就是 None，原推理流程不变。
                detection_callback=stm32.update if stm32 else None,
            )
    finally:
        # 无论正常退出还是发生异常，都释放硬件资源。
        if stm32 is not None:
            stm32.close()
        backend.release()


if __name__ == "__main__":
    main()
