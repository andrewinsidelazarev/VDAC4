#pragma once
#include "FreeRTOS.h"
struct StubMutex { bool taken; };
typedef StubMutex* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new StubMutex{false}; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  if (mutex->taken) return pdFALSE;
  mutex->taken = true;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->taken = false; return pdTRUE; }
