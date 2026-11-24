#pragma once
#include "FreeRTOS.h"
// Tests drive worker calls explicitly, including from a real thread. Never start the infinite UART task.
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *, UBaseType_t, TaskHandle_t *, int) {
  return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
