# Checklist

- [x] `main.c` 的 `Motor_SetSpeed()` 已去掉 `static` 关键字
- [x] `main.c` 在 `MX_USB_DEVICE_Init()` 之后调用了 `Motor_SetSpeed(0)`
- [x] `usbd_cdc_if.c` 在 `/* USER CODE BEGIN INCLUDE */` 区域声明了 `extern void Motor_SetSpeed(uint16_t speed);`
- [x] `CDC_Receive_FS()` 在收到 `'0'` 时调用 `Motor_SetSpeed(0)`
- [x] `CDC_Receive_FS()` 在收到 `'1'` 时调用 `Motor_SetSpeed(300)`
- [x] `CDC_Receive_FS()` 在收到 `'2'` 时调用 `Motor_SetSpeed(600)`
- [x] `CDC_Receive_FS()` 在收到 `'3'` 时调用 `Motor_SetSpeed(999)`
- [x] `CDC_Receive_FS()` 末尾保留了 `USBD_CDC_SetRxBuffer` 与 `USBD_CDC_ReceivePacket` 调用
- [x] `CDC_Receive_FS()` 在命令处理后调用了 `memset(UserRxBufferFS, 0, APP_RX_DATA_SIZE)` 清空缓冲区
- [x] `usbd_cdc_if.c` 加了 `#include <string.h>` 与 `#include <stdio.h>`
- [x] `CDC_Receive_FS()` 在收到 `'0' / '1' / '2' / '3'` 后回传 `OK:speed=<n>\r\n`
- [x] `CDC_Receive_FS()` 在收到非法字符（`\r` / `\n` 除外）后回传 `ERR\r\n`
- [x] `CDC_Init_FS()` 在枚举完成时回传 `RDY\r\n`
- [x] 未新增任何 `.h` / `.c` 文件
- [x] 未修改 `usb_device.c` / `usbd_desc.c` / `usbd_conf.c` 的 CubeMX 生成内容
