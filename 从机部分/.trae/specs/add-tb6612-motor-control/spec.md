# TB6612 A 路电机最简化控制 Spec

## Why
当前 STM32C8T6 工程中 TIM2_CH1 (PA0)、STBY (PB12)、AIN1 (PB13)、AIN2 (PB14) 已在 CubeMX 中完成配置，但 `main.c` 仍是空模板，电机无法运行。需要在最小化改动的前提下，让 A 路直流电机以软件手动切换档位的方式正转运行。

## What Changes
- 在 `main.c` 的 USER CODE 区域新增 `Motor_SetSpeed()` 函数
- 在 `main()` 初始化阶段启动 TIM2 PWM 输出，并置位 STBY/AIN1/AIN2
- 在 `while(1)` 中通过单行调用切换 4 档速度
- **不**新建任何 `.h` / `.c` 文件
- **不**使用按键、串口、中断、状态机
- **不**修改 CubeMX 生成的 `gpio.c` / `tim.c`

## Impact
- Affected specs: 无
- Affected code:
  - [main.c](file:///d:/embeded/昇腾开发板/昇腾课设代码/HuaweiOrangepai/从机部分/STM32C8T6/Core/Src/main.c)（唯一修改文件）

## ADDED Requirements

### Requirement: 4 档软件调速
系统 SHALL 在 `main.c` 内实现一个无参静态调速函数，可通过修改 `while(1)` 中的一行参数来切换电机档位。

#### Scenario: 停止档
- **WHEN** 调用 `Motor_SetSpeed(0)`
- **THEN** PWM 比较值被设置为 0，电机停止

#### Scenario: 30% 速度
- **WHEN** 调用 `Motor_SetSpeed(300)`
- **THEN** PWM 比较值被设置为 300，约 30% 占空比

#### Scenario: 60% 速度
- **WHEN** 调用 `Motor_SetSpeed(600)`
- **THEN** PWM 比较值被设置为 600，约 60% 占空比

#### Scenario: 全速
- **WHEN** 调用 `Motor_SetSpeed(999)`
- **THEN** PWM 比较值被设置为 999，约 100% 占空比

### Requirement: 方向与使能引脚固定
系统 SHALL 在初始化阶段把方向与使能引脚固定为电机正转状态：STBY=1、AIN1=1、AIN2=0，运行期间不再改变。

#### Scenario: 启动后立即正转
- **WHEN** `main()` 完成 `MX_GPIO_Init()` 与 `MX_TIM2_Init()`
- **THEN** 依次执行 `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)`、`HAL_GPIO_WritePin(STBY,1)`、`HAL_GPIO_WritePin(AIN1,1)`、`HAL_GPIO_WritePin(AIN2,0)`

### Requirement: CubeMX 兼容
所有新增代码 MUST 落在 `main.c` 的 USER CODE BEGIN / END 标记之间，避免 CubeMX 重新生成时被覆盖。
