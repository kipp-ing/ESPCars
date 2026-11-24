# ccp — CCP measurement and calibration

Supported platform: **ESP32 with ESP-IDF only**. Arduino and non-ESP32
targets are rejected during config validation. Requires a can_gateway port; its TWAI variant limits apply, including the ESP32-C5 rejection (V27).

`allow_write` defaults to `false`. Memory and status reads still transmit, but guarded
write commands, including `SELECT_CAL_PAGE` (`0x11`) and `SET_S_STATUS` (`0x0C`), are refused at runtime
with a warning and no command is sent. `ccp.select_cal_page` still validates
and compiles when writes are disabled. Selecting a calibration page writes no
memory, but switches the calibration data page the ECU runs on, with ECU-defined
effects, so it requires `allow_write: true`.

`SET_S_STATUS` can request STORE (bit 6, `0x40`), which saves calibration data
to non-volatile memory during shutdown, or RESUME (bit 2, `0x04`), which saves
DAQ setup during shutdown and restarts DAQ after power-up. These are write-class
effects, so the command requires `allow_write: true`, including through `ccp.raw`.
See the [NI session status reference](https://www.ni.com/docs/en-US/bundle/ecu-measurement-calibration-toolkit-labview-api-ref/page/mc-ccp-set-session-status-vi.html).

DAQ reads (`ccp.daq_read`) need `allow_write: true` because they configure DAQ
with `WRITE_DAQ`; this requirement already existed. `CcpHub::daq_read()` checks
that permission before `CcpHub::continue_daq_write_` sends `SET_S_STATUS(0x82)`
and then `START_STOP`, so gating `SET_S_STATUS` does not change measurement.
DAQ start/stop remain ungated.

Response IDs and static `ccp.daq_read` DTO IDs register before the gateway freezes
its receive filters. Only the response ID can complete a command; a DAQ read
collects samples only on its requested DTO ID.
For a templated `dto_id`, predeclare its possible 11-bit values in the hub's
`daq_ids` list (at most 2048 entries). An undeclared runtime ID is refused with a
warning before any command is sent. Duplicate receive IDs register only once.

A refused memory-sequence send releases the sequence immediately and reports
`on_error` with local code `0xFD` (`CCP_SEND_REFUSED`); write-memory callbacks
also receive `false`. Read callbacks remain success-only. DAQ cleanup still
attempts each existing stop/status step once. A rejection, timeout or negative
reply during cleanup makes the result unsuccessful, logs a warning that the ECU
may still be streaming, and preserves the first failed step.

Worked configurations are in `tests/components/ccp/common*.yaml`. Validate
or compile them with `script/test_build_components.py` and the corresponding
`test[-variant].<target>-idf.yaml` path. Schema reference expansion is P7.
