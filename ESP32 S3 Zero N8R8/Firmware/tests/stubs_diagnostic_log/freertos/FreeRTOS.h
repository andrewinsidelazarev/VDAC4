#pragma once
// Заглушка FreeRTOS: стенд однопоточный, замок — флажок (занят ли), ожидание не нужно.
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
