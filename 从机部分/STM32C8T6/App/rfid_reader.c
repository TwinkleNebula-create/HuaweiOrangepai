#include "rfid_reader.h"

/* 沁恒读卡器自动读卡帧：04 0C 02 30 00 04 00 + 4 字节卡号 */
static volatile uint8_t rfid_card[RFID_CARD_ID_LENGTH];
static volatile uint8_t rfid_card_saved;
static volatile uint8_t rfid_enroll_mode;

static volatile uint8_t rfid_index;
static volatile uint8_t rfid_temp[RFID_CARD_ID_LENGTH];
static volatile uint8_t rfid_last_card[RFID_CARD_ID_LENGTH];
static volatile uint8_t rfid_retry_count;
static volatile uint8_t rfid_unlocked;
static volatile RFID_Event rfid_event;

static uint8_t RFID_Check(void)
{
  uint8_t i;

  if (rfid_card_saved == 0U)
  {
    return 0U;
  }

  for (i = 0U; i < RFID_CARD_ID_LENGTH; i++)
  {
    if (rfid_temp[i] != rfid_card[i])
    {
      return 0U;
    }
  }

  return 1U;
}

static void RFID_SaveLastCard(void)
{
  uint8_t i;

  for (i = 0U; i < RFID_CARD_ID_LENGTH; i++)
  {
    rfid_last_card[i] = rfid_temp[i];
  }
}

static void RFID_HandleCard(void)
{
  uint8_t i;

  RFID_SaveLastCard();

  if (rfid_enroll_mode != 0U)
  {
    for (i = 0U; i < RFID_CARD_ID_LENGTH; i++)
    {
      rfid_card[i] = rfid_temp[i];
    }
    rfid_card_saved = 1U;
    rfid_enroll_mode = 0U;
    rfid_retry_count = 0U;
    rfid_event = RFID_EVENT_ENROLLED;
    return;
  }

  if (RFID_Check() != 0U)
  {
    rfid_unlocked = 1U;
    rfid_retry_count = 0U;
    rfid_event = RFID_EVENT_AUTHORIZED;
  }
  else
  {
    if (rfid_retry_count < 3U)
    {
      rfid_retry_count++;
    }
    rfid_event = RFID_EVENT_DENIED;
  }
}

void RFID_Init(void)
{
  uint8_t i;

  rfid_index = 0U;
  rfid_retry_count = 0U;
  rfid_unlocked = 0U;
  rfid_card_saved = 0U;
  rfid_enroll_mode = 0U;
  rfid_event = RFID_EVENT_NONE;

  for (i = 0U; i < RFID_CARD_ID_LENGTH; i++)
  {
    rfid_temp[i] = 0U;
    rfid_last_card[i] = 0U;
    rfid_card[i] = 0U;
  }
}

void RFID_ProcessByte(uint8_t byte)
{
  switch (rfid_index)
  {
    case 0:
      if (byte == 0x04U)
      {
        rfid_index++;
      }
      break;

    case 1:
      rfid_index = (byte == 0x0CU) ? (rfid_index + 1U) : 0U;
      break;

    case 2:
      rfid_index = (byte == 0x02U) ? (rfid_index + 1U) : 0U;
      break;

    case 3:
      rfid_index = (byte == 0x30U) ? (rfid_index + 1U) : 0U;
      break;

    case 4:
      rfid_index = (byte == 0x00U) ? (rfid_index + 1U) : 0U;
      break;

    case 5:
      rfid_index = (byte == 0x04U) ? (rfid_index + 1U) : 0U;
      break;

    case 6:
      rfid_index = (byte == 0x00U) ? (rfid_index + 1U) : 0U;
      break;

    case 7:
      rfid_temp[0] = byte;
      rfid_index++;
      break;

    case 8:
      rfid_temp[1] = byte;
      rfid_index++;
      break;

    case 9:
      rfid_temp[2] = byte;
      rfid_index++;
      break;

    case 10:
      rfid_temp[3] = byte;
      rfid_index = 0U;
      RFID_HandleCard();
      break;

    default:
      rfid_index = 0U;
      break;
  }
}

RFID_Event RFID_PollEvent(uint8_t card_id[RFID_CARD_ID_LENGTH], uint8_t *retry_count)
{
  RFID_Event event;
  uint8_t i;

  __disable_irq();
  event = rfid_event;
  rfid_event = RFID_EVENT_NONE;

  for (i = 0U; i < RFID_CARD_ID_LENGTH; i++)
  {
    card_id[i] = rfid_last_card[i];
  }

  if (retry_count != 0)
  {
    *retry_count = rfid_retry_count;
  }
  __enable_irq();

  return event;
}

uint8_t RFID_IsUnlocked(void)
{
  return rfid_unlocked;
}

void RFID_StartEnroll(void)
{
  rfid_enroll_mode = 1U;
  rfid_event = RFID_EVENT_NONE;
  rfid_index = 0U;
}

uint8_t RFID_HasCard(void)
{
  return rfid_card_saved;
}

void RFID_Lock(void)
{
  rfid_index = 0U;
  rfid_retry_count = 0U;
  rfid_unlocked = 0U;
  rfid_enroll_mode = 0U;
  rfid_event = RFID_EVENT_NONE;
}
