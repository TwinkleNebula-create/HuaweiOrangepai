#ifndef __RFID_READER_H__
#define __RFID_READER_H__

#include "main.h"
#include <stdint.h>

#define RFID_CARD_ID_LENGTH  4U

typedef enum
{
  RFID_EVENT_NONE = 0,
  RFID_EVENT_AUTHORIZED = 1,
  RFID_EVENT_DENIED = 2,
  RFID_EVENT_ENROLLED = 3
} RFID_Event;

void RFID_Init(void);
void RFID_ProcessByte(uint8_t byte);
RFID_Event RFID_PollEvent(uint8_t card_id[RFID_CARD_ID_LENGTH], uint8_t *retry_count);
void RFID_StartEnroll(void);
uint8_t RFID_HasCard(void);
uint8_t RFID_IsUnlocked(void);
void RFID_Lock(void);
void RFID_SetStoredCard(const uint8_t card_id[RFID_CARD_ID_LENGTH]);
uint8_t RFID_GetStoredCard(uint8_t card_id[RFID_CARD_ID_LENGTH]);

#endif /* __RFID_READER_H__ */
