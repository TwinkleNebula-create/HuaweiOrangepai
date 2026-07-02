#ifndef __OLED_H__
#define __OLED_H__

#include "main.h"
#include <stdint.h>

/* 屏幕物理分辨率：用户当前使用的是 128*128 的 SPI 彩屏/OLED 模块。 */
#define OLED_WIDTH   128U
#define OLED_HEIGHT  128U

/* RGB565 颜色定义：16 bit 颜色，高 5 位红色，中 6 位绿色，低 5 位蓝色。 */
#define OLED_COLOR_BLACK   0x0000U
#define OLED_COLOR_WHITE   0xFFFFU
#define OLED_COLOR_RED     0xF800U
#define OLED_COLOR_GREEN   0x07E0U
#define OLED_COLOR_BLUE    0x001FU
#define OLED_COLOR_CYAN    0x07FFU
#define OLED_COLOR_YELLOW  0xFFE0U

/*
 * OLED 页面显示的电机状态枚举。
 * 这里和串口指令保持一一对应：0=停止，1=一档，2=二档，3=三档。
 */
typedef enum
{
  OLED_MOTOR_STOP = 0,
  OLED_MOTOR_GEAR1 = 1,
  OLED_MOTOR_GEAR2 = 2,
  OLED_MOTOR_GEAR3 = 3
} OLED_MotorState;

typedef enum
{
  OLED_HOME_MOTOR = 0,
  OLED_HOME_SERVO = 1
} OLED_HomeSelection;

typedef enum
{
  OLED_DOOR_CLOSED = 0,
  OLED_DOOR_OPEN = 1
} OLED_DoorState;

/* 初始化屏幕控制器，完成复位、退出睡眠、颜色模式配置和清屏。 */
void OLED_Init(void);

/* 全屏填充指定 RGB565 颜色，主要用于初始化清屏。 */
void OLED_Fill(uint16_t color);

void OLED_PlayBootAnimation(void);
void OLED_ShowHomePage(OLED_HomeSelection selection);
void OLED_AnimateHomeSelection(OLED_HomeSelection from, OLED_HomeSelection to);

/* 绘制完整电机状态页面：标题、分隔线、状态标签和当前档位。 */
void OLED_ShowMotorPage(OLED_MotorState state);

/* 只刷新状态显示区域，不重画标题，适合 PWM 档位变化时调用。 */
void OLED_UpdateMotorState(OLED_MotorState state);

void OLED_ShowServoPage(OLED_DoorState state);
void OLED_UpdateServoState(OLED_DoorState state);

#endif /* __OLED_H__ */
