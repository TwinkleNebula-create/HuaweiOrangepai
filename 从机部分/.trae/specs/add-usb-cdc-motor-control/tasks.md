# Tasks

- [x] Task 1: 修改 `main.c` 中 `Motor_SetSpeed()` 的链接性
  - [x] SubTask 1.1: 去掉 `static` 关键字，改为外部链接
  - [x] SubTask 1.2: 保留函数注释与函数体不变

- [x] Task 2: 在 `main.c` 初始化末尾追加 `Motor_SetSpeed(0)`
  - [x] SubTask 2.1: 在 `MX_USB_DEVICE_Init()` 之后、`HAL_TIM_PWM_Start` 之后的位置追加 `Motor_SetSpeed(0);`
  - [x] SubTask 2.2: 代码放在 `/* USER CODE BEGIN 2 */` 与 `/* USER CODE END 2 */` 之间

- [x] Task 3: 在 `usbd_cdc_if.c` 添加 `Motor_SetSpeed` 外部声明
  - [x] SubTask 3.1: 在 `/* USER CODE BEGIN INCLUDE */` 区域加 `#include "main.h"` 与 `extern void Motor_SetSpeed(uint16_t speed);`

- [x] Task 4: 在 `usbd_cdc_if.c` 的 `CDC_Receive_FS()` 中解析命令
  - [x] SubTask 4.1: 在 `USBD_CDC_SetRxBuffer` 之前根据 `Buf[0]` 分支调用 `Motor_SetSpeed`
  - [x] SubTask 4.2: 仅处理 `'0' / '1' / '2' / '3'` 四种字符，其它字符不操作
  - [x] SubTask 4.3: 保留原有的 `USBD_CDC_SetRxBuffer` 与 `USBD_CDC_ReceivePacket` 调用

- [x] Task 5: 在 `CDC_Receive_FS()` 处理完后清空接收缓冲区
  - [x] SubTask 5.1: 加 `#include <string.h>` 提供 `memset`
  - [x] SubTask 5.2: 在命令解析后、`USBD_CDC_SetRxBuffer` 之前调用 `memset(UserRxBufferFS, 0, APP_RX_DATA_SIZE)`

- [x] Task 6: 在 `CDC_Receive_FS()` 与 `CDC_Init_FS()` 中加回传
  - [x] SubTask 6.1: 加 `#include <stdio.h>` 提供 `sprintf`
  - [x] SubTask 6.2: 收到 `'0' / '1' / '2' / '3'` 后 `CDC_Transmit_FS("OK:speed=<n>\r\n", ...)`
  - [x] SubTask 6.3: 收到非法字符（过滤 `\r` / `\n`）后 `CDC_Transmit_FS("ERR\r\n", 5)`
  - [x] SubTask 6.4: 在 `CDC_Init_FS()` 末尾 `CDC_Transmit_FS("RDY\r\n", 5)`

# Task Dependencies
- Task 2 依赖 Task 1（`Motor_SetSpeed` 必须可被调用）
- Task 3 依赖 Task 1（`Motor_SetSpeed` 必须外部可见）
- Task 4 依赖 Task 3（必须先声明再使用）
- Task 5 依赖 Task 4（在命令处理逻辑稳定后再清空）
- Task 6 依赖 Task 4（必须在命令解析基础上加回传）
