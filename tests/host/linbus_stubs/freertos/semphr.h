#pragma once
#include <mutex>
#include <functional>
#include "FreeRTOS.h"
using SemaphoreHandle_t = std::mutex *;
namespace linbus_test {
inline bool fail_mutex = false;
inline std::function<void()> before_take;
}  // namespace linbus_test
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return linbus_test::fail_mutex ? nullptr : new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  if (linbus_test::before_take)
    linbus_test::before_take();
  if (timeout == 0)
    return mutex->try_lock() ? pdTRUE : 0;
  mutex->lock();
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
