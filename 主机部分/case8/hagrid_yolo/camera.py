import json
import queue
import threading
import time

import cv2

from .metadata import available_video_devices, get_source
from .visualization import draw_detections


# camera.py 是“摄像头采集 + 模型推理循环”模块。
# 它不直接控制 STM32，也不直接打开串口。
# 如果外部需要拿到识别结果，就通过 infer_camera() 的 detection_callback 参数接收。


def infer_image(source, detector, labels, save=None, no_window=False, window_title="YOLO"):
    """单张图片推理，用于测试模型和后处理是否正常。"""
    image = cv2.imread(str(get_source(source)))
    if image is None:
        raise RuntimeError(f"Cannot read image: {source}")

    detections, latency_ms = detector.infer_frame(image)
    draw_detections(image, detections, labels)
    print(json.dumps(detections, indent=2))
    print(f"[{window_title}] inference latency: {latency_ms:.2f} ms")

    if save:
        cv2.imwrite(str(save), image)
    elif not no_window:
        cv2.imshow(window_title, image)
        cv2.waitKey(0)


def infer_camera(
    source,
    detector,
    labels,
    camera_width=640,
    camera_height=480,
    camera_fps=30,
    infer_every_n=1,
    sync_infer=False,
    max_frames=0,
    no_window=False,
    window_title="YOLO",
    latency_label="Infer",
    # 外部控制回调：camera.py 只负责采集和推理，不直接 import 串口模块。
    # 需要控制 STM32 时，由入口脚本传入 stm32.update；不传时原推理流程保持不变。
    detection_callback=None,
):
    """摄像头实时推理主循环。

    这个函数的职责：
    1. 打开摄像头。
    2. 不断读取画面 frame。
    3. 按 infer_every_n 的间隔调用 detector.infer_frame(frame) 得到 detections。
    4. 如果传入 detection_callback，就把最新 detections 交给外部模块。

    对 STM32 控制来说，detection_callback 通常就是 stm32.update。
    所以发送链路是：
    detector.infer_frame -> detections -> detection_callback -> stm32.update -> serial.write。
    """
    # 打开摄像头。source 一般是 /dev/video0，也可以是视频文件路径。
    cap = cv2.VideoCapture(get_source(source))
    if not cap.isOpened():
        devices = available_video_devices()
        device_text = ", ".join(devices) if devices else "none"
        raise RuntimeError(f"Cannot open video source: {source}. Available video devices: {device_text}")

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, camera_width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, camera_height)
    cap.set(cv2.CAP_PROP_FPS, camera_fps)

    # 下面这些变量用于统计帧率、推理次数和最近一次检测结果。
    prev_t = time.time()
    frame_id = 0
    # last_detections 保存最近一次 YOLO 检测结果。
    # 即使当前帧没有重新推理，画框和串口控制也可以使用最近一次结果。
    last_detections = []
    last_latency_ms = 0.0
    infer_every_n = max(1, infer_every_n)
    processed_frames = 0
    infer_count = 0
    total_latency_ms = 0.0
    start_t = time.time()
    result_lock = threading.Lock()
    frame_queue = queue.Queue(maxsize=1)
    stop_event = threading.Event()
    worker = None
    # 异步推理时用于判断“是否有新的推理结果”。
    # 只有 infer_count 变大，才调用一次 detection_callback，避免重复发送相同结果。
    last_callback_infer_count = 0

    if not sync_infer:
        # 默认是异步推理：主线程负责取摄像头画面，后台线程负责跑模型。
        # shared 是主线程和后台线程之间共享的一小块状态。
        shared = {"detections": [], "infer_count": 0, "latency_ms": 0.0, "total_latency_ms": 0.0, "error": None}

        def infer_worker():
            """后台推理线程：从 frame_queue 取图，跑模型，把结果写回 shared。"""
            while not stop_event.is_set():
                try:
                    worker_frame = frame_queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                try:
                    # detector.infer_frame 是真正调用模型的位置，返回 detections 和推理耗时。
                    detections, latency_ms = detector.infer_frame(worker_frame)
                    with result_lock:
                        shared["detections"] = detections
                        shared["infer_count"] += 1
                        shared["latency_ms"] = latency_ms
                        shared["total_latency_ms"] += latency_ms
                except Exception as exc:
                    with result_lock:
                        shared["error"] = exc
                    stop_event.set()
                finally:
                    frame_queue.task_done()

        worker = threading.Thread(target=infer_worker, daemon=True)
        worker.start()

    try:
        while True:
            # 主循环每次从摄像头读取一帧。
            ok, frame = cap.read()
            if not ok:
                break

            if sync_infer and frame_id % infer_every_n == 0:
                # 同步推理模式：取到 frame 后马上在主线程里推理。
                # 优点是流程直观；缺点是推理慢时摄像头画面会卡住。
                last_detections, last_latency_ms = detector.infer_frame(frame)
                infer_count += 1
                total_latency_ms += last_latency_ms
                # 同步推理模式：本轮循环刚产生新结果，立即把 detections 交给外部控制模块。
                if detection_callback is not None:
                    detection_callback(last_detections, labels)
            elif not sync_infer:
                with result_lock:
                    worker_error = shared["error"]
                if worker_error is not None:
                    raise RuntimeError(f"Background inference failed: {worker_error}") from worker_error

                if frame_id % infer_every_n == 0 and frame_queue.empty():
                    try:
                        # 把当前画面复制一份交给后台线程推理。
                        # 队列大小为 1，能避免推理跟不上时积压很多旧帧。
                        frame_queue.put_nowait(frame.copy())
                    except queue.Full:
                        pass
                with result_lock:
                    # 从后台线程共享状态中取出最新推理结果。
                    last_detections = list(shared["detections"])
                    infer_count = shared["infer_count"]
                    last_latency_ms = shared["latency_ms"]
                    total_latency_ms = shared["total_latency_ms"]

                # 异步推理模式：后台线程负责推理，主循环只读取 shared 里的最新结果。
                # infer_count 增加代表出现了新的推理结果；这样可避免同一个结果重复发串口命令。
                if detection_callback is not None and infer_count > last_callback_infer_count:
                    detection_callback(last_detections, labels)
                    last_callback_infer_count = infer_count

            if not no_window:
                # 只负责可视化画框，不影响串口发送。
                draw_detections(frame, last_detections, labels)

            now = time.time()
            fps = 1.0 / max(now - prev_t, 1e-6)
            prev_t = now
            if not no_window:
                cv2.putText(
                    frame,
                    f"FPS: {fps:.1f}, Infer: 1/{infer_every_n}, {latency_label}: {last_latency_ms:.1f} ms",
                    (20, 30),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.9,
                    (0, 255, 0),
                    2,
                )
            frame_id += 1
            processed_frames += 1

            if no_window:
                # 无窗口模式常用于 SSH 到开发板运行，此时不调用 cv2.imshow。
                if max_frames and processed_frames >= max_frames:
                    break
                continue

            cv2.imshow(window_title, frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
            if max_frames and processed_frames >= max_frames:
                break
    finally:
        cap.release()
        stop_event.set()
        if worker:
            worker.join(timeout=1.0)
        if not no_window:
            cv2.destroyAllWindows()

    elapsed = time.time() - start_t
    if elapsed > 0 and (no_window or max_frames):
        avg_latency_ms = total_latency_ms / infer_count if infer_count else 0.0
        print(
            f"Processed {processed_frames} frames, {infer_count} inferences, "
            f"camera FPS {processed_frames / elapsed:.2f}, "
            f"inference FPS {infer_count / elapsed:.2f}, "
            f"avg {latency_label} latency {avg_latency_ms:.2f} ms"
        )
