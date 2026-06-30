# Checklist

- [x] `main.c` 的 `/* USER CODE BEGIN 0 */` 区域包含 `Motor_SetSpeed(uint16_t speed)` 函数
- [x] `Motor_SetSpeed` 函数体仅调用 `__HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, speed);`
- [x] `main()` 的 `/* USER CODE BEGIN 2 */` 区域调用了 `HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1)`
- [x] STBY 引脚在初始化阶段被置高（`HAL_GPIO_WritePin(STBY_GPIO_Port, STBY_Pin, GPIO_PIN_SET)`）
- [x] AIN1 置高、AIN2 置低，正转方向固定
- [x] `while(1)` 的 `/* USER CODE BEGIN 3 */` 区域含有一行 `Motor_SetSpeed(...)` 调用
- [x] 调速调用上方有注释提示可替换为 0 / 300 / 600 / 999
- [x] 未新建任何额外 `.h` / `.c` 文件
- [x] 未修改 `gpio.c` 或 `tim.c` 的 CubeMX 生成内容
