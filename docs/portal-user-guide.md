# portal — Local portal

Supported platform: **ESP32 with ESP-IDF only**. Arduino and non-ESP32
targets are rejected during config validation. The compile fixtures use ESP32-C6. This component has no independent TWAI/C5 restriction.

Worked configurations are in `tests/components/portal/common*.yaml`. Validate
or compile them with `script/test_build_components.py` and the corresponding
`test[-variant].<target>-idf.yaml` path. Schema reference expansion is P7.
