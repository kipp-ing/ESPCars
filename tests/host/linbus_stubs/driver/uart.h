#pragma once
#include <cstddef>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
enum uart_port_t { UART_NUM_0, UART_NUM_1, UART_NUM_2 };
enum { UART_DATA, UART_BREAK, UART_FRAME_ERR, UART_PARITY_ERR, UART_FIFO_OVF, UART_BUFFER_FULL };
struct uart_event_t {
  int type;
  size_t size;
};
inline int uart_read_bytes(uart_port_t, void *, size_t, TickType_t) { return 0; }
inline void uart_flush_input(uart_port_t) {}
inline esp_err_t uart_wait_tx_done(uart_port_t, TickType_t) { return ESP_OK; }
