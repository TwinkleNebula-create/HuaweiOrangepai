# Tasks

- [x] Task 1: 在 `main.c` USER CODE BEGIN 0 区域实现 `Motor_SetSpeed(uint16_t speed)`
  - [x] SubTask 1.1: 函数体仅一行：`__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, speed);`
  - [x] SubTask 1.2: 函数声明放在 `/* USER CODE BEGIN 0 */` 与 `/* USER CODE END 0 */` 之间

- [x] Task 2: 在 `main()` USER CODE BEGIN 2 区域完成电机启动初始化
  - [x] SubTask 2.1: 调用 `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)` 启动 PWM
  - [x] SubTask 2.2: 置 `STBY_Pin` 高电平（使能驱动）
  - [x] SubTask 2.3: 置 `AIN1_Pin` 高电平、`AIN2_Pin` 低电平（正转方向）

- [x] Task 3: 在 `while(1)` USER CODE BEGIN 3 区域放置调速调用并加注释
  - [x] SubTask 3.1: 用一行 `Motor_SetSpeed(600);`（默认 60% 速度）
  - [x] SubTask 3.2: 在该行上方加注释列出 4 档可替换值：0 / 300 / 600 / 999

# Task Dependencies
- Task 2 依赖 Task 1（Motor_SetSpeed 函数已定义）
- Task 3 依赖 Task 1、Task 2（启动完毕后才能持续调速）
