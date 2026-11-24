#pragma once
#include "FreeRTOS.h"
#include <functional>
namespace linbus_test {
inline bool fail_task = false;
inline std::function<void(void *)> task_created;
}  // namespace linbus_test
// Never run the infinite task; observe its startup boundary instead.
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *arg, UBaseType_t, TaskHandle_t *,
                                   int) {
  if (linbus_test::task_created)
    linbus_test::task_created(arg);
  return linbus_test::fail_task ? 0 : pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
