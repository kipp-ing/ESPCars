#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_INVALID_STATE = -2;
constexpr int ESP_ERR_INVALID_ARG = -3;
inline const char *esp_err_to_name(esp_err_t) { return "host error"; }
