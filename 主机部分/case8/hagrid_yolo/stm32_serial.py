"""把手势识别结果通过串口发送给 STM32。

这个文件是“手势结果 -> STM32 命令”的独立转换层。

整体数据流如下：
1. camera.py 从摄像头取图，并调用 YOLO 模型得到 detections 检测结果。
2. infer_om_camera.py 把 stm32.update 作为回调函数传给 camera.py。
3. camera.py 每产生一次新的检测结果，就调用 stm32.update(detections, labels)。
4. 本文件从 detections 中选出一个手势标签，把它转换成 '0'/'1'/'2'/'3' 这样的命令字符。
5. STM32 的 USB CDC 接收回调收到字符后，再设置电机 PWM。

这样拆分的好处：
- 推理代码不需要知道 STM32 协议。
- STM32 串口发送代码不需要知道模型内部细节。
- 以后改“哪个手势控制哪个速度”，主要改 DEFAULT_GESTURE_COMMANDS 这个字典。
"""

import time


# 这个字典就是最重要、最常修改的参数表。
# 左边 key：模型识别出来的手势标签，来自 models/YOLOv10n_gestures_labels.txt。
# 右边 value：真正发给 STM32 的 ASCII 命令，可以是 '0' 这种单字节，也可以是 switch/palm 这种短文本。
#
# 你现在的 STM32 从机代码已经能识别这些字符：
# '0' -> Motor_SetSpeed(0)，停止
# '1' -> Motor_SetSpeed(300)，低速
# '2' -> Motor_SetSpeed(600)，中速
# '3' -> Motor_SetSpeed(999)，高速
# switch -> 首页切换菜单
# like/dislike -> 进入/退出界面
# palm/fist -> 舵机页开门/关门
#
# 例子：如果你想让 like 手势表示高速，就加一行："like": "3"。
DEFAULT_GESTURE_COMMANDS = {
    "stop": "0",
    "stop_inverted": "0",
    "palm": "palm",
    "fist": "fist",
    "like": "like",
    "dislike": "dislike",
    "thumb_index": "switch",
    "three_gun": "switch",
    "one": "1",
    "two_up": "2",
    "peace": "2",    
    "peace": "2",  
    "peace_inverted": "2",
    "three": "3",
    "three2": "3",
    "three3": "3",
}

class Stm32SerialController:
    """把 YOLO 检测结果转换成 STM32 串口命令。

    camera.py 不直接操作串口，它只调用 update()。
    这个类内部做三件事：
    1. 从当前帧的 detections 中选出置信度最高的检测结果。
    2. 用 labels 把 class_id 转成手势名字，比如 stop、one、two_up。
    3. 用 DEFAULT_GESTURE_COMMANDS 把手势名字转成命令字符，并通过串口发出。
    """

    def __init__(
        self,
        port,
        baudrate=115200,
        min_interval=1.0,
        default_command="0",
        gesture_commands=None,
        dry_run=False,
    ):
        # port 是 Linux 下的串口设备名。STM32 USB CDC 通常是 /dev/ttyACM0。
        self.port = port
        # baudrate 是波特率。USB CDC 有时不严格依赖它，但 pyserial 打开串口时仍需要这个参数。
        self.baudrate = baudrate
        # min_interval 是命令稳定时间，防止手势切换途中短暂误识别被发送。
        self.min_interval = max(0.0, min_interval)
        # default_command 是默认命令。识别不到手势或手势没有配置时，默认发 '0' 停止电机。
        self.default_command = default_command
        # gesture_commands 是手势到命令的映射表。这里复制一份，避免外部修改影响运行中的对象。
        self.gesture_commands = dict(gesture_commands or DEFAULT_GESTURE_COMMANDS)
        # dry_run 是调试模式：不打开真实串口，只在终端打印将要发送的命令。
        self.dry_run = dry_run

        self._serial = None
        self._last_command = None
        self._last_send_time = 0.0
        self._pending_command = None
        self._pending_label = None
        self._pending_since = 0.0

        if not dry_run:
            # 只有真正启用串口时才导入 pyserial。
            # 这样不接 STM32、只跑手势识别时，不会因为缺少 pyserial 影响主程序。
            try:
                import serial
            except ImportError as exc:
                raise RuntimeError("pyserial is required. Install it with: pip install pyserial") from exc

            # 打开 STM32 的 USB CDC 串口设备。如果失败，优先检查 /dev/ttyACM0 是否存在以及权限是否足够。
            self._serial = serial.Serial(port=port, baudrate=baudrate, timeout=0.05)

    def close(self):
        """程序退出时关闭串口，释放设备。"""
        if self._serial is not None and self._serial.is_open:
            self._serial.close()

    def update(self, detections, labels):
        """接收 camera.py 传来的最新检测结果，并发送一个电机控制命令。

        detections 是检测结果列表，每个元素大概长这样：
            {"box": [x1, y1, x2, y2], "score": 0.93, "class_id": 24}

        labels 是标签表，从 *_labels.txt 读取。
        它的作用是把 class_id 转成手势名字，例如 "stop"、"one"、"two_up"、"three"。
        """
        # 第一步：从当前帧的 detections 中选一个最可信的手势标签。
        label = self._select_label(detections, labels)
        # 第二步：把手势标签查表转换成 STM32 命令。查不到就用 default_command，也就是停止。
        command = self.gesture_commands.get(label, self.default_command)
        now = time.time()

        if command != self._pending_command:
            self._pending_command = command
            self._pending_label = label
            self._pending_since = now
        else:
            self._pending_label = label

        if now - self._pending_since < self.min_interval:
            return

        # 第三步：命令稳定足够久之后，真正发送命令字符。
        self.send_command(command, self._pending_label)

    def send_command(self, command, label=None):
        """向 STM32 发送 1 个 ASCII 命令字符。

        注意：这里不是发送完整 JSON，也不是发送手势名字。
        实际发送的是一个短 ASCII 命令，例如 b'0'、b'3' 或 b'switch'。
        STM32 收到命令后，会结合当前 OLED 页面决定具体动作。
        """
        now = time.time()

        # 同一个命令已经发过时，不再重复发送。
        # 这样同一个手势一直在镜头前时，串口不会被重复命令刷屏。
        if command == self._last_command:
            return

        if self.dry_run:
            # 调试模式：只打印，不打开串口。适合先确认手势映射是否符合预期。
            print(f"[STM32 dry-run] gesture={label or 'none'} command={command}")
        else:
            # 正常模式：把字符编码成 ASCII 字节并写入串口。
            self._serial.write(command.encode("ascii"))
            # flush 确保数据尽快发出去，减少控制延迟。
            self._serial.flush()

        # 记录本次发送，用于下一次限频判断。
        self._last_command = command
        self._last_send_time = now

    @staticmethod
    def _select_label(detections, labels):
        """从当前帧选择一个手势标签。

        一帧图像里可能没有手势，也可能检测到多个手势。
        但电机控制需要一个明确指令，所以这里选择 score 最大，也就是置信度最高的检测结果。
        """
        if not detections:
            # 没识别到任何东西，返回 None；上层会使用 default_command 停止电机。
            return None

        # max(..., key=...) 表示按 score 字段找出最高置信度的检测结果。
        best = max(detections, key=lambda item: item.get("score", 0.0))
        class_id = best.get("class_id", -1)
        # class_id 是数字下标，labels[class_id] 才是人能读懂的手势名字。
        if 0 <= class_id < len(labels):
            return labels[class_id]
        return None
