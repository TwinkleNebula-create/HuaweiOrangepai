#include "at24c02.h"
#include "i2c.h"

#define AT24C02_A2             0U
#define AT24C02_A1             0U
#define AT24C02_A0             0U
#define AT24C02_I2C_ADDR       ((uint16_t)((0x50U | (AT24C02_A2 << 2) | (AT24C02_A1 << 1) | AT24C02_A0) << 1))
#define AT24C02_PAGE_SIZE      8U
#define AT24C02_I2C_TIMEOUT_MS 50U
#define AT24C02_WRITE_TRIALS   20U

uint8_t AT24C02_IsReady(void)
{
  return (HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_I2C_ADDR, AT24C02_WRITE_TRIALS, AT24C02_I2C_TIMEOUT_MS) == HAL_OK) ? 1U : 0U;
}

HAL_StatusTypeDef AT24C02_Read(uint8_t mem_addr, uint8_t *data, uint16_t len)
{
  if ((data == 0) || (len == 0U) || (((uint16_t)mem_addr + len) > AT24C02_SIZE_BYTES))
  {
    return HAL_ERROR;
  }

  return HAL_I2C_Mem_Read(&hi2c1,
                          AT24C02_I2C_ADDR,
                          mem_addr,
                          I2C_MEMADD_SIZE_8BIT,
                          data,
                          len,
                          AT24C02_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef AT24C02_Write(uint8_t mem_addr, const uint8_t *data, uint16_t len)
{
  HAL_StatusTypeDef status;
  uint16_t offset;
  uint16_t chunk;
  uint16_t page_left;

  if ((data == 0) || (len == 0U) || (((uint16_t)mem_addr + len) > AT24C02_SIZE_BYTES))
  {
    return HAL_ERROR;
  }

  offset = 0U;
  while (offset < len)
  {
    page_left = (uint16_t)(AT24C02_PAGE_SIZE - (((uint16_t)mem_addr + offset) % AT24C02_PAGE_SIZE));
    chunk = (uint16_t)(len - offset);
    if (chunk > page_left)
    {
      chunk = page_left;
    }

    status = HAL_I2C_Mem_Write(&hi2c1,
                               AT24C02_I2C_ADDR,
                               (uint16_t)(mem_addr + offset),
                               I2C_MEMADD_SIZE_8BIT,
                               (uint8_t *)&data[offset],
                               chunk,
                               AT24C02_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
      return status;
    }

    status = HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_I2C_ADDR, AT24C02_WRITE_TRIALS, AT24C02_I2C_TIMEOUT_MS);
    if (status != HAL_OK)
    {
      return status;
    }

    offset = (uint16_t)(offset + chunk);
  }

  return HAL_OK;
}
