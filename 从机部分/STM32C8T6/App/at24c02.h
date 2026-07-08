#ifndef __AT24C02_H__
#define __AT24C02_H__

#include "main.h"
#include <stdint.h>

#define AT24C02_SIZE_BYTES  256U

HAL_StatusTypeDef AT24C02_Read(uint8_t mem_addr, uint8_t *data, uint16_t len);
HAL_StatusTypeDef AT24C02_Write(uint8_t mem_addr, const uint8_t *data, uint16_t len);
uint8_t AT24C02_IsReady(void);

#endif /* __AT24C02_H__ */
