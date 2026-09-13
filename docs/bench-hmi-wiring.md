# Bench HMI wiring — display + SD + rotary encoder on the C6 adapter

Quick reference distilled from the 2026-07-22/23 wiring session. Full software
task list (schema changes, HIL gate, etc.) lives in
[`TODO-bench-hmi.md`](TODO-bench-hmi.md) — this doc is just "how to connect it."

**Scope:** dual CAN + LIN keep running; add SD logging (`sd_logger`), an SPI
display, and a rotary encoder. I2C and RS-485 are dropped — **U20 (SP3485) is
removed from the board**, freeing GPIO16/17/23. Verified against
`production/netlist.ipc` in the PCB repo (the README pin table is stale).

**Verdict: feasible.** With U20 gone there are exactly 8 clean pins for 8
needed signals.

## Pinout

| Signal | GPIO | Header | On-board parts | Extra part needed |
|---|---|---|---|---|
| SPI SCK | 21 | J3.3 | 100 Ω (R7) + 2.2 k↑ (R6) | — |
| SPI MOSI | 22 | J3.4 | 100 Ω (R8) + 2.2 k↑ (R3) | — |
| SPI MISO | 20 | J4.6 | 100 Ω (R20) + 4.7 k↑ (R16) | — |
| SD_CS | 15 | J4.5 | 100 Ω (R19) + 4.7 k↑ (R15) | — |
| DISP_DC | 1 | J2.3 | 100 Ω (R9) + 4.7 k↑ (R4) | — |
| SD_PWR | 17 | J5.6 | 100 Ω (R32) | 10 k↓ to GND |
| DISP_CS | 16 | J5.7 | 100 Ω (R33) | 10 k↑ to 3V3 |
| CAN_GW_SW | 23 | J5.8 | 100 Ω (R34) | flying wire to R39 input; 10 k↓ at Q3 base |
| DISP_RST | — | — | — | RC on the module (10 k + 100 nF); no GPIO |
| ENC_A | 4 | J1.3 | 100 Ω (R10) only | **10 k↑ to 3V3 — board has none** |
| ENC_B | 5 | J1.4 | 100 Ω (R11) + 2.35 k↑ (R1‖R2) | — |
| ENC_SW | 9 | J1.5 | 100 Ω (R12) + 4.7 k↑ (R5) | — |
| VCC sense | 6 | on-board | R21/R22/C1 divider | — |
| Backlight | — | — | — | 100 Ω to 3V3 (no pin left — always on) |
| *unused* | 0 | J4.3 | FUSB302 INT (U16.5) | leave open |

Unchanged: CAN1 GPIO2/3, CAN2 GPIO10/11, LIN GPIO18/19 + GPIO7, LED GPIO8,
USB-JTAG GPIO12/13.

**Solder jumpers:** JP4 + JP5 closed (J4 pin 1 = 3V3, and the R15/R16 pull-up
rail = 3V3), JP3 and JP6 open, JP1 closed (J2 pin 1 = 3V3), JP2 open, JP14/JP15
stay open. JP20/JP22 closed for CAN2, JP17/18/19 open. **JP21 stays open** —
that net is GPIO12, which is USB D− (the whole bench is USB-only); drive
CAN_GW_SW with a flying wire instead.

## Gotchas found while tracing the netlist

- **One SPI host.** The C6 only has one general-purpose SPI (SPI2). SD and the
  display must **share the bus** — `sd_logger` today calls
  `spi_bus_initialize()` itself, which collides with ESPHome's `spi:`
  component doing the same. This is the blocking software task (SW-1 in the
  TODO).
- **GPIO4 (ENC_A) has no working pull-up.** R1 *and* R2 (4.7 k each) both land
  on J1 pin 4 (ENC_B) instead of one going to pin 3 — looks like an R1
  placement mistake on the board. Fit an external 10 k on ENC_A; it's also a
  strapping pin (MTMS), so don't let it float at reset.
- **Encoder switch is on the boot-mode pin.** J1 pin 2 is GND, so the encoder
  button pulls GPIO9 low — **holding it during reset boots into download
  mode.**
- **Display timing is tight.** 100 Ω series resistors on every header pin plus
  flying leads make 20 MHz marginal — cap SPI `data_rate` at 10 MHz, keep
  refresh ≤ 2 Hz, prefer partial updates (a full 240×240×16bpp blit holds the
  shared bus ~92 ms — long enough to stall the SD writer).
- **No reset pin for the display** — GPIO23 was needed for the CAN gateway
  bypass relay instead, so the display gets an RC power-on reset on the module.
- **VCC-sense divider is affine, not a pure ratio** (`VCC = 10.8673·V_pin +
  0.1959`), so the emergency-close trip point reads ~0.2 V low — safe
  direction, but worth documenting or modeling properly.
- **No hold-up energy exists on this board today.** D6 is a shunt clamp, not a
  series diode, so a clean SD close on power loss is best-effort until a
  series Schottky + ~1000 µF is added at J8.
