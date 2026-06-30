# USB CDC 虚拟串口调速 Spec

## Why
当前 `main.c` 中电机只能通过修改 `Motor_SetSpeed()` 参数手动换档，无法在运行中通过上位机调整速度。需要在最小改动前提下，利用工程已配好的 USB Device FS + CDC Virtual Port，让单字符命令 `0/1/2/3` 直接控制 PWM 占空比。

## What Changes
- `main.c` 中把 `Motor_SetSpeed()` 由 `static` 改为外部可见
- `main.c` 启动序列末尾追加 `Motor_SetSpeed(0)`，保证上电停在停止档
- `usbd_cdc_if.c` 在 `CDC_Receive_FS()` 中按 `Buf[0]` 解析 `0/1/2/3` 并调用 `Motor_SetSpeed()`
- `usbd_cdc_if.c` 收到有效命令后用 `CDC_Transmit_FS` 回传 `OK:speed=<n>\r\n`
- `usbd_cdc_if.c` 收到非法字符（非 `\r` / `\n`）回传 `ERR\r\n`
- `usbd_cdc_if.c` 在 `CDC_Init_FS()` 中回传 `RDY\r\n` 提示枚举完成
- `usbd_cdc_if.c` 顶部用 `extern` 声明 `Motor_SetSpeed`
- `usbd_cdc_if.c` 命令处理完后用 `memset` 清空 `UserRxBufferFS`
- `usbd_cdc_if.c` 顶部加 `#include <string.h>` 与 `#include <stdio.h>` 以提供 `memset` / `sprintf`
- **不**修改 CubeMX 生成的 `usb_device.c` / `usbd_desc.c` / `usbd_conf.c`
- **不**新增 `.h` / `.c` 文件
- **不**改用 USART、不加按键、不加定时器中断

## Impact
- Affected specs: 无
- Affected code:
  - [main.c](file:///d:/embeded/昇腾开发板/昇腾课设代码/HuaweiOrangepai/从机部分/STM32C8T6/Core/Src/main.c)
  - [usbd_cdc_if.c](file:///d:/embeded/昇腾开发板/昇腾课设代码/HuaweiOrangepai/从机部分/STM32C8T6/USB_DEVICE/App/usbd_cdc_if.c)

## ADDED Requirements

### Requirement: 单字符命令调速
USB CDC 接收回调 SHALL 在收到 `Buf[0]` 为 `'0' / '1' / '2' / '3'` 时，立刻调用 `Motor_SetSpeed()` 对应档位：`0 / 300 / 600 / 999`。非法字符不处理。

#### Scenario: 收到 '0'
- **WHEN** 上位机发送字符 `0`
- **THEN** `Motor_SetSpeed(0)` 被调用，PWM 占空比置 0，电机停转

#### Scenario: 收到 '1'
- **WHEN** 上位机发送字符 `1`
- **THEN** `Motor_SetSpeed(300)` 被调用，约 30% 占空比

#### Scenario: 收到 '2'
- **WHEN** 上位机发送字符 `2`
- **THEN** `Motor_SetSpeed(600)` 被调用，约 60% 占空比

#### Scenario: 收到 '3'
- **WHEN** 上位机发送字符 `3`
- **THEN** `Motor_SetSpeed(999)` 被调用，全速

### Requirement: 不阻塞 USB
`CDC_Receive_FS()` MUST 在命令解析后继续执行原有的 `USBD_CDC_SetRxBuffer` 与 `USBD_CDC_ReceivePacket`，保证下一次接收可用。

#### Scenario: 命令后保持接收
- **WHEN** 任一档位命令被处理
- **THEN** 同一函数内继续执行 `USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0])` 与 `USBD_CDC_ReceivePacket(&hUsbDeviceFS)`

### Requirement: 处理后清空接收缓冲区
`CDC_Receive_FS()` MUST 在命令解析之后、`USBD_CDC_SetRxBuffer` 之前用 `memset(UserRxBufferFS, 0, APP_RX_DATA_SIZE)` 清空接收缓冲区，避免残留字节影响下一帧解析。

#### Scenario: 清空缓冲区
- **WHEN** 当前帧的命令已被处理
- **THEN** `UserRxBufferFS` 的全部 `APP_RX_DATA_SIZE` 字节被写为 0

### Requirement: 跨文件可见
`Motor_SetSpeed()` MUST 从 `static` 改为外部链接，使 `usbd_cdc_if.c` 可通过 `extern` 声明调用；`main.c` 启动后 SHALL 调用 `Motor_SetSpeed(0)` 复位初始速度。

#### Scenario: 上电停在停止档
- **WHEN** `main()` 完成 USB 与电机初始化
- **THEN** `Motor_SetSpeed(0)` 被调用，电机初始为停止状态，等待上位机指令

### Requirement: 串口回传确认
USB CDC SHALL 在收到有效命令与枚举完成时通过 `CDC_Transmit_FS` 回传简短字符串。

#### Scenario: 收到有效命令回 OK
- **WHEN** 上位机发送 `'0' / '1' / '2' / '3'`
- **THEN** STM32 通过 CDC 发送 `OK:speed=0\r\n` / `OK:speed=300\r\n` / `OK:speed=600\r\n` / `OK:speed=999\r\n`

#### Scenario: 收到非法字符回 ERR
- **WHEN** 上位机发送 `'0' / '1' / '2' / '3'` 之外的字符（`\r` / `\n` 除外）
- **THEN** STM32 通过 CDC 发送 `ERR\r\n`

#### Scenario: 枚举完成回 RDY
- **WHEN** USB 主机完成 CDC 类初始化
- **THEN** STM32 在 `CDC_Init_FS()` 内发送 `RDY\r\n`
