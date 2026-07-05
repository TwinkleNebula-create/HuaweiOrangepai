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
#include "rfid_reader.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SERVO_CLOSED_PULSE  1000U
#define SERVO_OPEN_PULSE    2000U
#define SERVO_OLED_SETTLE_MS  300U
#define APP_UNLOCK_MESSAGE_MS  900U
#define APP_UNLOCK_CODE_LENGTH  4U
#define APP_MESSAGE_NONE        0U
#define APP_MESSAGE_UNLOCK_OK   1U
#define APP_MESSAGE_LOCK_FAIL   2U
#define APP_MESSAGE_ENROLL_OK   3U
#define APP_MESSAGE_MODIFY_OK   4U
#define APP_LOCK_ERROR_MS       1000U
#define APP_GESTURE_ENROLL_COUNTDOWN_MAX  3U
#define APP_GESTURE_ENROLL_STEP_MS        1000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t uart1_rx_byte;
static uint8_t uart2_rx_byte;

typedef enum
{
  APP_PAGE_HOME = 0,
  APP_PAGE_MOTOR = 1,
  APP_PAGE_SERVO = 2,
  APP_PAGE_LOCK = 3,
  APP_PAGE_RFID_ENROLL = 4,
  APP_PAGE_GESTURE_ENROLL = 5
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
static volatile uint8_t access_unlocked = 0U;
static volatile uint8_t access_unlock_pending = 0U;
static volatile uint8_t unlock_code_index = 0U;
static uint8_t unlock_input_code[APP_UNLOCK_CODE_LENGTH];
static volatile uint8_t lock_error_pending = 0U;
static volatile uint32_t lock_error_due_tick = 0U;
static volatile uint8_t access_message_pending = 0U;
static volatile uint8_t access_message_mode = APP_MESSAGE_NONE;
static volatile uint32_t access_message_due_tick = 0U;
static char uart_cmd_buffer[8];
static uint8_t uart_cmd_len = 0U;
static uint8_t unlock_code[APP_UNLOCK_CODE_LENGTH] = {'1', '2', '3', '4'};
static uint8_t gesture_enroll_code[APP_UNLOCK_CODE_LENGTH];
static volatile uint8_t gesture_enroll_index = 0U;
static volatile uint8_t gesture_enroll_recording = 0U;
static volatile uint8_t gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
static volatile uint32_t gesture_enroll_due_tick = 0U;

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
static void UART2_StartReceive(void);
static void App_EnterHomeAfterUnlock(void);
static void App_ReturnHome(void);
static void App_LockAndShow(void);
static void App_OpenHomeItem(uint8_t item);
static void App_SelectHomeItem(uint8_t item);
static void App_HandleUnlockByte(uint8_t byte);
static void App_HandleLockErrorTimer(void);
static void App_StartGestureEnroll(void);
static void App_StartGestureEnrollStep(void);
static void App_HandleGestureEnrollByte(uint8_t byte);
static void App_HandleGestureInvalid(void);
static void App_HandleGestureEnrollTimer(void);
static void App_HandleRfidEvents(void);
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
  static const char *known_commands[] = {"ok", "exit", "open", "close", "like", "dislike", "palm", "fist", "invalid"};
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
  if ((app_page == (uint8_t)APP_PAGE_HOME) &&
      ((App_StringEquals(command, "exit") != 0U) || (App_StringEquals(command, "dislike") != 0U)))
  {
    App_LockAndShow();
    return;
  }

  if ((app_page != (uint8_t)APP_PAGE_HOME) &&
      ((App_StringEquals(command, "exit") != 0U) || (App_StringEquals(command, "dislike") != 0U)))
  {
    App_ReturnHome();
    return;
  }

  if (app_page == (uint8_t)APP_PAGE_HOME)
  {
    if ((App_StringEquals(command, "ok") != 0U) || (App_StringEquals(command, "like") != 0U))
    {
      App_OpenHomeItem(home_selection);
    }
    return;
  }

  if (app_page == (uint8_t)APP_PAGE_GESTURE_ENROLL)
  {
    if ((App_StringEquals(command, "ok") != 0U) || (App_StringEquals(command, "like") != 0U))
    {
      App_StartGestureEnrollStep();
    }
    else if (App_StringEquals(command, "invalid") != 0U)
    {
      App_HandleGestureInvalid();
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

  if (access_unlocked == 0U)
  {
    App_HandleUnlockByte(byte);
    return;
  }

  if ((byte >= (uint8_t)'A') && (byte <= (uint8_t)'Z'))
  {
    byte = (uint8_t)(byte + ((uint8_t)'a' - (uint8_t)'A'));
  }

  if ((byte >= (uint8_t)'0') && (byte <= (uint8_t)'5'))
  {
    uart_cmd_len = 0U;
    uart_cmd_buffer[0] = '\0';

    if ((app_page == (uint8_t)APP_PAGE_HOME) && (byte >= (uint8_t)'1') && (byte <= (uint8_t)'4'))
    {
      App_SelectHomeItem((uint8_t)(byte - (uint8_t)'1'));
    }
    else if (app_page == (uint8_t)APP_PAGE_GESTURE_ENROLL)
    {
      App_HandleGestureEnrollByte(byte);
    }
    else if ((app_page == (uint8_t)APP_PAGE_MOTOR) && (byte <= (uint8_t)'3'))
    {
      Motor_HandleCommand(byte);
    }
    else if ((app_page == (uint8_t)APP_PAGE_SERVO) && (byte == (uint8_t)'5'))
    {
      Servo_SetDoor(1U);
    }
    else if ((app_page == (uint8_t)APP_PAGE_SERVO) && (byte == (uint8_t)'0'))
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

  if ((App_StringEquals(uart_cmd_buffer, "ok") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "exit") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "open") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "close") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "like") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "dislike") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "palm") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "fist") != 0U) ||
      (App_StringEquals(uart_cmd_buffer, "invalid") != 0U))
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

static void UART2_StartReceive(void)
{
  HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1);
}

static void App_EnterHomeAfterUnlock(void)
{
  access_unlocked = 1U;
  app_page = (uint8_t)APP_PAGE_HOME;
  home_selection = (uint8_t)OLED_HOME_MOTOR;
  home_selection_old = (uint8_t)OLED_HOME_MOTOR;
  home_transition_pending = 0U;
  page_redraw_pending = 1U;
}

static void App_ReturnHome(void)
{
  Motor_SetSpeed(0U);
  Servo_SetDoor(0U);
  gesture_enroll_index = 0U;
  gesture_enroll_recording = 0U;
  gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
  gesture_enroll_due_tick = 0U;
  app_page = (uint8_t)APP_PAGE_HOME;
  home_selection = (uint8_t)OLED_HOME_MOTOR;
  home_selection_old = (uint8_t)OLED_HOME_MOTOR;
  home_transition_pending = 0U;
  page_redraw_pending = 1U;
}

static void App_LockAndShow(void)
{
  Motor_SetSpeed(0U);
  Servo_SetDoor(0U);
  RFID_Lock();
  access_unlocked = 0U;
  access_unlock_pending = 0U;
  unlock_code_index = 0U;
  lock_error_pending = 0U;
  lock_error_due_tick = 0U;
  gesture_enroll_index = 0U;
  gesture_enroll_recording = 0U;
  gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
  gesture_enroll_due_tick = 0U;
  access_message_pending = 0U;
  access_message_mode = APP_MESSAGE_NONE;
  app_page = (uint8_t)APP_PAGE_LOCK;
  home_selection = (uint8_t)OLED_HOME_MOTOR;
  home_selection_old = (uint8_t)OLED_HOME_MOTOR;
  home_transition_pending = 0U;
  page_redraw_pending = 1U;
}

static void App_HandleUnlockByte(uint8_t byte)
{
  uint8_t i;
  uint8_t matched;

  if (lock_error_pending != 0U)
  {
    return;
  }

  if ((byte < (uint8_t)'0') || (byte > (uint8_t)'9'))
  {
    return;
  }

  if (unlock_code_index < APP_UNLOCK_CODE_LENGTH)
  {
    unlock_input_code[unlock_code_index] = byte;
    unlock_code_index++;
    OLED_ShowLockInputPage(unlock_input_code, unlock_code_index, 0U);
  }

  if (unlock_code_index >= APP_UNLOCK_CODE_LENGTH)
  {
    matched = 1U;
    for (i = 0U; i < APP_UNLOCK_CODE_LENGTH; i++)
    {
      if (unlock_input_code[i] != unlock_code[i])
      {
        matched = 0U;
        break;
      }
    }

    unlock_code_index = 0U;
    if (matched != 0U)
    {
      access_unlocked = 1U;
      access_unlock_pending = 1U;
    }
    else
    {
      lock_error_pending = 1U;
      lock_error_due_tick = HAL_GetTick() + APP_LOCK_ERROR_MS;
      OLED_ShowLockInputPage(unlock_input_code, APP_UNLOCK_CODE_LENGTH, 1U);
    }
  }
}

static void App_HandleLockErrorTimer(void)
{
  if ((lock_error_pending != 0U) && ((int32_t)(HAL_GetTick() - lock_error_due_tick) >= 0))
  {
    lock_error_pending = 0U;
    unlock_code_index = 0U;
    OLED_ShowLockInputPage(unlock_input_code, 0U, 0U);
  }
}

static void App_OpenHomeItem(uint8_t item)
{
  switch (item)
  {
    case OLED_HOME_MOTOR:
      app_page = (uint8_t)APP_PAGE_MOTOR;
      break;
    case OLED_HOME_SERVO:
      app_page = (uint8_t)APP_PAGE_SERVO;
      break;
    case OLED_HOME_RFID:
      RFID_StartEnroll();
      app_page = (uint8_t)APP_PAGE_RFID_ENROLL;
      break;
    case OLED_HOME_GESTURE:
      App_StartGestureEnroll();
      break;
    default:
      app_page = (uint8_t)APP_PAGE_HOME;
      break;
  }

  page_redraw_pending = 1U;
}

static void App_SelectHomeItem(uint8_t item)
{
  if (item >= (uint8_t)OLED_HOME_COUNT)
  {
    return;
  }

  home_selection = item;
  home_selection_old = item;
  home_transition_pending = 0U;
  page_redraw_pending = 1U;
}

static void App_StartGestureEnroll(void)
{
  gesture_enroll_index = 0U;
  gesture_enroll_recording = 0U;
  gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
  gesture_enroll_due_tick = 0U;
  app_page = (uint8_t)APP_PAGE_GESTURE_ENROLL;
}

static void App_StartGestureEnrollStep(void)
{
  if ((gesture_enroll_index >= APP_UNLOCK_CODE_LENGTH) || (gesture_enroll_recording != 0U))
  {
    return;
  }

  gesture_enroll_recording = 1U;
  gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
  gesture_enroll_due_tick = HAL_GetTick() + APP_GESTURE_ENROLL_STEP_MS;
  page_redraw_pending = 1U;
}

static void App_HandleGestureEnrollByte(uint8_t byte)
{
  uint8_t i;

  if ((gesture_enroll_recording == 0U) || (byte < (uint8_t)'0') || (byte > (uint8_t)'5'))
  {
    return;
  }

  if (gesture_enroll_index < APP_UNLOCK_CODE_LENGTH)
  {
    gesture_enroll_code[gesture_enroll_index] = byte;
    gesture_enroll_index++;
  }

  if (gesture_enroll_index >= APP_UNLOCK_CODE_LENGTH)
  {
    gesture_enroll_recording = 0U;
    gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
    gesture_enroll_due_tick = 0U;
    for (i = 0U; i < APP_UNLOCK_CODE_LENGTH; i++)
    {
      unlock_code[i] = gesture_enroll_code[i];
    }
    unlock_code_index = 0U;
    access_message_pending = 1U;
    access_message_mode = APP_MESSAGE_MODIFY_OK;
    access_message_due_tick = HAL_GetTick() + APP_UNLOCK_MESSAGE_MS;
    OLED_ShowModifySuccessPage();
  }
  else
  {
    gesture_enroll_recording = 0U;
    gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
    gesture_enroll_due_tick = 0U;
    page_redraw_pending = 1U;
  }
}

static void App_HandleGestureInvalid(void)
{
  if (app_page != (uint8_t)APP_PAGE_GESTURE_ENROLL)
  {
    return;
  }

  gesture_enroll_recording = 0U;
  gesture_enroll_countdown = APP_GESTURE_ENROLL_COUNTDOWN_MAX;
  gesture_enroll_due_tick = 0U;
  page_redraw_pending = 1U;
}

static void App_HandleGestureEnrollTimer(void)
{
  if ((app_page != (uint8_t)APP_PAGE_GESTURE_ENROLL) || (gesture_enroll_recording == 0U) || (access_message_pending != 0U))
  {
    return;
  }

  if ((int32_t)(HAL_GetTick() - gesture_enroll_due_tick) >= 0)
  {
    if (gesture_enroll_countdown > 1U)
    {
      gesture_enroll_countdown--;
      gesture_enroll_due_tick = HAL_GetTick() + APP_GESTURE_ENROLL_STEP_MS;
      page_redraw_pending = 1U;
    }
    else
    {
      App_HandleGestureInvalid();
    }
  }
}

static void App_HandleRfidEvents(void)
{
  RFID_Event event;
  uint8_t card_id[RFID_CARD_ID_LENGTH];
  uint8_t retry_count;
  uint8_t unlock_pending;

  __disable_irq();
  unlock_pending = access_unlock_pending;
  access_unlock_pending = 0U;
  __enable_irq();

  if (unlock_pending != 0U)
  {
    access_message_pending = 1U;
    access_message_mode = APP_MESSAGE_UNLOCK_OK;
    access_message_due_tick = HAL_GetTick() + APP_UNLOCK_MESSAGE_MS;
    OLED_ShowUnlockSuccessPage();
  }

  event = RFID_PollEvent(card_id, &retry_count);
  if (event == RFID_EVENT_AUTHORIZED)
  {
    if (access_unlocked == 0U)
    {
      access_unlocked = 1U;
      access_message_pending = 1U;
      access_message_mode = APP_MESSAGE_UNLOCK_OK;
      access_message_due_tick = HAL_GetTick() + APP_UNLOCK_MESSAGE_MS;
      OLED_ShowUnlockSuccessPage();
    }
  }
  else if (event == RFID_EVENT_DENIED)
  {
    if (access_unlocked == 0U)
    {
      access_message_pending = 1U;
      access_message_mode = APP_MESSAGE_LOCK_FAIL;
      access_message_due_tick = HAL_GetTick() + APP_UNLOCK_MESSAGE_MS;
      OLED_ShowUnlockDeniedPage(retry_count);
    }
  }
  else if (event == RFID_EVENT_ENROLLED)
  {
    access_message_pending = 1U;
    access_message_mode = APP_MESSAGE_ENROLL_OK;
    access_message_due_tick = HAL_GetTick() + APP_UNLOCK_MESSAGE_MS;
    OLED_ShowRfidEnrollSuccessPage();
  }

  if ((access_message_pending != 0U) && ((int32_t)(HAL_GetTick() - access_message_due_tick) >= 0))
  {
    access_message_pending = 0U;
    if (access_message_mode == APP_MESSAGE_UNLOCK_OK)
    {
      App_EnterHomeAfterUnlock();
    }
    else if ((access_message_mode == APP_MESSAGE_ENROLL_OK) || (access_message_mode == APP_MESSAGE_MODIFY_OK))
    {
      App_ReturnHome();
    }
    else
    {
      app_page = (uint8_t)APP_PAGE_LOCK;
      OLED_ShowLockPage();
    }
    access_message_mode = APP_MESSAGE_NONE;
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    /* 串口中断里只处理命令和更新 PWM，不在这里刷 OLED，保证中断尽快退出。 */
    UART1_ProcessRxByte(uart1_rx_byte);
    UART1_StartReceive();
  }
  else if (huart->Instance == USART2)
  {
    RFID_ProcessByte(uart2_rx_byte);
    UART2_StartReceive();
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    /* 串口出错后重新打开接收，避免一次错误导致后续指令都收不到。 */
    UART1_StartReceive();
  }
  else if (huart->Instance == USART2)
  {
    UART2_StartReceive();
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
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  RFID_Init();

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
  OLED_ShowLockPage();
  app_page = (uint8_t)APP_PAGE_LOCK;
  access_unlocked = 0U;

  /* 上电默认停止，随后等待 USART1 接收上位机发送的 0/1/2/3 指令。 */
  Motor_SetSpeed(0);
  Servo_SetDoor(0U);

  /* 默认停止页面已经手动绘制过，所以清掉 Motor_SetSpeed(0) 置位的刷新请求。 */
  motor_display_pending = 0U;
  servo_display_pending = 0U;
  home_transition_pending = 0U;
  page_redraw_pending = 0U;
  UART1_StartReceive();
  UART2_StartReceive();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint8_t servo_update_due;

    App_HandleRfidEvents();
    App_HandleLockErrorTimer();
    App_HandleGestureEnrollTimer();

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
        else if (page == (uint8_t)APP_PAGE_SERVO)
        {
          OLED_ShowServoPage((OLED_DoorState)door_state);
        }
        else if (page == (uint8_t)APP_PAGE_RFID_ENROLL)
        {
          OLED_ShowRfidEnrollPage();
        }
        else if (page == (uint8_t)APP_PAGE_GESTURE_ENROLL)
        {
          OLED_ShowGestureEnrollPage(gesture_enroll_index, gesture_enroll_countdown, gesture_enroll_recording, gesture_enroll_code);
        }
        else
        {
          OLED_ShowLockPage();
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
