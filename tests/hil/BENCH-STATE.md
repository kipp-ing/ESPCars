# Bench state

Measured 2026-08-04 09:16:05 CEST by `script/hil/selftest.py --no-flash`. Regenerate rather than edit.

**`--no-flash`** — this read whatever firmware was already running, which may not be the selftest firmware; a gate can FAIL or read INDETERMINATE for that reason alone, and this report may say less than the one it overwrote (`git diff` before trusting it).

**Read the gates before the wiring.** Each gate is a precondition for the
one below it. An unpowered CAN transceiver reproduces every symptom of
broken wiring — no echo, no ACK, TEC to bus-off, nothing received — so a
topology line from a board whose `xcvr` gate failed is not evidence about
cabling and is reported as INDETERMINATE here on purpose.

| gate | proves |
|---|---|
| `reach` | the board can be talked to at all — it accepted a flash. Above every gate below it: an unreachable board has no measurements, only a recovery. |
| `rail` | 12 V on J8 (P1) **and** GPIO7 high (P2). The GPIO6 sense is itself gated by GPIO7, so 0 V means *GPIO7 low **or** no 12 V*, never "no 12 V" alone. |
| `xcvr.<port>` | the +5 V domain (U18, **JP14**), the TJA1044, and the logic-side jumpers (JP20/JP22 arm the GPIO10/11 channel). One TXD→RXD echo, no fixture needed. |
| `bus` | that traffic **survives** on the wire — the controller's own `bus_err` / `tx_fail` / TEC / REC, judged on movement between windows. The echo above proves the transceiver; this proves the segment. A missing terminator or a long stub echoes perfectly and still shreds frames. |
| `lin` | the LIN transceiver — which runs off 12 V and does **not** need +5 V. LIN healthy with both CAN ports dead is the signature of JP14 open. |
| `lin.selftest` | master only — the LIN schedule is answered **on time**, not merely answered. A wire that works but jitters passes `lin` and fails this. |
| `sd` | the card answering CMD0, bit-banged, with no component in the way. |

## Gates

| board | reach | rail | xcvr | bus | lin | sd | verdict |
|---|---|---|---|---|---|---|---|
| **blue** | n/a | — | — | — | — | — | **SILENT — no `[bench]` line — is selftest.yaml the running firmware?** |
| **orange** | n/a | PASS (11.76V) | seg1:PASS / seg2:PASS | PASS | INDETERMINATE | PASS (r1=0x01) | **INCONCLUSIVE** (unproven: `lin`) |

## Wiring

| port | hears | heard by |
|---|---|---|
| `orange.bus0` | `blue.bus0` | — nobody — |
| `orange.bus1` | — nothing — | — nobody — |

Segments measured (connected components of "can hear"):

1. `blue.bus0` ↔ `orange.bus0`
2. `orange.bus1` — isolated

### Problems

- `orange.bus0` hears others but nobody hears it — TX side open.
- `orange.bus1` hears nobody and is heard by nobody. Its xcvr gate PASSED, so the board is fine — this is a cable, a jumper or a termination.
