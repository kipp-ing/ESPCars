#pragma once
#include <mutex>
#include "FreeRTOS.h"
using SemaphoreHandle_t = std::mutex *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  if (timeout == 0)
    return mutex->try_lock() ? pdTRUE : 0;
  mutex->lock();
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
