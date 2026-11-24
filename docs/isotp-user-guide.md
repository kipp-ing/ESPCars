# isotp — ISO-TP transport

Supported platform: **ESP32 with ESP-IDF only**. Arduino and non-ESP32
targets are rejected during config validation. Requires a can_gateway port; its TWAI variant limits apply, including the ESP32-C5 rejection (V27).

Extended addressing requires both `target_address` (the peer's destination byte
on outgoing frames) and `source_address` (our local destination byte on received
frames). Existing extended configurations must add `source_address`; omission
is a validation error. Every SF, FF, CF and FC with another destination byte is
ignored before it can deliver data, change state or trigger flow control.

An incoming message larger than `max_message_size` sends the existing overflow
flow control and reports `OVERFLOW_LOCAL` once through `on_error`, registered
consumers and `transfers_failed`, even if the CAN port refuses the flow control.
The next valid transfer starts with a cleared receive error.

Worked configurations are in `tests/components/isotp/common*.yaml`. Validate
or compile them with `script/test_build_components.py` and the corresponding
`test[-variant].<target>-idf.yaml` path. Schema reference expansion is P7.
