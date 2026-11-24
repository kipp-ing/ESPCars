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

`isotp.send` accepts 1–4095 bytes, further bounded by the instance's
`max_message_size`. Static lists and UTF-8 strings are checked during configuration;
lambda payloads are checked at their full length at runtime before any transmission.

A refused flow-control frame is retried only while it belongs to the current
transfer. Completion, failure, timeout or a replacement SF/FF cancels it; receive
timeout is checked before retrying. Refused overflow notifications retain their
existing retry behavior but expire after `n_cr_timeout`, without another error event.

PCI-only consecutive frames are ignored and counted in `frames_ignored`; they do
not consume sequence numbers, grant another block or restart N_Cr. This follows
[ISO 15765-2:2016](https://studylib.net/doc/27665798/iso-15765-2-2016), §10.4.3
(ignore undersized protocol data units) and §9.6.4.1–2 (CF length, with a shortened
last CF carrying the remaining data). The original N_Cr deadline remains active.
This is a receive validation change; accepted frames retain their wire format.

Worked configurations are in `tests/components/isotp/common*.yaml`. Validate
or compile them with `script/test_build_components.py` and the corresponding
`test[-variant].<target>-idf.yaml` path. Schema reference expansion is P7.
