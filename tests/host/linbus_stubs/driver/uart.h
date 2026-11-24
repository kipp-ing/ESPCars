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

namespace linbus_test {
inline bool installed = false;
inline bool fail_write = false;
inline int param_calls = 0, pin_calls = 0, deletes = 0;
}  // namespace linbus_test
constexpr int UART_DATA_8_BITS = 0, UART_PARITY_DISABLE = 0, UART_STOP_BITS_1 = 0;
constexpr int UART_HW_FLOWCTRL_DISABLE = 0, UART_SCLK_DEFAULT = 0, UART_PIN_NO_CHANGE = -1;
struct uart_config_t {
  int baud_rate, data_bits, parity, stop_bits, flow_ctrl, rx_flow_ctrl_thresh, source_clk;
};
inline bool uart_is_driver_installed(uart_port_t) { return linbus_test::installed; }
inline int uart_param_config(uart_port_t, const uart_config_t *) {
  ++linbus_test::param_calls;
  return ESP_OK;
}
inline int uart_set_pin(uart_port_t, int, int, int, int) {
  ++linbus_test::pin_calls;
  return ESP_OK;
}
inline int uart_driver_install(uart_port_t, int, int, int, QueueHandle_t *q, int) {
  if (linbus_test::installed)
    return ESP_FAIL;
  linbus_test::installed = true;
  *q = reinterpret_cast<QueueHandle_t>(1);
  return ESP_OK;
}
inline int uart_driver_delete(uart_port_t) {
  ++linbus_test::deletes;
  linbus_test::installed = false;
  return ESP_OK;
}
inline int uart_set_rx_full_threshold(uart_port_t, int) { return ESP_OK; }
inline int uart_set_rx_timeout(uart_port_t, int) { return ESP_OK; }
inline int uart_set_baudrate(uart_port_t, int) { return ESP_OK; }
inline int uart_write_bytes(uart_port_t, const char *, size_t len) { return linbus_test::fail_write ? -1 : int(len); }
inline int uart_get_buffered_data_len(uart_port_t, size_t *len) {
  *len = 0;
  return ESP_OK;
}
