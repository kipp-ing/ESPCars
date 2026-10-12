# Portal bench checklist (PS-M2-06..13, PS-M3-06/07, PS-M5-*)

Board: identified by MAC, test image `tests/build/portal/test.esp32-c6-idf.yaml` (no bus
component; template entities only). `H=<board ip>`, `U=demo:demo-pass` (the fixture's `auth:`).
Before flashing record the running image; restore it afterwards. Schema check of live JSON:
`curl -s http://$H/api/v1/state | python3 script/portal_schema.py /dev/stdin`.

| step | command | expect |
|---|---|---|
| state open | `curl -si http://$H/api/v1/state` | 200, `Transfer-Encoding: chunked`, validates (M2-06) |
| page open | `curl -si http://$H/ \| head -12` | 200 gzip, no-store, nosniff, no login (M2-09) |
| query rules | `?x=1` -> 400; `?after=abc` -> 400; `?path=nope` -> 404; `?after=<seq>` -> 204 | (M2-08) |
| 401 without login | `curl -si -X POST -d '' http://$H/api/v1/actions/vehicle.logger/restart` | 401 + Digest challenge, `seq` unchanged (M2-09, M3-07) |
| 202 with login | `curl -si --digest -u $U -X POST -d '' http://$H/api/v1/actions/vehicle.device/restart` | 202 running/queued, next state shows accepted/applied |
| switch round trip | `curl --digest -u $U -H 'Content-Type: application/json' -d '{"value":true}' http://$H/api/v1/actions/vehicle.logger/logging`, then state, then `false` | value true then false (M3-06) |
| select / number | `-d '{"value":"run"}'` on `vehicle.device/mode`; `-d '{"value":12}'` on `vehicle.device/interval` | accepted |
| refusals | `{"value":99}` on interval -> 422 out_of_range; `{"value":"x"}` on mode -> 422 unknown_option; `{"value":"x"}` on logging -> 400 wrong_type; `-d 'junk'` with JSON type -> 400 invalid_body | recorded as refused in the tree (M3-07) |
| 404 | `.../actions/nope/x` and `.../actions/vehicle.logger/nope` | 404 |
| 411 | `curl -si --digest -u $U -X POST http://$H/api/v1/actions/vehicle.device/restart` (no `-d`) | 411 from web_server_idf, nothing recorded |
| form >1 KiB | `-d "value=$(head -c 1500 /dev/zero \| tr '\0' a)"` | 400 from web_server_idf, nothing recorded |
| curl form default | `-d '{"value":true}'` without the JSON header | 400 invalid_body (form body); `-d 'value=true'` works |
| busy | two POSTs back to back on one action | second may be 409 `{"error":"busy"}`, not recorded |
| 503 lock timeout | hammer 20 parallel POSTs while polling state | only 202/409/503 appear, 503 has empty body, tree unchanged by it |
| heap / stack | log lines `free heap`, `max block`, httpd stack high-water after a full state | stack >= 1024 B (M2-07); numbers to the board (M2-11) |
| shutdown / OTA | start a POST loop (`while true; do curl -s -o /dev/null --digest -u $U -X POST -d '' http://$H/api/v1/actions/vehicle.device/restart; done`) and press the device's restart (or OTA the same image) | no crash, no reboot loop, no panic in the log; requests during shutdown are served or refused (B1) |
| N1 back to back | alternate a big JSON body POST (200 B) and an empty button POST, 100x | each result matches its own request (no body bleed) |
| AP fallback | join the AP, `curl -si http://192.168.4.1/`, `/api/v1/state`, `/wifi`, `/config.json`, `/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`, `/zzz` | page, state with `wifi_setup` link, WiFi page, JSON, then 302 `Location: http://192.168.4.1/` x4 (M2-10, M2-12) |
| page: toggle / dropdown | click the logging toggle, pick a mode | result chip appears without clicking elsewhere, `seq` in the header follows the board (M3-10) |
| page: clocks | leave the page idle 15 s | `up …` and the chip ages keep counting (M3-10) |
| no-auth image | a build without `auth:` | state 200; POST 202 without login |

## Last run: 2026-10-12, ESP32-C6, ESPHome 2026.7.0 (8695e40)

All rows green, the page rows after the fix in 8695e40. httpd stack high-water at least 1448 B free;
idle heap of this test image 285-290 KB free, largest block 264 KB. AP fallback checked with a phone: portal,
captive redirects, Digest prompt on an action, `/wifi`. Note: credentials saved through `/wifi` bring the board
back over DHCP, not on its `manual_ip` (find it via `<name>.local`); the next image uses its YAML networks again.
