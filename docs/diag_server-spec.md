# `diag_server`: a table-driven diagnostic responder

**Status:** spec, 2026-11-27. No code yet.

**Use:** bench only. `diag_server` makes an ESP answer diagnostic requests the way an ECU would, so a tester (`uds` hubs, `isotp` raw testers) can run against a table instead of a vehicle. It is a test fixture; it never goes on a vehicle bus. That is why everything in it starts off.

## Model

Each server is one ECU behind one `isotp` instance. On that instance, the IDs are swapped: `tx_id` is the ECU's reply ID and `rx_id` is the tester's request ID. `diag_server` is the instance's consumer (`IsoTpProtocol::set_consumer`, one consumer per instance, the same rule as `uds` U13). When a request message arrives it looks up a reply and sends it with `isotp.send`.

The protocol is raw bytes, so UDS and KWP2000-on-ISO-TP both work. The table decides what is a valid request; the server only knows the rules in section R.

## YAML

```yaml
diag_server:
  - id: sim_engine
    isotp_id: iso_engine          # required, an isotp instance whose consumer is free
    table: sim/engine.json        # required, path relative to the YAML, read at config time
    ecu: engine                   # required: which ECU entry of the table to serve
    enabled: false                # runtime switch initial state (switch platform below)
    s3_timeout: 5s                # non-default session falls back after this much silence
    faults:                       # all off by default, each also a runtime number/switch
      delay: 0ms                  # extra delay before every reply
      pending_78: false           # answer 7F <sid> 78 first, the real reply after `delay`
      drop_every: 0               # 0 = never; n = no reply to every n-th request
switch:
  - platform: diag_server
    diag_server_id: sim_engine
    name: "Sim engine"            # enables or disables answering; off = silent
sensor:
  - platform: diag_server
    diag_server_id: sim_engine
    requests: {name: "Sim engine requests"}
    replies: {name: "Sim engine replies"}
    misses: {name: "Sim engine misses"}   # requests answered by a default NRC
```

## Table file (`diag-sim/1`)

JSON; codegen reads it, validates it and compiles it into flash (const arrays). No file I/O at runtime.

```json
{"format": "diag-sim/1",
 "ecus": [{"name": "engine", "req": "0x7E0", "resp": "0x7E8",
   "entries": [
     {"req": "22 F1 90", "resp": "62 F1 90 57 44 44 ...", "src": "run 2026-01-01"},
     {"req": "10 03",    "resp": "50 03 00 32 01 F4",     "src": "synthetic"},
     {"req": "22 01 *",  "resp": "7F 22 31",              "src": "synthetic"}]}]}
```

- `req` and `resp` are hex bytes with spaces. A trailing `*` in `req` matches any remaining bytes (a prefix match). An exact match always beats a prefix match, and the longest prefix wins among prefix matches.
- `req`/`resp` of the ECU are the CAN IDs. Codegen checks them against the isotp instance's `rx_id`/`tx_id` and refuses a mismatch, naming both values.
- `src` is free text and is kept only for humans. Unknown keys are refused. Duplicate `req` strings are refused.
- Limits are refused at config time with the actual numbers: a reply longer than the isotp `max_message_size`, or a table larger than 64 KB of flash.

## R: rules built in

Match order, first hit wins: (1) exact table entry, (2) the `3E` rows below, (3) longest prefix entry, (4) the `10 xx` default row, (5) NRC `31`, then NRC `11`. So `10 xx` "with a table entry" means an exact or prefix match.

| Request | Answer |
|---|---|
| `3E 00` | `7E 00` |
| `3E 80` (suppress positive response) | nothing |
| `10 xx` without a table entry | `50 xx 00 32 01 F4`, session becomes `xx` |
| `10 xx` with a table entry | the table reply. The session becomes `xx` if the reply starts with `50`. |
| no match, table has other entries with the same first byte | `7F <sid> 31` (requestOutOfRange) |
| no match at all | `7F <sid> 11` (serviceNotSupported) |
| empty request | ignored |

- **Session:** the server tracks the current session. It starts at `01` and falls back to `01` after `s3_timeout` without any request.
- **Session-scoped entries:** an entry may carry `"session": "03"`. When it wins the match outside that session, the reply is `7F <sid> 7F`, serviceNotSupportedInActiveSession; matching does not fall through to other entries.

## Faults (bench only)

- **`delay`:** the reply is held this long, then sent. It is non-blocking: the server holds the reply and the loop runs on.
- **`pending_78`:** the server sends `7F <sid> 78` immediately and the real reply after `delay`. If `delay` is 0, the gap is 100 ms.
- **`drop_every: n`:** every n-th request gets no reply; it is counted in `misses`.

## Behaviour guarantees

- **Busy:** if a request arrives while the previous reply is still being sent (`is_busy()`), the server logs a warning and drops the new request. It is counted in `misses`. Nothing queues beyond one held reply.
- **Off:** when the switch is off, the server sends nothing at all, not even NRCs.
- **No heap after setup:** the table is in flash, and there is one reply buffer per server sized to its longest reply.
- **Logging:** VERBOSE logs one line per request: request hex, rule or entry index, reply length. DEBUG logs only faults and misses.

## Not in scope

- Security access (`27`), with no seed/key logic. A table entry can still answer `27 01` with fixed bytes.
- Multi-frame request reassembly beyond what `isotp` already does.
- Functional addressing (`0x7DF`). One server per physical ID pair.
- Broadcast traffic. That is `can_gateway` `cyclic_sends`.

## Tests (written by a tester, not the programmer)

- **Host/pytest:** schema and table validation for every refusal above, with the message naming the values.
- **Host C++ (if the matcher is a header like `gateway_core.h`):** match precedence (exact, `3E` rules, longest prefix, `10 xx` default, NRC 31/11), the session and S3 fallback with a fake clock, the session-scoped entry, and the counter for every fault.
- **`tests/components/diag_server/`:** a config with a fictional table (`tests/uds/fixtures/` style, no real vehicle data); `config` always, `compile` once.
- **HIL selftest:** two bench boards. One runs `diag_server` on a fictional table, the other a `uds` hub polling it: the values arrive, `misses` stays 0, and each fault produces the tester-side timeout or pending-wait and then recovery.
