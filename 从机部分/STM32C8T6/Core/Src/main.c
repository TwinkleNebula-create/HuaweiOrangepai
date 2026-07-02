/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* OLED 应用层接口：底层驱动在 App/oled.c，主程序只负责初始化和状态刷新。 */
#include "oled.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SERVO_CLOSED_PULSE  1000U
#define SERVO_OPEN_PULSE    2000U
#define SERVO_OLED_SETTLE_MS  300U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t uart1_rx_byte;

typedef enum
{
  APP_PAGE_HOME = 0,
  APP_PAGE_MOTOR = 1,
  APP_PAGE_SERVO = 2
} AppPage;

/*
 * OLED 刷新状态缓存。
 * motor_display_state 保存当前应该显示的电机状态；motor_display_pending 表示主循环需要刷新屏幕。
 * 这两个变量会在串口接收中断和主循环之间共享，所以使用 volatile 防止编译器优化掉读取。
 */
static volatile uint8_t motor_display_state = (uint8_t)OLED_MOTOR_STOP;
static volatile uint8_t motor_display_pending = 0U;
static volatile uint8_t app_page = (uint8_t)APP_PAGE_HOME;
static volatile uint8_t home_selection = (uint8_t)OLED_HOME_MOTOR;
static volatile uint8_t home_selection_old = (uint8_t)OLED_HOME_MOTOR;
static volatile uint8_t home_transition_pending = 0U;
static volatile uint8_t page_redraw_pending = 0U;
static volatile uint8_t servo_door_state = (uint8_t)OLED_DOOR_CLOSED;
static volatile uint8_t servo_display_pending = 0U;
static volatile uint32_t servo_display_due_tick = 0U;
static char uart_cmd_buffer[8];
static uint8_t uart_cmd_len = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Motor_SetSpeed(uint16_t speed);
static void Motor_HandleCommand(uint8_t command);
static void Servo_SetDoor(uint8_t open);
static uint8_t App_StringEquals(const char *a, const char *b);
static uint8_t UART1_CommandIsPrefix(const char *command, uint8_t length);
static void App_HandleTextCommand(const char *command);
static void UART1_ProcessRxByte(uint8_t byte);
static void UART1_StartReceive(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief  设置电机 PWM 占空比，并同步记录 OLED 需要显示的档位。
 * @param  speed PWM 比较值，当前约定为 0、300、600、999 四档。
 * @note   这个函数会被串口接收中断调用，所以这里只改 PWM 和置刷新标志，不直接进行 SPI 刷屏。
 */
void Motor_SetSpeed(uint16_t speed)
{
  uint8_t state;

  /* PWM 最大值按当前 TIM2 周期限制为 999，防止外部调用传入过大值。 */
  if (speed > 999U)
  {
    speed = 999U;
  }

  /* 修改 TIM2_CH1 的比较值，实际改变电机 PWM 输出占空比。 */
  __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, speed);

  /* 将 PWM 数值映射成 OLED 页面上的状态：0=停止，300=一档，600=二档，999=三档。 */
  if (speed == 0U)
  {
    state = (uint8_t)OLED_MOTOR_STOP;
  }
  else if (speed <= 300U)
  {
    state = (uint8_t)OLED_MOTOR_GEAR1;
  }
  else if (speed <= 600U)
  {
    state = (uint8_t)OLED_MOTOR_GEAR2;
  }
  else
  {
    state = (uint8_t)OLED_MOTOR_GEAR3;
  }

  /*
   * 只记录状态并置位刷新请求。
   * 真正的 OLED_UpdateMotorState() 放在 while(1) 中执行，避免在 UART 中断里长时间阻塞 SPI。
   */
  motor_display_state = state;
  motor_display_pending = 1U;
}

static void Motor_HandleCommand(uint8_t command)
{
  /* 上位机发送 ASCII 字符 '0'~'3'，分别对应停止、一档、二档、三档 PWM。 */
  switch (command)
  {
    case '0':
      Motor_SetSpeed(0);
      break;
    case '1':
      Motor_SetSpeed(300);
      break;
    case '2':
      Motor_SetSpeed(600);
      break;
    case '3':
      Motor_SetSpeed(999);
      break;
    default:
      break;
  }
}

static void Servo_SetDoor(uint8_t open)
{
  __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_1, (open != 0U) ? SERVO_OPEN_PULSE : SERVO_CLOSED_PULSE);
  servo_door_state = (open != 0U) ? (uint8_t)OLED_DOOR_OPEN : (uint8_t)OLED_DOOR_CLOSED;
  servo_display_due_tick = HAL_GetTick() + SERVO_OLED_SETTLE_MS;
  servo_display_pending = 1U;
}

static uint8_t App_StringEquals(const char *a, const char *b)
{
  while ((*a != '\0') && (*b != '\0'))
  {
    if (*a != *b)
    {
      return 0U;
    }
    a++;
    b++;
  }

  return ((*a == '\0') && (*b == '\0')) ? 1U : 0U;
}

static uint8_t UART1_CommandIsPrefix(const char *command, uint8_t length)
{
  static const char *known_commands[] = {"left", "right", "ok", "exit", "open", "close", "switch", "like", "dislike", "palm", "fist"};
  uint8_t i;
  uint8_t j;
  uint8_t matched;

  for (i = 0U; i < (uint8_t)(sizeof(known_commands) / sizeof(known_commands[0])); i++)
  {
    matched = 1U;
    for (j = 0U; j < length; j++)
    {
      if ((known_commands[i][j] == '\0') || (known_commands[i][j] != command[j]))
      {
        matched = 0U;
        break;
      }
    }

    if (matched != 0U)
    {
      return 1U;
    }
  }

  return 0U;
}

static void App_HandleTextCommand(const char *command)
{
  uint8_t old_selection;

  if ((app_page != (uint8_t)APP_PAGE_HOME) &&
      ((App_StringEquals(command, "exit") != 0U) || (App_StringEquals(command, "dislike") != 0U)))
  {
    app_page = (uint8_t)APP_PAGE_HOME;
    page_redraw_pending = 1U;
    return;
  }

  if (app_page == (uint8_t)APP_PAGE_HOME)
  {
    if ((App_StringEquals(command, "left") != 0U) ||
        (App_StringEquals(command, "right") != 0U) ||
        (App_StringEquals(command, "switch") != 0U))
    {
      old_selection = home_selection;
      if (App_StringEquals(command, "switch") != 0U)
      {
        home_selection = (home_selection == (uint8_t)OLED_HOME_MOTOR) ? (uint8_t)OLED_HOME_SERVO : (uint8_t)OLED_HOME_MOTOR;
      }
      else
      {
        home_selection = (App_StringEquals(command, "left") != 0U) ? (uint8_t)OLED_HOME_MOTOR : (uint8_t)OLED_HOME_SERVO;
      }
      if (old_selection != home_selection)
      {
        home_selection_old = old_selection;
        home_transition_pending = 1U;
      }
      return;
    }

    if ((App_StringEquals(command, "ok") != 0U) || (App_StringEquals(command, "like") != 0U))
    {
      app_page = (home_selection == (uint8_t)OLED_HOME_MOTOR) ? (uint8_t)APP_PAGE_MOTOR : (uint8_t)APP_PAGE_SERVO;
      page_redraw_pending = 1U;
    }
    return;
  }

  if (app_page == (uint8_t)APP_PAGE_SERVO)
  {
    if ((App_StringEquals(command, "open") != 0U) || (App_StringEquals(command, "palm") != 0U))
    {
      Servo_SetDoor(1U);
    }
    else if ((App_StringEquals(command, "close") != 0U) || (App_StringEquals(command, "fist") != 0U))
    {
      Servo_SetDoor(0U);
    }
  }
}

static void UART1_ProcessRxByte(uint8_t byte)
{
  char ch;

  if ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'Z'))
  {
    byte = (uint8_t)(byte + ((uint8_t)'a' - (uint8_t)'A'));
  }

  if ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'5'))
  {
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';

    if ((app_page == (uint8_t)APP_PAGE_MOTOR) && (byte <= (uint8_t)'3'))
    {
      Motor_HandleCommand(byte);
    }
    else if ((app_page == (uint8_t)APP_PAGE_SERVO) && (byte == (uint8_t)'4'))
    {
      Servo_SetDoor(1U);
    }
    else if ((app_page == (uint8_t)APP_PAGE_SERVO) && (byte == (uint8_t)'5'))
    {
      Servo_SetDoor(0U);
    }
    return;
  }

  if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n') || (byte == (uint8_t)' '))
  {
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';
    return;
  }

  ch = (char)byte;
  if ((ch < 'a') || (ch > 'z'))
  {
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';
    return;
  }

  if (uart_cmd_len >= (uint8_t)(sizeof(uart_cmd_buffer) - 1U))
  {
    uart_cmd_len = 0U;
  }

  uart_cmd_buffer[uart_cmd_len] = ch;
  uart_cmd_len++;
  uart_cmd_buffer[uart_cmd_len] = '\0';

  if ((App_StringEquals(uart_cmd_buffer, "left") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "right") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "ok") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "exit") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "open") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "close") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "switch") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "like") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "dislike") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "palm") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "fist") != 0U))
  {
    App_HandleTextCommand(uart_cmd_buffer);
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';
  }
  else if (UART1_CommandIsPrefix(uart_cmd_buffer, uart_cmd_len) == 0U)
  {
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';
  }
}

static void UART1_StartReceive(void)
{
  /* 每次只接收 1 字节命令；接收完成后在回调函数里重新开启下一次接收。 */
  HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    /* 串口中断里只处理命令和更新 PWM，不在这里刷 OLED，保证中断尽快退出。 */
    UART1_ProcessRxByte(uart1_rx_byte);
    UART1_StartReceive();
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    /* 串口出错后重新打开接收，避免一次错误导致后续指令都收不到。 */
    UART1_StartReceive();
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  MX_TIM4_Init();
  /* USER CODE BEGIN 2 */

  /* 启动 TIM2_CH1 PWM 输出 */
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_1);

  /* 使能 TB6612 驱动板 */
  HAL_GPIO_WritePin(STBY_GPIO_Port, STBY_Pin, GPIO_PIN_SET);
  /* 固定正转方向：AIN1=1, AIN2=0 */
  HAL_GPIO_WritePin(AIN1_GPIO_Port, AIN1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(AIN2_GPIO_Port, AIN2_Pin, GPIO_PIN_RESET);

  /* SPI1 和 DC/RES/CS GPIO 初始化完成后，再初始化 OLED 并绘制默认停止页面。 */
  OLED_Init();
  OLED_PlayBootAnimation();
  OLED_ShowHomePage(OLED_HOME_MOTOR);

  /* 上电默认停止，随后等待 USART1 接收上位机发送的 0/1/2/3 指令。 */
  Motor_SetSpeed(0);
  Servo_SetDoor(0U);

  /* 默认停止页面已经手动绘制过，所以清掉 Motor_SetSpeed(0) 置位的刷新请求。 */
  motor_display_pending = 0U;
  servo_display_pending = 0U;
  home_transition_pending = 0U;
  page_redraw_pending = 0U;
  UART1_StartReceive();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint8_t servo_update_due;

    servo_update_due = ((servo_display_pending != 0U) && ((int32_t)(HAL_GetTick() - servo_display_due_tick) >= 0)) ? 1U : 0U;

    if ((home_transition_pending != 0U) || (page_redraw_pending != 0U) || (motor_display_pending != 0U) || (servo_update_due != 0U))
    {
      uint8_t page;
      uint8_t selection;
      uint8_t old_selection;
      uint8_t motor_state;
      uint8_t door_state;
      uint8_t transition_pending;
      uint8_t redraw_pending;
      uint8_t motor_pending;
      uint8_t servo_pending;

      /*
       * 关中断只保护这两个共享变量的读写，临界区很短。
       * SPI 刷屏放在开中断之后执行，避免影响串口接收和系统响应。
       */
      __disable_irq();
      page = app_page;
      selection = home_selection;
      old_selection = home_selection_old;
      motor_state = motor_display_state;
      door_state = servo_door_state;
      transition_pending = home_transition_pending;
      redraw_pending = page_redraw_pending;
      motor_pending = motor_display_pending;
      servo_pending = servo_update_due;
      home_transition_pending = 0U;
      page_redraw_pending = 0U;
      motor_display_pending = 0U;
      if (servo_pending != 0U)
      {
        servo_display_pending = 0U;
      }
      __enable_irq();

      /* 根据最新 PWM 档位刷新底部状态显示：停止/一档/二档/三档。 */
      if (transition_pending != 0U)
      {
        OLED_AnimateHomeSelection((OLED_HomeSelection)old_selection, (OLED_HomeSelection)selection);
      }

      if (redraw_pending != 0U)
      {
        if (page == (uint8_t)APP_PAGE_HOME)
        {
          OLED_ShowHomePage((OLED_HomeSelection)selection);
        }
        else if (page == (uint8_t)APP_PAGE_MOTOR)
        {
          OLED_ShowMotorPage((OLED_MotorState)motor_state);
        }
        else
        {
          OLED_ShowServoPage((OLED_DoorState)door_state);
        }
      }

      if ((motor_pending != 0U) && (page == (uint8_t)APP_PAGE_MOTOR))
      {
        OLED_UpdateMotorState((OLED_MotorState)motor_state);
      }

      if ((servo_pending != 0U) && (page == (uint8_t)APP_PAGE_SERVO))
      {
        OLED_UpdateServoState((OLED_DoorState)door_state);
      }
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
