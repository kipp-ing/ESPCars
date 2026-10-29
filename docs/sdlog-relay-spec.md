# sdlog_relay: reach an sd_logger behind someone else's Home Assistant

2026-10-29. **Confirmed as the starting point (Q1, Q2 as proposed); reality will refine it.** A Home Assistant custom integration that lets a collector
(`sdlog_fleet` / `sdlog_collect`) pull an sd_logger that sits in a LAN the collector cannot route to,
when the only way in is that LAN's Home Assistant (for example through a tunnel that forwards HA and
nothing else). It also hands the logger its next firmware, so the logger can update itself without
internet access.

Every requirement carries an ID and the test that proves it. **A requirement without a test is not done.**
The collector half (bearer token, auth lockout) is specified in [`sdlog-fleet-spec.md`](sdlog-fleet-spec.md), not here.

## 1. Shape

```
collector ──HTTP, HA bearer token──▶ HA core ──▶ sdlog_relay ──HTTP──▶ sd_logger :8080 (same LAN as HA)
                                                     ▲
sd_logger update entity ──HTTP, no token, source IP = logger──┘  (fw/manifest.json, fw/firmware-<md5>.ota.bin)
admin (HA admin token) ──PUT fw──▶ sdlog_relay ──▶ /config/sdlog_relay/<logger_id>/
```

**Pass-through, not store-and-forward.** The relay never keeps a chunk. The confirm (`POST /sdlog/done`)
reaches the device only when the collector sends it, after its own verification, so the end-to-end
guarantee of the collection design is untouched. A buffering relay would have to confirm before the data
reached its archive.

## 2. Configuration

One config entry per logger, created by a config flow (UI or `config/flow` over the API), so it can be set
up remotely:

| Key | Meaning |
|---|---|
| `logger_id` | path segment, `^[a-z0-9][a-z0-9_-]{0,31}$`, unique per HA |
| `host` | the logger's IPv4 address in HA's LAN (a DHCP reservation is the operator's job) |
| `port` | default `8080` |

No other setting. The relay can reach exactly the configured `host:port` per entry and nothing else.

## 3. Requirements

### Pass-through

| ID | Requirement | Test |
|---|---|---|
| R1 | `GET /api/sdlog_relay/<logger_id>/sdlog/index`, `GET …/sdlog/status`, `GET …/sdlog/f/<name>`, `POST …/sdlog/done/<name>` are forwarded to the same path on the logger. Status code and body pass through unchanged, including `206`, `404`, `409` and `416`. | `test_relay_passes_status_and_body` (parametrized over the [wire contract](sdlog-phase-b-wire-contract.md) cases) |
| R2 | Request header `Range` passes through. Response headers `Content-Length`, `Content-Range`, `Content-Type`, `Accept-Ranges` pass through. Nothing else in either direction (no cookies, no `Authorization` upstream). | `test_relay_range_resume`, `test_relay_strips_headers` |
| R3 | The body is streamed in blocks of at most 64 KiB. Memory use does not grow with chunk size. | `test_relay_streams_32mib_with_bounded_memory` |
| R4 | `<name>` must match `^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$` and contain no `..`. Anything else is answered `400` without contacting the logger. Unknown `logger_id` → `404`. No other path under `/api/sdlog_relay/` is forwarded. | `test_relay_rejects_bad_names`, `test_relay_unknown_logger` |
| R5 | Logger unreachable (connect refused, timeout 5 s connect / 30 s between blocks) → `502` with `{"reason":"logger_unreachable"}`. Upstream drops mid-body → the response is aborted, never padded or completed. | `test_relay_502_when_offline`, `test_relay_aborts_on_upstream_drop` |
| R6 | One upstream request per logger at a time. A second request for the same logger waits up to 5 s, then gets `503` with `{"reason":"busy"}`. The ESP's httpd serves few sockets. *(Built 2026-10-29: refusing at once failed on Linux, where the collector's next request regularly arrives before the previous handler has finished.)* | `test_relay_one_upstream_per_logger`, `test_relay_queues_a_request_behind_a_short_one` |
| R7 | All pass-through views require HA authentication (`requires_auth = True`). Any HA user may use them, admin is not needed. | `test_relay_requires_auth`, `test_relay_any_user_may_pull` (tests_ha) |
| R8 | The relay never answers `401` for its own reasons. Only HA's auth layer does, and HA counts each `401` as a failed login against `request.remote` (`http/ban.py`). Upstream errors become `502`/`503`, never `401`. | `test_relay_never_emits_401`, `test_only_ha_auth_counts_a_failed_login` (tests_ha) |

### Firmware

| ID | Requirement | Test |
|---|---|---|
| R9 | `GET /api/sdlog_relay/<logger_id>/fw/manifest.json` and `…/fw/firmware-<md5>.ota.bin` (the name the manifest gives, relative to it) need **no** HA auth, but answer only when `request.remote` (after HA's `X-Forwarded-For` handling) equals the entry's `host`. Every other source gets `404`, which does not count as a failed login. Reason: an ESPHome firmware embeds the WiFi PSK and the API key. | `test_fw_only_for_logger_ip`, `test_fw_other_source_404`, `test_fw_xff_from_trusted_proxy_not_spoofable` |
| R10 | `PUT /api/sdlog_relay/<logger_id>/fw` takes the `.ota.bin` as body plus query `version` and `chip_family` (ESP Web Tools names, e.g. `ESP32-C6`). **Admin only.** The relay computes the MD5 and writes an ESP Web Tools manifest with an `ota` block ([format](https://esphome.io/components/update/http_request/)). | `test_fw_upload_admin_only`, `test_fw_manifest_format` |
| R11 | Files live in `/config/sdlog_relay/<logger_id>/`, never under `/config/www`. The binary is named by its MD5 and written first (temp file + rename), then the manifest the same way. The previous binary is kept, older ones are pruned. So any manifest a reader holds names a complete binary with matching MD5, even mid-upload. *(Built 2026-10-29: two fixed names cannot give that guarantee.)* | `test_fw_atomic_replace` |
| R12 | No firmware uploaded yet → `404` for the logger too. | `test_fw_absent_404` |

### Shape of the code

| ID | Requirement | Test |
|---|---|---|
| R13 | The forwarding logic lives in a module that imports neither `homeassistant` nor anything outside `aiohttp` and the stdlib (the `gateway_core.h` rule). The HA glue (views, config flow, auth checks) stays thin. | `test_core_has_no_ha_import` |
| R14 | Core tests (`tests/relay/`) run against the existing `FakeDevice` from `tests/sd_logger/test_collect.py` behind an in-process relay, and the real `sdlog_collect.py` passes its pull cases through it unchanged. HA glue tests live in `tests_ha/` (own venv, CI job `ha-integration`). | `test_collect_through_relay` |
| R15 | Layout for HACS: `custom_components/sdlog_relay/` with `manifest.json` (`domain`, `name`, `version`, `documentation`, `issue_tracker`, `codeowners`), `hacs.json` at the repo root, `custom_components/sdlog_relay/brand/icon.png` ([HACS](https://www.hacs.xyz/docs/publish/integration/), [brand folder since HA 2026.3](https://contributors.home-assistant.io/blog/2026/02/24/brands-proxy-api)). | `test_hacs_layout` |

## 4. Scenarios

| ID | Case | Expected |
|---|---|---|
| S1 | Collector pulls a sealed chunk through the relay | archived, verified and confirmed exactly as on the LAN |
| S2 | HA restarts mid-chunk | collector sees a short transfer and resumes with `Range` next pass |
| S3 | Logger off (car away) | `502 logger_unreachable`; the collector treats it as "device away" (exit 75) |
| S4 | Collector with a wrong token | HA answers `401` once; the collector's lockout (fleet spec) stops further attempts |
| S5 | Internet client fetches `fw/firmware.ota.bin` through the public HA URL | `404` |
| S6 | Client in the LAN, not the logger, fetches `fw/` | `404` |
| S7 | Logger's update entity polls during a firmware upload | gets the old pair or the new pair |
| S8 | `<name>` = `../../config` | `400`, the logger is never contacted |

## 5. Not in scope

- Auth towards the logger. The logger's `:8080` has none, and the relay adds none.
- Entities. The collector side already reports pull state to its own Home Assistant.
- Any write to a vehicle bus. The relay only speaks the collection endpoints.

## 6. Open questions

| ID | Question | Proposal |
|---|---|---|
| Q1 | How to test the HA glue (views, flow, admin check)? | `pytest-homeassistant-custom-component`, pinned, in its own CI job added to `Required checks`, not in `requirements_test.txt` |
| Q2 | Release channel for HACS | GitHub releases tagged `sdlog_relay-vX.Y.Z` |
