# isotp — ISO-TP transport

Supported platform: **ESP32 with ESP-IDF only**. Arduino and non-ESP32
targets are rejected during config validation. Requires a can_gateway port; its TWAI variant limits apply, including the ESP32-C5 rejection (V27).

Worked configurations are in `tests/components/isotp/common*.yaml`. Validate
or compile them with `script/test_build_components.py` and the corresponding
`test[-variant].<target>-idf.yaml` path. Schema reference expansion is P7.
