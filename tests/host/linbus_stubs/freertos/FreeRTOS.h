#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskHandle_t = void *;
using QueueHandle_t = void *;
constexpr int pdTRUE = 1;
constexpr int pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int configMAX_PRIORITIES = 25;
constexpr int tskNO_AFFINITY = -1;
#define pdMS_TO_TICKS(ms) (ms)
inline int xQueueReceive(QueueHandle_t, void *, TickType_t) { return 0; }
inline void xQueueReset(QueueHandle_t) {}
