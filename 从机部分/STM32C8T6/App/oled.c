#include "oled.h"
#include "spi.h"

/*
 * 128*128 SPI 屏幕底层驱动。
 * 当前驱动按常见 ST7735 类 1.44 寸 128*128 RGB 屏编写，使用 CubeMX 已生成的 SPI1。
 * 硬件连接关系：SCL->PA5(SPI1_SCK)，SDA->PA7(SPI1_MOSI)，DC->PB0，RES->PB1，CS->PB10。
 * BLK 背光脚当前没有在 CubeMX 中生成 GPIO，建议硬件上直接接 3.3V 常亮。
 */

/*
 * 部分 128*128 屏幕内部显存并不是从可视区 (0,0) 开始。
 * 如果烧录后画面整体偏移，可以只改这里两个偏移量，不需要改绘图坐标。
 */
#define OLED_X_OFFSET  2U
#define OLED_Y_OFFSET  3U

#define OLED_COLOR_DARK     0x0841U
#define OLED_COLOR_CARD     0x18E3U
#define OLED_COLOR_GRAY     0x8410U
#define OLED_COLOR_ORANGE   0xFD20U

/* ST7735 常用指令码，只保留本驱动实际用到的命令，方便之后查阅和维护。 */
#define OLED_CMD_SWRESET  0x01U
#define OLED_CMD_SLPOUT   0x11U
#define OLED_CMD_NORON    0x13U
#define OLED_CMD_DISPON   0x29U
#define OLED_CMD_CASET    0x2AU
#define OLED_CMD_RASET    0x2BU
#define OLED_CMD_RAMWR    0x2CU
#define OLED_CMD_MADCTL   0x36U
#define OLED_CMD_COLMOD   0x3AU

/*
 * 控制线宏：CS 负责片选，DC 负责区分命令/数据。
 * DC=0 时 SPI 发送的是屏幕控制命令；DC=1 时 SPI 发送的是命令参数或像素数据。
 */
#define OLED_CS_LOW()   HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET)
#define OLED_CS_HIGH()  HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET)
#define OLED_DC_CMD()   HAL_GPIO_WritePin(DC_GPIO_Port, DC_Pin, GPIO_PIN_RESET)
#define OLED_DC_DATA()  HAL_GPIO_WritePin(DC_GPIO_Port, DC_Pin, GPIO_PIN_SET)

/*
 * 本页面只需要显示少量汉字，所以不引入完整中文字库。
 * 枚举顺序必须和 oled_hanzi_16x16 数组顺序一致：手、势、电、机、驱、动、系、统、状态、停止、一二三档。
 */
typedef enum
{
  GLYPH_SHOU = 0,
  GLYPH_SHI,
  GLYPH_DIAN,
  GLYPH_JI,
  GLYPH_QU,
  GLYPH_DONG,
  GLYPH_XI,
  GLYPH_TONG,
  GLYPH_ZHUANG,
  GLYPH_TAI,
  GLYPH_TING,
  GLYPH_ZHI,
  GLYPH_YI,
  GLYPH_ER,
  GLYPH_SAN,
  GLYPH_DANG
} OLED_GlyphIndex;

typedef enum
{
  GLYPH16_SHOU = 0,
  GLYPH16_SHI,
  GLYPH16_JIA,
  GLYPH16_JU,
  GLYPH16_GUAN,
  GLYPH16_LI,
  GLYPH16_XI,
  GLYPH16_TONG,
  GLYPH16_DIAN,
  GLYPH16_JI,
  GLYPH16_DUO,
  GLYPH16_KAI,
  GLYPH16_GUAN2
} OLED_Glyph16Index;


#define OLED_GLYPH_WIDTH          24U
#define OLED_GLYPH_HEIGHT         24U
#define OLED_GLYPH_BYTES_PER_ROW  3U

#define OLED_GLYPH16_WIDTH          16U
#define OLED_GLYPH16_HEIGHT         16U
#define OLED_GLYPH16_BYTES_PER_ROW  2U

static const uint8_t oled_hanzi_24x24[][72] =
{
  { /* GLYPH_SHOU */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xE0U, 0x1FU, 0xFFU, 0xF0U, 0x1FU, 0xFFU, 0x80U,
    0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x0FU, 0xFFU, 0xF0U,
    0x1FU, 0xFFU, 0xF0U, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U,
    0x7FU, 0xFFU, 0xFCU, 0x7FU, 0xFFU, 0xFCU, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U,
    0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x38U, 0x00U, 0x00U, 0xF8U, 0x00U,
    0x00U, 0xF0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_SHI */
    0x00U, 0x00U, 0x00U, 0x06U, 0x06U, 0x00U, 0x06U, 0x06U, 0x00U, 0x06U, 0x06U, 0x00U,
    0x7FU, 0xBFU, 0xE0U, 0x3FU, 0x9FU, 0x60U, 0x06U, 0x06U, 0x60U, 0x07U, 0x9EU, 0x60U,
    0x7FU, 0x1EU, 0x60U, 0x76U, 0x0FU, 0x60U, 0x06U, 0x1AU, 0x6CU, 0x06U, 0x70U, 0x7CU,
    0x1EU, 0x60U, 0x3CU, 0x18U, 0x30U, 0x18U, 0x1FU, 0xFFU, 0xE0U, 0x3FU, 0xFFU, 0xE0U,
    0x00U, 0x60U, 0x60U, 0x00U, 0xE0U, 0x60U, 0x01U, 0xC0U, 0x60U, 0x07U, 0x80U, 0xE0U,
    0x3FU, 0x07U, 0xC0U, 0x38U, 0x07U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_DIAN */
    0x00U, 0x00U, 0x00U, 0x00U, 0x30U, 0x00U, 0x00U, 0x70U, 0x00U, 0x00U, 0x70U, 0x00U,
    0x00U, 0x70U, 0x00U, 0x3FU, 0xFFU, 0xE0U, 0x3FU, 0xFFU, 0xE0U, 0x38U, 0x70U, 0x60U,
    0x38U, 0x70U, 0x60U, 0x38U, 0x70U, 0x60U, 0x3FU, 0xFFU, 0xE0U, 0x3FU, 0xFFU, 0xE0U,
    0x38U, 0x70U, 0x60U, 0x38U, 0x70U, 0x60U, 0x38U, 0x70U, 0x60U, 0x3FU, 0xFFU, 0xE0U,
    0x3FU, 0xFFU, 0xE0U, 0x38U, 0x70U, 0x00U, 0x00U, 0x70U, 0x0CU, 0x00U, 0x70U, 0x0CU,
    0x00U, 0x3FU, 0xFCU, 0x00U, 0x3FU, 0xF8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_JI */
    0x00U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x0EU, 0x00U, 0x00U, 0x0EU, 0x1FU, 0xC0U,
    0x0EU, 0x1FU, 0xC0U, 0x0EU, 0x18U, 0xC0U, 0x7FU, 0xD8U, 0xC0U, 0x7FU, 0xD8U, 0xC0U,
    0x0EU, 0x18U, 0xC0U, 0x0EU, 0x18U, 0xC0U, 0x1FU, 0x18U, 0xC0U, 0x1FU, 0x98U, 0xC0U,
    0x1FU, 0xD8U, 0xC0U, 0x3EU, 0xD8U, 0xC0U, 0x3EU, 0x18U, 0xC0U, 0x6EU, 0x18U, 0xC0U,
    0x6EU, 0x30U, 0xC0U, 0x0EU, 0x30U, 0xC0U, 0x0EU, 0x70U, 0xCCU, 0x0EU, 0x60U, 0xECU,
    0x0EU, 0xE0U, 0xFCU, 0x0EU, 0x40U, 0x78U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_QU */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x9FU, 0xFCU, 0x61U, 0x98U, 0x08U,
    0x31U, 0x98U, 0x00U, 0x31U, 0x98U, 0x18U, 0x31U, 0x9BU, 0x38U, 0x31U, 0x9BU, 0x30U,
    0x31U, 0x99U, 0xB0U, 0x31U, 0x99U, 0xE0U, 0x31U, 0x98U, 0xE0U, 0x3FU, 0xD8U, 0xE0U,
    0x3FU, 0xD8U, 0xF0U, 0x00U, 0xD9U, 0xB8U, 0x00U, 0xDBU, 0x98U, 0x7EU, 0xDBU, 0x18U,
    0x7EU, 0xDAU, 0x00U, 0x00U, 0xD8U, 0x00U, 0x00U, 0xD8U, 0x00U, 0x07U, 0xDFU, 0xFCU,
    0x07U, 0x8FU, 0xFCU, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_DONG */
    0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x01U, 0x80U, 0x00U, 0x01U, 0x80U,
    0x3FU, 0xE1U, 0x80U, 0x3FU, 0xE1U, 0x80U, 0x00U, 0x1FU, 0xF8U, 0x00U, 0x1FU, 0xFCU,
    0x00U, 0x01U, 0x8CU, 0x7FU, 0xE1U, 0x8CU, 0x7FU, 0xE1U, 0x8CU, 0x0EU, 0x01U, 0x9CU,
    0x0CU, 0x03U, 0x1CU, 0x0CU, 0xC3U, 0x18U, 0x18U, 0xC3U, 0x18U, 0x18U, 0x63U, 0x18U,
    0x33U, 0xF7U, 0x18U, 0x7FU, 0xF6U, 0x18U, 0x3CU, 0x3EU, 0x18U, 0x00U, 0x1CU, 0x18U,
    0x00U, 0x38U, 0xF8U, 0x00U, 0x30U, 0xF0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_XI */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x60U, 0x00U, 0x3FU, 0xF0U, 0x1FU, 0xFFU, 0xC0U,
    0x1EU, 0x70U, 0x00U, 0x00U, 0x70U, 0x80U, 0x00U, 0xC1U, 0xC0U, 0x03U, 0x87U, 0x80U,
    0x0FU, 0xFEU, 0x00U, 0x07U, 0xFCU, 0x00U, 0x00U, 0x71U, 0xC0U, 0x01U, 0xC0U, 0xE0U,
    0x0FU, 0x8FU, 0xF0U, 0x1FU, 0xFFU, 0xF8U, 0x0CU, 0x18U, 0x18U, 0x00U, 0x18U, 0x00U,
    0x07U, 0x18U, 0xC0U, 0x0EU, 0x18U, 0xF0U, 0x1CU, 0x18U, 0x78U, 0x38U, 0x18U, 0x18U,
    0x30U, 0xF8U, 0x08U, 0x00U, 0x70U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_TONG */
    0x00U, 0x00U, 0x00U, 0x00U, 0x06U, 0x00U, 0x0EU, 0x07U, 0x00U, 0x0CU, 0x03U, 0x00U,
    0x1CU, 0x7FU, 0xF8U, 0x18U, 0x7FU, 0xFCU, 0x39U, 0x86U, 0x00U, 0x33U, 0x8EU, 0x00U,
    0x7FU, 0x0CU, 0x00U, 0x7EU, 0x18U, 0x60U, 0x0EU, 0x38U, 0x70U, 0x0CU, 0x7FU, 0xF8U,
    0x18U, 0x7FU, 0xF8U, 0x3FU, 0x8DU, 0xC0U, 0x3FU, 0x8DU, 0xC0U, 0x38U, 0x0DU, 0xC0U,
    0x00U, 0x0DU, 0xC0U, 0x01U, 0x9DU, 0xC0U, 0x1FU, 0x99U, 0xCCU, 0x7EU, 0x39U, 0xCCU,
    0x30U, 0x70U, 0xFCU, 0x00U, 0xE0U, 0xF8U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_ZHUANG */
    0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x03U, 0x07U, 0x00U, 0x03U, 0x07U, 0x60U,
    0x03U, 0x03U, 0x70U, 0x63U, 0x03U, 0x38U, 0x33U, 0x03U, 0x10U, 0x3BU, 0x03U, 0x00U,
    0x13U, 0x7FU, 0xFCU, 0x03U, 0x7FU, 0xFCU, 0x03U, 0x07U, 0x00U, 0x07U, 0x07U, 0x00U,
    0x0FU, 0x07U, 0x80U, 0x1FU, 0x07U, 0x80U, 0x3BU, 0x0FU, 0x80U, 0x73U, 0x0DU, 0xC0U,
    0x23U, 0x0CU, 0xC0U, 0x03U, 0x18U, 0xE0U, 0x03U, 0x38U, 0x70U, 0x03U, 0x70U, 0x38U,
    0x03U, 0xF0U, 0x1CU, 0x03U, 0x60U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_TAI */
    0x00U, 0x00U, 0x00U, 0x00U, 0x30U, 0x00U, 0x00U, 0x70U, 0x00U, 0x00U, 0x70U, 0x00U,
    0x3FU, 0xFFU, 0xF8U, 0x7FU, 0xFFU, 0xF8U, 0x00U, 0xE6U, 0x00U, 0x00U, 0xC6U, 0x00U,
    0x01U, 0x83U, 0x00U, 0x03U, 0xC3U, 0x80U, 0x07U, 0x71U, 0xC0U, 0x1EU, 0x30U, 0xF8U,
    0x7CU, 0x10U, 0x38U, 0x30U, 0x00U, 0x08U, 0x00U, 0x18U, 0x00U, 0x19U, 0x98U, 0x70U,
    0x19U, 0x9CU, 0x30U, 0x39U, 0x88U, 0x38U, 0x31U, 0x81U, 0xDCU, 0x31U, 0x81U, 0xC8U,
    0x61U, 0xFFU, 0x80U, 0x00U, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_TING */
    0x00U, 0x00U, 0x00U, 0x06U, 0x06U, 0x00U, 0x0EU, 0x07U, 0x00U, 0x0DU, 0xFFU, 0xFCU,
    0x0CU, 0xFFU, 0xFCU, 0x1CU, 0x00U, 0x00U, 0x1CU, 0x7FU, 0xF0U, 0x3CU, 0x7FU, 0xF0U,
    0x3CU, 0x60U, 0x30U, 0x7CU, 0x7FU, 0xF0U, 0x6CU, 0x7FU, 0xE0U, 0x0DU, 0xFFU, 0xFCU,
    0x0DU, 0xFFU, 0xFCU, 0x0DU, 0x80U, 0x0CU, 0x0DU, 0x70U, 0x64U, 0x0CU, 0x7FU, 0xF0U,
    0x0CU, 0x03U, 0x00U, 0x0CU, 0x03U, 0x00U, 0x0CU, 0x03U, 0x00U, 0x0CU, 0x03U, 0x00U,
    0x0CU, 0x1FU, 0x00U, 0x0CU, 0x0EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_ZHI */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x00U,
    0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U, 0x00U, 0x18U, 0x00U,
    0x06U, 0x18U, 0x00U, 0x06U, 0x1FU, 0xF0U, 0x06U, 0x1FU, 0xF0U, 0x06U, 0x18U, 0x00U,
    0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U,
    0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U, 0x06U, 0x18U, 0x00U,
    0x7FU, 0xFFU, 0xFCU, 0x7FU, 0xFFU, 0xFCU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_YI */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0xFFU, 0xFCU,
    0x7FU, 0xFFU, 0xFCU, 0x7FU, 0xFFU, 0xFCU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_ER */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x1FU, 0xFFU, 0xE0U, 0x1FU, 0xFFU, 0xE0U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x7FU, 0xFFU, 0xFCU, 0x7FU, 0xFFU, 0xFCU,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_SAN */
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1FU, 0xFFU, 0xF0U,
    0x1FU, 0xFFU, 0xF0U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x07U, 0xFFU, 0xC0U, 0x0FU, 0xFFU, 0xC0U,
    0x07U, 0xFFU, 0xC0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0xFFU, 0xFCU,
    0x7FU, 0xFFU, 0xFCU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH_DANG */
    0x00U, 0x00U, 0x00U, 0x04U, 0x03U, 0x00U, 0x0CU, 0x03U, 0x00U, 0x0CU, 0x63U, 0x18U,
    0x0CU, 0x73U, 0x38U, 0x0CU, 0x33U, 0x30U, 0x7FU, 0x3BU, 0x70U, 0x7FU, 0x03U, 0x20U,
    0x0CU, 0x03U, 0x00U, 0x1CU, 0x7FU, 0xF8U, 0x1FU, 0x7FU, 0xF8U, 0x3FU, 0x80U, 0x18U,
    0x3DU, 0x80U, 0x18U, 0x7DU, 0xBFU, 0xF8U, 0x6CU, 0x3FU, 0xF8U, 0x6CU, 0x00U, 0x18U,
    0x0CU, 0x00U, 0x18U, 0x0CU, 0x00U, 0x18U, 0x0CU, 0x7FU, 0xF8U, 0x0CU, 0x7FU, 0xF8U,
    0x0CU, 0x00U, 0x18U, 0x0CU, 0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  }
};

static const uint8_t oled_hanzi_16x16[][32] =
{
  { /* GLYPH16_SHOU */
    0x00U, 0x00U, 0x03U, 0xFCU, 0x1FU, 0x80U, 0x00U, 0x80U,
    0x00U, 0x80U, 0x3FU, 0xFCU, 0x00U, 0x80U, 0x00U, 0x80U,
    0x7FU, 0xFEU, 0x00U, 0x80U, 0x00U, 0x80U, 0x00U, 0x80U,
    0x00U, 0x80U, 0x03U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_SHI */
    0x08U, 0x40U, 0x08U, 0x40U, 0x7DU, 0xF8U, 0x08U, 0x48U,
    0x1DU, 0xC8U, 0x68U, 0xE8U, 0x09U, 0x8AU, 0x3BU, 0x0EU,
    0x21U, 0x04U, 0x3FU, 0xF8U, 0x03U, 0x08U, 0x02U, 0x08U,
    0x0CU, 0x08U, 0x78U, 0x70U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_JIA */
    0x01U, 0x80U, 0x01U, 0x80U, 0x7FU, 0xFEU, 0x60U, 0x06U,
    0x6FU, 0xF6U, 0x03U, 0x00U, 0x07U, 0x08U, 0x39U, 0x98U,
    0x63U, 0xF0U, 0x0CU, 0xD0U, 0x71U, 0x58U, 0x06U, 0x4CU,
    0x18U, 0x46U, 0x63U, 0x80U, 0x01U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_JU */
    0x3FU, 0xFCU, 0x30U, 0x0CU, 0x3FU, 0xFCU, 0x30U, 0xC0U,
    0x30U, 0xC0U, 0x3FU, 0xFEU, 0x30U, 0xC0U, 0x30U, 0xC0U,
    0x27U, 0xFCU, 0x24U, 0x0CU, 0x24U, 0x0CU, 0x67U, 0xFCU,
    0x44U, 0x0CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_GUAN */
    0x18U, 0x30U, 0x1FU, 0x3EU, 0x34U, 0x48U, 0x65U, 0x4CU,
    0x01U, 0x80U, 0x7FU, 0xFEU, 0x40U, 0x02U, 0x1FU, 0xF0U,
    0x18U, 0x10U, 0x1FU, 0xF0U, 0x18U, 0x00U, 0x1FU, 0xF8U,
    0x18U, 0x08U, 0x1FU, 0xF8U, 0x18U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_LI */
    0x01U, 0xFEU, 0x7DU, 0x26U, 0x11U, 0x26U, 0x11U, 0xFEU,
    0x11U, 0x26U, 0x7DU, 0x26U, 0x11U, 0xFEU, 0x10U, 0x20U,
    0x10U, 0x20U, 0x13U, 0xFEU, 0x1CU, 0x20U, 0x60U, 0x20U,
    0x03U, 0xFEU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_XI */
    0x00U, 0x1CU, 0x3FU, 0xE0U, 0x01U, 0x80U, 0x02U, 0x10U,
    0x0CU, 0x60U, 0x1FU, 0xC0U, 0x03U, 0x10U, 0x0CU, 0x0CU,
    0x3FU, 0xFEU, 0x00U, 0x80U, 0x08U, 0x90U, 0x10U, 0x8CU,
    0x20U, 0x86U, 0x43U, 0x80U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_TONG */
    0x10U, 0x60U, 0x10U, 0x20U, 0x33U, 0xFEU, 0x24U, 0x40U,
    0x4CU, 0x40U, 0x78U, 0x88U, 0x11U, 0x8CU, 0x11U, 0xFEU,
    0x24U, 0x50U, 0x78U, 0xD0U, 0x00U, 0xD0U, 0x04U, 0x90U,
    0x79U, 0x92U, 0x43U, 0x1EU, 0x02U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_DIAN */
    0x01U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x3FU, 0xF8U,
    0x21U, 0x08U, 0x21U, 0x08U, 0x3FU, 0xF8U, 0x21U, 0x08U,
    0x21U, 0x08U, 0x3FU, 0xF8U, 0x3FU, 0xF8U, 0x21U, 0x00U,
    0x01U, 0x02U, 0x01U, 0xFEU, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_JI */
    0x18U, 0x00U, 0x18U, 0xF8U, 0x18U, 0x98U, 0x7EU, 0x98U,
    0x18U, 0x98U, 0x18U, 0x98U, 0x3CU, 0x98U, 0x3CU, 0x98U,
    0x3EU, 0x98U, 0x58U, 0x98U, 0x59U, 0x98U, 0x19U, 0x18U,
    0x1BU, 0x1AU, 0x1AU, 0x0EU, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_DUO */
    0x18U, 0x30U, 0x18U, 0x10U, 0x3EU, 0x00U, 0x22U, 0xFEU,
    0x2AU, 0x82U, 0x2AU, 0x82U, 0x22U, 0x60U, 0x7EU, 0x64U,
    0x22U, 0x7CU, 0x2AU, 0x70U, 0x2AU, 0x60U, 0x62U, 0x60U,
    0x42U, 0x62U, 0x46U, 0x3EU, 0x40U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_KAI */
    0x3FU, 0xFCU, 0x06U, 0x20U, 0x06U, 0x20U, 0x06U, 0x20U,
    0x06U, 0x20U, 0x7FU, 0xFEU, 0x7FU, 0xFEU, 0x06U, 0x20U,
    0x04U, 0x20U, 0x04U, 0x20U, 0x0CU, 0x20U, 0x18U, 0x20U,
    0x70U, 0x20U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
  },
  { /* GLYPH16_GUAN2 */
    0x04U, 0x20U, 0x04U, 0x20U, 0x06U, 0x60U, 0x3FU, 0xFCU,
    0x3FU, 0xFCU, 0x01U, 0x80U, 0x01U, 0x80U, 0x01U, 0x80U,
    0x7FU, 0xFEU, 0x01U, 0x80U, 0x03U, 0x40U, 0x06U, 0x70U,
    0x0CU, 0x1CU, 0x70U, 0x0EU, 0x00U, 0x00U, 0x00U, 0x00U
  }
};

static void OLED_WriteCommand(uint8_t command);
static void OLED_WriteData(uint8_t data);
static void OLED_WriteDataBuffer(const uint8_t *data, uint16_t size);
static void OLED_Reset(void);
static void OLED_SetAddressWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
static void OLED_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
static void OLED_FillRectSigned(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
static void OLED_DrawGlyph24(uint16_t x, uint16_t y, OLED_GlyphIndex glyph, uint16_t color, uint16_t background);
static void OLED_DrawGlyph16(uint16_t x, uint16_t y, OLED_Glyph16Index glyph, uint16_t color, uint16_t background);
static void OLED_DrawGlyph16Scaled24(uint16_t x, uint16_t y, OLED_Glyph16Index glyph, uint16_t color, uint16_t background);
static void OLED_DrawText16(uint16_t x, uint16_t y, const OLED_Glyph16Index *text, uint8_t length, uint16_t color, uint16_t background);
static void OLED_DrawFrame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t thickness);
static void OLED_DrawFanIcon(uint16_t cx, uint16_t cy, uint8_t large, uint16_t color);
static void OLED_DrawDoorIcon(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t selected, uint8_t open);
static void OLED_DrawHomeFrame(OLED_HomeSelection selection, int8_t slide);

/* 发送 1 字节屏幕命令：拉低 CS，DC 置 0，通过 SPI1 发出命令码。 */
static void OLED_WriteCommand(uint8_t command)
{
  OLED_CS_LOW();
  OLED_DC_CMD();
  HAL_SPI_Transmit(&hspi1, &command, 1U, HAL_MAX_DELAY);
  OLED_CS_HIGH();
}

/* 发送 1 字节数据：DC 置 1，通常用于命令后的参数或少量像素数据。 */
static void OLED_WriteData(uint8_t data)
{
  OLED_CS_LOW();
  OLED_DC_DATA();
  HAL_SPI_Transmit(&hspi1, &data, 1U, HAL_MAX_DELAY);
  OLED_CS_HIGH();
}

/* 批量发送数据：用于连续写入地址参数或大量 RGB565 像素数据。 */
static void OLED_WriteDataBuffer(const uint8_t *data, uint16_t size)
{
  OLED_CS_LOW();
  OLED_DC_DATA();
  HAL_SPI_Transmit(&hspi1, (uint8_t *)data, size, HAL_MAX_DELAY);
  OLED_CS_HIGH();
}

/* 硬件复位屏幕控制器：RES 先拉低再拉高，延时等待控制器稳定。 */
static void OLED_Reset(void)
{
  HAL_GPIO_WritePin(RES_GPIO_Port, RES_Pin, GPIO_PIN_RESET);
  HAL_Delay(20U);
  HAL_GPIO_WritePin(RES_GPIO_Port, RES_Pin, GPIO_PIN_SET);
  HAL_Delay(120U);
}

/*
 * 屏幕初始化流程：复位、软件复位、退出睡眠、配置帧率/电源参数、设置扫描方向和 RGB565 颜色格式。
 * 如果你的屏幕出现颜色反、上下左右颠倒，可优先调整 MADCTL 写入的 0xC8。
 */
void OLED_Init(void)
{
  OLED_Reset();

  /* 软件复位，确保屏幕控制器从默认状态开始。 */
  OLED_WriteCommand(OLED_CMD_SWRESET);
  HAL_Delay(120U);

  /* 退出睡眠模式，之后才能正常写显存和点亮显示。 */
  OLED_WriteCommand(OLED_CMD_SLPOUT);
  HAL_Delay(120U);

  /* 以下 B1/B2/B3/B4/C0~C5 为 ST7735 常用帧率、电源和 VCOM 参数。 */
  OLED_WriteCommand(0xB1U);
  OLED_WriteData(0x01U);
  OLED_WriteData(0x2CU);
  OLED_WriteData(0x2DU);

  OLED_WriteCommand(0xB2U);
  OLED_WriteData(0x01U);
  OLED_WriteData(0x2CU);
  OLED_WriteData(0x2DU);

  OLED_WriteCommand(0xB3U);
  OLED_WriteData(0x01U);
  OLED_WriteData(0x2CU);
  OLED_WriteData(0x2DU);
  OLED_WriteData(0x01U);
  OLED_WriteData(0x2CU);
  OLED_WriteData(0x2DU);

  OLED_WriteCommand(0xB4U);
  OLED_WriteData(0x07U);

  OLED_WriteCommand(0xC0U);
  OLED_WriteData(0xA2U);
  OLED_WriteData(0x02U);
  OLED_WriteData(0x84U);

  OLED_WriteCommand(0xC1U);
  OLED_WriteData(0xC5U);

  OLED_WriteCommand(0xC2U);
  OLED_WriteData(0x0AU);
  OLED_WriteData(0x00U);

  OLED_WriteCommand(0xC3U);
  OLED_WriteData(0x8AU);
  OLED_WriteData(0x2AU);

  OLED_WriteCommand(0xC4U);
  OLED_WriteData(0x8AU);
  OLED_WriteData(0xEEU);

  OLED_WriteCommand(0xC5U);
  OLED_WriteData(0x0EU);

  /* 设置显存扫描方向和 RGB/BGR 顺序；画面方向不对时主要调这个参数。 */
  OLED_WriteCommand(OLED_CMD_MADCTL);
  OLED_WriteData(0xC8U);

  OLED_WriteCommand(0xE0U);
  OLED_WriteData(0x0FU);
  OLED_WriteData(0x1AU);
  OLED_WriteData(0x0FU);
  OLED_WriteData(0x18U);
  OLED_WriteData(0x2FU);
  OLED_WriteData(0x28U);
  OLED_WriteData(0x20U);
  OLED_WriteData(0x22U);
  OLED_WriteData(0x1FU);
  OLED_WriteData(0x1BU);
  OLED_WriteData(0x23U);
  OLED_WriteData(0x37U);
  OLED_WriteData(0x00U);
  OLED_WriteData(0x07U);
  OLED_WriteData(0x02U);
  OLED_WriteData(0x10U);

  OLED_WriteCommand(0xE1U);
  OLED_WriteData(0x0FU);
  OLED_WriteData(0x1BU);
  OLED_WriteData(0x0FU);
  OLED_WriteData(0x17U);
  OLED_WriteData(0x33U);
  OLED_WriteData(0x2CU);
  OLED_WriteData(0x29U);
  OLED_WriteData(0x2EU);
  OLED_WriteData(0x30U);
  OLED_WriteData(0x30U);
  OLED_WriteData(0x39U);
  OLED_WriteData(0x3FU);
  OLED_WriteData(0x00U);
  OLED_WriteData(0x07U);
  OLED_WriteData(0x03U);
  OLED_WriteData(0x10U);

  OLED_WriteCommand(0xF0U);
  OLED_WriteData(0x01U);

  OLED_WriteCommand(0xF6U);
  OLED_WriteData(0x00U);

  /* 设置像素格式为 16 bit RGB565：每个像素发送两个字节。 */
  OLED_WriteCommand(OLED_CMD_COLMOD);
  OLED_WriteData(0x05U);

  /* 打开正常显示模式并点亮显示。 */
  OLED_WriteCommand(OLED_CMD_NORON);
  HAL_Delay(10U);

  OLED_WriteCommand(OLED_CMD_DISPON);
  HAL_Delay(120U);

  OLED_Fill(OLED_COLOR_BLACK);
}

/*
 * 设置后续写显存的矩形窗口。
 * 屏幕控制器收到 CASET/RASET 后，RAMWR 写入的数据会依次填充这个矩形区域。
 */
static void OLED_SetAddressWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t x0;
  uint16_t x1;
  uint16_t y0;
  uint16_t y1;
  uint8_t data[4];

  x0 = x + OLED_X_OFFSET;
  x1 = x + w - 1U + OLED_X_OFFSET;
  y0 = y + OLED_Y_OFFSET;
  y1 = y + h - 1U + OLED_Y_OFFSET;

  OLED_WriteCommand(OLED_CMD_CASET);
  data[0] = (uint8_t)(x0 >> 8);
  data[1] = (uint8_t)x0;
  data[2] = (uint8_t)(x1 >> 8);
  data[3] = (uint8_t)x1;
  OLED_WriteDataBuffer(data, 4U);

  OLED_WriteCommand(OLED_CMD_RASET);
  data[0] = (uint8_t)(y0 >> 8);
  data[1] = (uint8_t)y0;
  data[2] = (uint8_t)(y1 >> 8);
  data[3] = (uint8_t)y1;
  OLED_WriteDataBuffer(data, 4U);

  OLED_WriteCommand(OLED_CMD_RAMWR);
}

/* 全屏填充，本质上是填充一个 128*128 的矩形。 */
void OLED_Fill(uint16_t color)
{
  OLED_FillRect(0U, 0U, OLED_WIDTH, OLED_HEIGHT, color);
}

/*
 * 填充矩形区域。
 * 先做边界裁剪，防止坐标超出 128*128；再用 64 字节小缓冲分块发送，避免占用太多 SRAM。
 */
static void OLED_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  uint8_t buffer[128];
  uint32_t pixel_count;
  uint16_t i;
  uint16_t chunk_pixels;
  uint16_t chunk_bytes;

  if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT) || (w == 0U) || (h == 0U))
  {
    /* 矩形完全无效时直接返回，避免向屏幕发送错误窗口。 */
    return;
  }

  /* 超出右边界或下边界时缩小宽高，只绘制屏幕内可见部分。 */
  if ((x + w) > OLED_WIDTH)
  {
    w = OLED_WIDTH - x;
  }

  if ((y + h) > OLED_HEIGHT)
  {
    h = OLED_HEIGHT - y;
  }

  /* RGB565 一个像素两个字节，先把缓冲区填成重复的目标颜色。 */
  for (i = 0U; i < sizeof(buffer); i += 2U)
  {
    buffer[i] = (uint8_t)(color >> 8);
    buffer[i + 1U] = (uint8_t)color;
  }

  OLED_SetAddressWindow(x, y, w, h);
  pixel_count = (uint32_t)w * (uint32_t)h;

  /* 每次最多发送 32 个像素，循环直到整个矩形填充完成。 */
  while (pixel_count > 0U)
  {
    chunk_pixels = (pixel_count > (sizeof(buffer) / 2U)) ? (uint16_t)(sizeof(buffer) / 2U) : (uint16_t)pixel_count;
    chunk_bytes = (uint16_t)(chunk_pixels * 2U);
    OLED_WriteDataBuffer(buffer, chunk_bytes);
    pixel_count -= chunk_pixels;
  }
}

static void OLED_FillRectSigned(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  if ((w <= 0) || (h <= 0))
  {
    return;
  }

  if (x < 0)
  {
    w = (int16_t)(w + x);
    x = 0;
  }

  if (y < 0)
  {
    h = (int16_t)(h + y);
    y = 0;
  }

  if ((x >= (int16_t)OLED_WIDTH) || (y >= (int16_t)OLED_HEIGHT) || (w <= 0) || (h <= 0))
  {
    return;
  }

  if ((x + w) > (int16_t)OLED_WIDTH)
  {
    w = (int16_t)OLED_WIDTH - x;
  }

  if ((y + h) > (int16_t)OLED_HEIGHT)
  {
    h = (int16_t)OLED_HEIGHT - y;
  }

  OLED_FillRect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

static void OLED_DrawGlyph24(uint16_t x, uint16_t y, OLED_GlyphIndex glyph, uint16_t color, uint16_t background)
{
  uint8_t row;
  uint8_t col;
  uint8_t line[OLED_GLYPH_WIDTH * 2U];
  const uint8_t *bitmap;
  uint16_t pixel_color;

  if ((x >= OLED_WIDTH) || (y >= OLED_HEIGHT))
  {
    return;
  }

  bitmap = oled_hanzi_24x24[(uint8_t)glyph];
  OLED_SetAddressWindow(x, y, OLED_GLYPH_WIDTH, OLED_GLYPH_HEIGHT);

  for (row = 0U; row < OLED_GLYPH_HEIGHT; row++)
  {
    for (col = 0U; col < OLED_GLYPH_WIDTH; col++)
    {
      if ((bitmap[(row * OLED_GLYPH_BYTES_PER_ROW) + (col / 8U)] & (0x80U >> (col % 8U))) != 0U)
      {
        pixel_color = color;
      }
      else
      {
        pixel_color = background;
      }

      line[col * 2U] = (uint8_t)(pixel_color >> 8);
      line[(col * 2U) + 1U] = (uint8_t)pixel_color;
    }

    OLED_WriteDataBuffer(line, (uint16_t)sizeof(line));
  }
}

static void OLED_DrawGlyph16(uint16_t x, uint16_t y, OLED_Glyph16Index glyph, uint16_t color, uint16_t background)
{
  uint8_t row;
  uint8_t col;
  uint8_t line[OLED_GLYPH16_WIDTH * 2U];
  const uint8_t *bitmap;
  uint16_t pixel_color;

  if (((x + OLED_GLYPH16_WIDTH) > OLED_WIDTH) || ((y + OLED_GLYPH16_HEIGHT) > OLED_HEIGHT))
  {
    return;
  }

  bitmap = oled_hanzi_16x16[(uint8_t)glyph];
  OLED_SetAddressWindow(x, y, OLED_GLYPH16_WIDTH, OLED_GLYPH16_HEIGHT);

  for (row = 0U; row < OLED_GLYPH16_HEIGHT; row++)
  {
    for (col = 0U; col < OLED_GLYPH16_WIDTH; col++)
    {
      if ((bitmap[(row * OLED_GLYPH16_BYTES_PER_ROW) + (col / 8U)] & (0x80U >> (col % 8U))) != 0U)
      {
        pixel_color = color;
      }
      else
      {
        pixel_color = background;
      }

      line[col * 2U] = (uint8_t)(pixel_color >> 8);
      line[(col * 2U) + 1U] = (uint8_t)pixel_color;
    }

    OLED_WriteDataBuffer(line, (uint16_t)sizeof(line));
  }
}

static void OLED_DrawGlyph16Scaled24(uint16_t x, uint16_t y, OLED_Glyph16Index glyph, uint16_t color, uint16_t background)
{
  uint8_t row;
  uint8_t col;
  uint8_t src_row;
  uint8_t src_col;
  uint8_t line[OLED_GLYPH_WIDTH * 2U];
  const uint8_t *bitmap;
  uint16_t pixel_color;

  if (((x + OLED_GLYPH_WIDTH) > OLED_WIDTH) || ((y + OLED_GLYPH_HEIGHT) > OLED_HEIGHT))
  {
    return;
  }

  bitmap = oled_hanzi_16x16[(uint8_t)glyph];
  OLED_SetAddressWindow(x, y, OLED_GLYPH_WIDTH, OLED_GLYPH_HEIGHT);

  for (row = 0U; row < OLED_GLYPH_HEIGHT; row++)
  {
    src_row = (uint8_t)(((uint16_t)row * OLED_GLYPH16_HEIGHT) / OLED_GLYPH_HEIGHT);

    for (col = 0U; col < OLED_GLYPH_WIDTH; col++)
    {
      src_col = (uint8_t)(((uint16_t)col * OLED_GLYPH16_WIDTH) / OLED_GLYPH_WIDTH);

      if ((bitmap[(src_row * OLED_GLYPH16_BYTES_PER_ROW) + (src_col / 8U)] & (0x80U >> (src_col % 8U))) != 0U)
      {
        pixel_color = color;
      }
      else
      {
        pixel_color = background;
      }

      line[col * 2U] = (uint8_t)(pixel_color >> 8);
      line[(col * 2U) + 1U] = (uint8_t)pixel_color;
    }

    OLED_WriteDataBuffer(line, (uint16_t)sizeof(line));
  }
}

static void OLED_DrawText16(uint16_t x, uint16_t y, const OLED_Glyph16Index *text, uint8_t length, uint16_t color, uint16_t background)
{
  uint8_t i;

  for (i = 0U; i < length; i++)
  {
    OLED_DrawGlyph16((uint16_t)(x + ((uint16_t)i * OLED_GLYPH16_WIDTH)), y, text[i], color, background);
  }
}

static void OLED_DrawFrame(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t thickness)
{
  int16_t t;

  t = (int16_t)thickness;
  OLED_FillRectSigned(x, y, w, t, color);
  OLED_FillRectSigned(x, (int16_t)(y + h - t), w, t, color);
  OLED_FillRectSigned(x, y, t, h, color);
  OLED_FillRectSigned((int16_t)(x + w - t), y, t, h, color);
}

static void OLED_DrawFanIcon(uint16_t cx, uint16_t cy, uint8_t large, uint16_t color)
{
  int16_t r;

  r = (large != 0U) ? 18 : 14;
  OLED_DrawFrame((int16_t)cx - r, (int16_t)cy - r, (int16_t)(r * 2), (int16_t)(r * 2), color, 1U);
  OLED_FillRectSigned((int16_t)cx - 2, (int16_t)cy - r + 4, 5, (int16_t)r - 7, color);
  OLED_FillRectSigned((int16_t)cx + 3, (int16_t)cy - 2, (int16_t)r - 5, 5, color);
  OLED_FillRectSigned((int16_t)cx - r + 5, (int16_t)cy + 3, (int16_t)r - 6, 5, color);
  OLED_FillRectSigned((int16_t)cx - 3, (int16_t)cy - 3, 7, 7, OLED_COLOR_WHITE);
}

static void OLED_DrawDoorIcon(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t selected, uint8_t open)
{
  uint16_t frame_color;
  uint16_t door_color;

  frame_color = (selected != 0U) ? OLED_COLOR_YELLOW : OLED_COLOR_CYAN;
  door_color = (open != 0U) ? OLED_COLOR_GREEN : OLED_COLOR_ORANGE;
  OLED_DrawFrame((int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, frame_color, (selected != 0U) ? 2U : 1U);

  if (open != 0U)
  {
    OLED_FillRect((uint16_t)(x + 4U), (uint16_t)(y + 4U), (uint16_t)(w - 12U), (uint16_t)(h - 8U), OLED_COLOR_DARK);
    OLED_FillRect((uint16_t)(x + (w / 2U)), (uint16_t)(y + 5U), (uint16_t)(w / 3U), (uint16_t)(h - 10U), door_color);
    OLED_FillRect((uint16_t)(x + (w / 2U) + 4U), (uint16_t)(y + (h / 2U)), 3U, 3U, OLED_COLOR_WHITE);
  }
  else
  {
    OLED_FillRect((uint16_t)(x + 4U), (uint16_t)(y + 4U), (uint16_t)(w - 8U), (uint16_t)(h - 8U), door_color);
    OLED_FillRect((uint16_t)(x + w - 11U), (uint16_t)(y + (h / 2U)), 3U, 3U, OLED_COLOR_WHITE);
  }
}

static void OLED_DrawHomeFrame(OLED_HomeSelection selection, int8_t slide)
{
  static const OLED_Glyph16Index title[] = {GLYPH16_SHOU, GLYPH16_SHI, GLYPH16_JIA, GLYPH16_JU, GLYPH16_GUAN, GLYPH16_LI, GLYPH16_XI, GLYPH16_TONG};
  static const OLED_Glyph16Index motor_label[] = {GLYPH16_DIAN, GLYPH16_JI};
  static const OLED_Glyph16Index servo_label[] = {GLYPH16_DUO, GLYPH16_JI};
  uint8_t motor_selected;
  int16_t motor_x;
  int16_t motor_y;
  int16_t motor_w;
  int16_t motor_h;
  int16_t servo_x;
  int16_t servo_y;
  int16_t servo_w;
  int16_t servo_h;

  motor_selected = (selection == OLED_HOME_MOTOR) ? 1U : 0U;

  OLED_FillRect(0U, 29U, OLED_WIDTH, 82U, OLED_COLOR_BLACK);
  OLED_DrawText16(0U, 4U, title, 8U, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_FillRect(8U, 24U, 112U, 2U, OLED_COLOR_BLUE);
  OLED_FillRect(18U, 27U, 92U, 1U, OLED_COLOR_CYAN);

  if (motor_selected != 0U)
  {
    motor_x = (int16_t)(4 + slide);
    motor_y = 32;
    motor_w = 56;
    motor_h = 74;
    servo_x = (int16_t)(74 + slide);
    servo_y = 39;
    servo_w = 48;
    servo_h = 60;
  }
  else
  {
    motor_x = (int16_t)(6 + slide);
    motor_y = 39;
    motor_w = 48;
    motor_h = 60;
    servo_x = (int16_t)(68 + slide);
    servo_y = 32;
    servo_w = 56;
    servo_h = 74;
  }

  OLED_FillRectSigned(motor_x, motor_y, motor_w, motor_h, motor_selected ? OLED_COLOR_CARD : OLED_COLOR_DARK);
  OLED_DrawFrame(motor_x, motor_y, motor_w, motor_h, motor_selected ? OLED_COLOR_YELLOW : OLED_COLOR_GRAY, motor_selected ? 2U : 1U);
  OLED_DrawFanIcon((uint16_t)(motor_x + (motor_w / 2)), (uint16_t)(motor_y + 25), motor_selected, motor_selected ? OLED_COLOR_YELLOW : OLED_COLOR_CYAN);
  OLED_DrawText16((uint16_t)(motor_x + ((motor_w - 32) / 2)), (uint16_t)(motor_y + motor_h - 24), motor_label, 2U, motor_selected ? OLED_COLOR_WHITE : OLED_COLOR_GRAY, motor_selected ? OLED_COLOR_CARD : OLED_COLOR_DARK);

  OLED_FillRectSigned(servo_x, servo_y, servo_w, servo_h, (motor_selected == 0U) ? OLED_COLOR_CARD : OLED_COLOR_DARK);
  OLED_DrawFrame(servo_x, servo_y, servo_w, servo_h, (motor_selected == 0U) ? OLED_COLOR_YELLOW : OLED_COLOR_GRAY, (motor_selected == 0U) ? 2U : 1U);
  OLED_DrawDoorIcon((uint16_t)(servo_x + ((servo_w - 28) / 2)), (uint16_t)(servo_y + 11), 28U, 34U, (motor_selected == 0U), 0U);
  OLED_DrawText16((uint16_t)(servo_x + ((servo_w - 32) / 2)), (uint16_t)(servo_y + servo_h - 24), servo_label, 2U, (motor_selected == 0U) ? OLED_COLOR_WHITE : OLED_COLOR_GRAY, (motor_selected == 0U) ? OLED_COLOR_CARD : OLED_COLOR_DARK);
}

void OLED_PlayBootAnimation(void)
{
  static const OLED_Glyph16Index title_top[] = {GLYPH16_SHOU, GLYPH16_SHI, GLYPH16_JIA, GLYPH16_JU};
  static const OLED_Glyph16Index title_bottom[] = {GLYPH16_GUAN, GLYPH16_LI, GLYPH16_XI, GLYPH16_TONG};
  uint8_t frame;
  uint16_t bar_width;

  for (frame = 0U; frame < 6U; frame++)
  {
    OLED_Fill(OLED_COLOR_BLACK);
    OLED_DrawText16(32U, 12U, title_top, 4U, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
    OLED_DrawText16(32U, 32U, title_bottom, 4U, OLED_COLOR_CYAN, OLED_COLOR_BLACK);
    OLED_DrawFanIcon(42U, 78U, (frame & 1U), (frame & 1U) ? OLED_COLOR_YELLOW : OLED_COLOR_CYAN);
    OLED_DrawDoorIcon(78U, 58U, 28U, 40U, 1U, (frame > 2U) ? 1U : 0U);
    OLED_FillRect(20U, 113U, 88U, 5U, OLED_COLOR_DARK);
    bar_width = (uint16_t)(((uint16_t)(frame + 1U) * 88U) / 6U);
    OLED_FillRect(20U, 113U, bar_width, 5U, OLED_COLOR_GREEN);
    HAL_Delay(80U);
  }
}

void OLED_ShowHomePage(OLED_HomeSelection selection)
{
  OLED_Fill(OLED_COLOR_BLACK);
  OLED_DrawHomeFrame(selection, 0);
}

void OLED_AnimateHomeSelection(OLED_HomeSelection from, OLED_HomeSelection to)
{
  int8_t direction;
  int8_t slide;
  uint8_t step;

  if (from == to)
  {
    OLED_ShowHomePage(to);
    return;
  }

  direction = (to == OLED_HOME_SERVO) ? -1 : 1;

  for (step = 0U; step <= 3U; step++)
  {
    slide = (int8_t)(direction * (int8_t)(step * 2U));
    OLED_DrawHomeFrame(from, slide);
    HAL_Delay(16U);
  }

  for (step = 3U; step > 0U; step--)
  {
    slide = (int8_t)((-direction) * (int8_t)(step * 2U));
    OLED_DrawHomeFrame(to, slide);
    HAL_Delay(16U);
  }

  OLED_DrawHomeFrame(to, 0);
}

/*
 * 绘制完整的电机状态页面。
 * 页面布局：上方两行 32x32 大字标题，中间蓝色分隔线，下方显示“状态”和当前电机档位。
 */
void OLED_ShowMotorPage(OLED_MotorState state)
{
  OLED_Fill(OLED_COLOR_BLACK);

  /* 标题第一行：手势电机。 */
  OLED_DrawGlyph24(16U, 6U, GLYPH_SHOU, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(40U, 6U, GLYPH_SHI, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(64U, 6U, GLYPH_DIAN, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(88U, 6U, GLYPH_JI, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);

  /* 标题第二行：驱动系统。 */
  OLED_DrawGlyph24(16U, 34U, GLYPH_QU, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(40U, 34U, GLYPH_DONG, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(64U, 34U, GLYPH_XI, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(88U, 34U, GLYPH_TONG, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);

  /* 状态区域标题：蓝色分隔线 + “状态”两个小字。 */
  OLED_FillRect(8U, 64U, 112U, 2U, OLED_COLOR_BLUE);
  OLED_DrawGlyph24(28U, 68U, GLYPH_ZHUANG, OLED_COLOR_CYAN, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(52U, 68U, GLYPH_TAI, OLED_COLOR_CYAN, OLED_COLOR_BLACK);

  OLED_UpdateMotorState(state);
}

/*
 * 只刷新底部状态区域。
 * 主程序每次 PWM 档位变化后调用此函数，实现 0=停止，1=一档，2=二档，3=三档 的显示映射。
 */
void OLED_UpdateMotorState(OLED_MotorState state)
{
  uint16_t color;
  OLED_GlyphIndex first;
  OLED_GlyphIndex second;

  color = OLED_COLOR_GREEN;
  first = GLYPH_TING;
  second = GLYPH_ZHI;

  /* 根据电机状态选择要显示的两个汉字；停止用红色，运行档位用绿色。 */
  switch (state)
  {
    case OLED_MOTOR_GEAR1:
      first = GLYPH_YI;
      second = GLYPH_DANG;
      break;
    case OLED_MOTOR_GEAR2:
      first = GLYPH_ER;
      second = GLYPH_DANG;
      break;
    case OLED_MOTOR_GEAR3:
      first = GLYPH_SAN;
      second = GLYPH_DANG;
      break;
    case OLED_MOTOR_STOP:
    default:
      color = OLED_COLOR_RED;
      first = GLYPH_TING;
      second = GLYPH_ZHI;
      break;
  }

  /* 先擦除旧状态，再绘制新状态，避免档位文字叠在一起。 */
  OLED_FillRect(0U, 96U, OLED_WIDTH, 32U, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(40U, 98U, first, color, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(64U, 98U, second, color, OLED_COLOR_BLACK);
}

void OLED_ShowServoPage(OLED_DoorState state)
{
  OLED_Fill(OLED_COLOR_BLACK);

  OLED_DrawGlyph16Scaled24(2U, 6U, GLYPH16_DUO, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph16Scaled24(22U, 6U, GLYPH16_JI, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(42U, 6U, GLYPH_QU, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(63U, 6U, GLYPH_DONG, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(84U, 6U, GLYPH_XI, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(104U, 6U, GLYPH_TONG, OLED_COLOR_YELLOW, OLED_COLOR_BLACK);
  OLED_FillRect(8U, 36U, 112U, 2U, OLED_COLOR_BLUE);
  OLED_DrawGlyph24(40U, 42U, GLYPH_ZHUANG, OLED_COLOR_CYAN, OLED_COLOR_BLACK);
  OLED_DrawGlyph24(64U, 42U, GLYPH_TAI, OLED_COLOR_CYAN, OLED_COLOR_BLACK);

  OLED_UpdateServoState(state);
}

void OLED_UpdateServoState(OLED_DoorState state)
{
  uint16_t color;
  OLED_Glyph16Index glyph;

  color = (state == OLED_DOOR_OPEN) ? OLED_COLOR_GREEN : OLED_COLOR_RED;
  glyph = (state == OLED_DOOR_OPEN) ? GLYPH16_KAI : GLYPH16_GUAN2;
  OLED_FillRect(0U, 70U, OLED_WIDTH, 58U, OLED_COLOR_BLACK);
  OLED_DrawDoorIcon(46U, 70U, 36U, 28U, 1U, (state == OLED_DOOR_OPEN) ? 1U : 0U);
  OLED_DrawGlyph16(56U, 106U, glyph, color, OLED_COLOR_BLACK);
}
