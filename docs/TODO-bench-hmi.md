# TODO — SD + display + encoder on the reference board (software tasks)

Drafted 2026-07-22. Scope: bring `sd_logger`, an SPI display and the rotary
encoder up on `PCB-ESP32C6-Adapter-CAN-Modbus-LIN` while dual CAN and LIN keep
running. I2C and RS-485 are dropped; **U20 (SP3485) is removed from the board**,
which frees GPIO16/17/23.

Hardware pinout this plan assumes is in "Pinout" below. Pin facts are taken from
`production/netlist.ipc` in the PCB repo, not from its README (which is stale).

---

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
| DISP_RST | — | — | — | RC on the module (10 k + 100 nF) |
| ENC_A | 4 | J1.3 | 100 Ω (R10) only | **10 k↑ to 3V3 — board has none** |
| ENC_B | 5 | J1.4 | 100 Ω (R11) + 2.35 k↑ (R1‖R2) | — |
| ENC_SW | 9 | J1.5 | 100 Ω (R12) + 4.7 k↑ (R5) | — |
| VCC sense | 6 | on-board | R21/R22/C1 divider | — |
| Backlight | — | — | — | 100 Ω to 3V3 (no pin left) |
| *unused* | 0 | J4.3 | FUSB302 INT (U16.5) | leave open |

Solder jumpers: **JP4 + JP5 closed** (J4 pin 1 = 3V3 *and* the R15/R16 pull-up
rail = 3V3), JP3 and JP6 open, JP1 closed (J2 pin 1 = 3V3), JP2 open, JP14/JP15
stay open. JP20/JP22 closed for CAN2, JP17/18/19 open. **JP21 stays open** —
GPIO12 is USB D−, see SW-8.

Unchanged: CAN1 GPIO2/3, CAN2 GPIO10/11, LIN GPIO18/19 + GPIO7, LED GPIO8,
USB-JTAG GPIO12/13.

---

## SW-1 — `sd_logger` must share SPI2, not own it (blocker)

The ESP32-C6 has exactly one general-purpose SPI host. `sd_logger.cpp:108` calls
`spi_bus_initialize(SDSPI_DEFAULT_HOST)`; ESPHome's `spi` component calls the
same on the same host (`spi_esp_idf.cpp:251`, and `get_hw_interface_list()`
returns a single `spi2` entry for the C6). Second caller gets
`ESP_ERR_INVALID_STATE`. **SD + SPI display cannot both boot today.**

- [ ] Add optional `spi_id: use_id(spi.SPIComponent)` to the schema. Mutually
      exclusive with `clk_pin`/`mosi_pin`/`miso_pin`; `cs_pin` stays required
      in both modes. New rule **V10** (accept + reject paths).
- [ ] Resolve the host id in `FINAL_VALIDATE_SCHEMA` by reading the referenced
      spi component's validated `interface_index`. Do **not** add a getter to
      `SPIComponent` — the hard rule is no esphome-core edits. Reject
      `interface: software`.
- [ ] C++: `set_own_bus(bool)` + `set_host(int)`. Skip `spi_bus_initialize`
      when not the owner; still call `sdspi_host_init_device()` with the shared
      `host_id`. Keep the H2 settle delay on the owning path only.
- [ ] Setup ordering: `sd_logger` must run after `SPIComponent::setup()`
      (`setup_priority::BUS`). Set its priority below BUS and assert the bus is
      initialized before mounting.
- [ ] Verify ESPHome's bus init leaves `max_transfer_sz` large enough for the
      SD block transfers; bump via the display's config if not.
- [ ] Tests: schema accept (shared-bus), reject (both `spi_id` and `clk_pin`;
      neither; software interface). Build yaml with `spi:` + display + sd_logger
      on one bus. `esphome compile` on the C6.

## SW-2 — `card_power_pin` behaviour

**Blocked, and the reason matters: there is no switch.** GPIO17 reaches header
J5.6 through R32 and terminates there; the H5 high-side switch on card VCC was
specified but never built. Everything below is a no-op until it is fitted, and
the bench configs no longer declare `card_power_pin` (2026-07-29). Recovery from
a wedge is the in-band reset instead — `components/sd_logger/card_reset.h`,
which needs no hardware.

**Lean towards closing this section, not fitting the switch.** On 2026-07-29 the
in-band reset cleared **12 of 12** staged wedges on Mr. Orange with no power
cycle, including the real stress rig cut by a reflash mid-write (back up in
3.1 s) — wedge run (2026-07-29, 12/12, git history). H5 was justified by the belief that
some wedges are only clearable by removing power; no such wedge has now been
observed. The two outcomes that would have been evidence for it
(`no response at cmd0`, `mute at cmd0`) each appeared once and cleared on the
very next attempt. Fit H5 only if a wedge survives a full ladder — and record
the `card reset:` lines that show it, because nothing so far has.

- [ ] Fit the H5 switch, or close this section. If it is fitted: the firmware
      must drive CLK/MOSI/MISO/CS **low** for the whole off window, because
      `gpio_reset_pin()` leaves them pulled up and the board adds 2.2k/4.7k
      externals — a card whose VCC is cut back-powers through its I/O clamp
      diodes and never sees a power-on reset. 150 ms is also too short; ~500 ms.
- [ ] Default the pin **off** at boot, power up in `setup()` with a settle delay
      before mount (external 10 k↓ holds the switch off while GPIO17 floats).
- [x] Add power-cycle-on-wedge recovery: on repeated write failure, de-power,
      wait, re-init, re-mount. *Shipped in 88e4840 and inert on this board —
      see above.*
- [ ] Emergency path (§7 Layer B): close file, then de-power the card.
- [ ] Schema test for the pin being distinct from SPI pins already exists
      (V1/V8) — extend to the shared-bus mode.

## SW-3 — Display

- [ ] Pick the driver and add it to the build yamls. Cap `data_rate` at
      **10 MHz** — 100 Ω series on every header pin plus flying leads makes
      20 MHz marginal.
- [ ] `dc_pin: GPIO1`, `cs_pin: GPIO16`, **no `reset_pin`** — GPIO23 goes to the
      bypass relay (SW-8) and the module gets an RC power-on reset instead.
      Confirm the chosen driver tolerates an omitted `reset_pin`; if it does not,
      SW-8 and the display reset compete for the same pin and one has to give.
- [ ] Keep refresh ≤ 2 Hz and prefer partial updates: a 240×240×16 bpp full
      blit holds the shared bus ~92 ms at 10 MHz, which is a writer stall.
- [ ] Confirm `buffer_depth` absorbs the worst-case blit; 4096 records covers it
      with margin, but measure rather than assume.

## SW-4 — Encoder

- [ ] `rotary_encoder` on GPIO4/GPIO5, `binary_sensor` (gpio) on GPIO9, all with
      `mode: INPUT_PULLUP`.
- [ ] **GPIO4 has no board pull-up.** R1 and R2 (4.7 k each) both landed on
      J1 pin 4, so ENC_B sees 2.35 k and ENC_A sees nothing. Internal pull-up
      alone is ~45 k — fit an external 10 k, and note GPIO4 is a strapping pin
      (MTMS) that should not float at reset.
- [ ] Document in HIL.md: J1 pin 2 is GND, so the switch pulls GPIO9 low —
      **holding the encoder button during reset boots into download mode.**
- [ ] Verify `rotary_encoder` builds on the C6 (PCNT path) — it has no variant
      gate in `sensor.py`, but it has never been compiled for this target here.

## SW-5 — `vcc_monitor` against the real divider

- [ ] **Blocker found on the bench 2026-07-26: the ADC conversion is
      uncalibrated and reads 26 % low, so the emergency close fires on a healthy
      rail.** Measured on Mr. Orange: raw 1087 → `monitor_loop_()` computes
      8.94 V while esphome's calibrated `adc` on the same pin reads v_pin =
      1.093 V ⇒ 12.07 V actual. The trip is one-way (`dying_` latches and the
      task deletes itself), so the effect is that `vcc_monitor` silently and
      permanently disables logging at boot. Not configurable around: the schema
      caps `adc_full_scale` at 3300 mV, worth only 9.51 V. Fix is `adc_cali`
      curve fitting (`adc_cali_create_scheme_curve_fitting`, fall back to the
      linear estimate when no eFuse scheme is available), plus a schema test.
      `tests/hil/mr-orange-sdlog.yaml` currently carries a board-specific
      `divider: 14.67` stopgap that must be reverted to the physical 10.8673
      once this lands.
- [ ] Configure `adc_pin: 6`, `divider: 10.8673`.
- [ ] The board's divider is affine (`VCC = 10.8673·V_pin + 0.1959`, the offset
      being Q2's Vce_sat) but the schema models a pure ratio. Effect: the rail
      reads ~0.2 V low, so the emergency close trips ~0.2 V early — safe
      direction. Either document it, or add an optional `offset` key (V11) and
      model it properly.
- [ ] Note in the spec that the divider top is gated by GPIO7 (Q1/Q2), so the
      monitor reads ~0 during a deliberate sleep window. Suppress the trip while
      GPIO7 is low, otherwise a sleep cycle looks like a power failure.

## SW-6 — HIL integration and gate

- [x] Replace the placeholder pins in `tests/hil/mr-orange-sdlog.yaml` with the
      table above. **Done 2026-07-26** — closes **M0**. The old placeholders were
      not merely unproven, they were actively wrong: `cs_pin` sat on GPIO23 (the
      SW-8 bypass relay) and the ADC on GPIO0 (FUSB302 INT). Card enumerated at
      10 MHz first try; logger ran with `dropped=0`. Display and encoder are
      still to be added.
- [ ] Run `verify.py --seconds 45 --min-laps 75`. Baseline is 19.4 laps/s with
      zero errors on every counter; SPI traffic must not move it.
      ~~**Blocked 2026-07-26:** Mr. Blue was decommissioned, and `verify.py`
      requires all three boards. Needs a new baseline under the new topology.~~
      **Unblocked 2026-07-29:** Blue is back and the gate measures the original
      19.4 laps/s — no new baseline needed. This item is runnable; what it still
      needs is the *display+encoder* load case, not a working bench.
- [ ] Add a load case: display refreshing while the logger runs, asserting
      `dropped == 0` and CAN `bus_err` unchanged.
- [ ] Record the run in HIL.md the same way the token-ring baseline is recorded.

## SW-7 — Observability for the shared bus

- [ ] Expose max writer-stall duration in `sd_logger` statistics, so a display
      blit starving the writer is visible rather than inferred.
- [ ] Log dropped-record count as a warning, not just an info line.

## SW-8 — CAN gateway bypass relay (negative control for the ring)

K1 (G6K-2F, DPDT) has its commons on **J12 pins 3/4 — the CAN2 connector pair**
— switching them between `JP8-A`/`JP10-B` (U3's CANH/CANL, two independent
segments) and `J12.1`/`J12.2` (CAN1's bus, segments merged and U3 off the wire).
Drive chain: `/CAN/CAN_GW_SW` → R39 (1 kΩ) → Q3 base (SS8050) → coil on the
GPIO7-gated **+5 V** rail, D3 flyback.

This is the missing negative control: today nothing proves the token ring
depends on gateway forwarding rather than on the two segments being joined
somewhere. Energize the relay and the ring must *break*.

- [ ] Drive it from **GPIO23**, not GPIO12. JP21 would tie the net to GPIO12,
      which is USB D− — and the whole harness is USB-only (`verify.py:42` opens
      `serial.Serial` on all three ports, `flash.sh:18` uploads over usbmodem,
      `hil_common.yaml` logs over `USB_SERIAL_JTAG` with no wifi/API/OTA).
      Leave JP21 open and run a flying wire from J5.8 to R39's input.
- [ ] Expose it as a `switch` (`restore_mode: ALWAYS_OFF`) on Mr. Orange, named
      so it is obvious in the log that the bench is in bypass.
- [ ] Add the negative-control case to `verify.py`: run the ring green, energize
      the relay, assert the ring **stops** (token age exceeds threshold, CAN2
      goes quiet or bus-off), de-energize, assert recovery to baseline. A run
      where the ring survives bypass means the segments are shorted somewhere
      and the gate has been passing for the wrong reason.
- [ ] Feed the bypass window into `on_bus_off`/`on_recovered` so it also
      exercises the G3 auto-recovery path rather than just breaking the ring.

Before driving it, check three hardware facts:

- **K1 may not be fitted.** Its BOM row has an empty LCSC part number, so it was
  not in the JLCPCB assembly — hand-fit only, like the jumpers and standoffs.
- **BOM value and footprint disagree**: value `G6K-2F DC5`, footprint
  `RELAY_G6K-2F DC3_OMR`. Verify the fitted coil is 5 V; a 3 V coil on +5 V
  overheats.
- **NO/NC direction is not derivable from the netlist** — confirm by continuity
  which coil state merges the segments.

Side effects to expect: the coil sits on the GPIO7-gated domain, so a sleep
window drops the relay (safe, and it means bypass cannot outlive an awake
board). Energizing adds roughly 28 mA to a domain the PCB repo's lab notes
characterized at ~4 mA gated draw — re-baseline that measurement if K1 is fitted.

## SW-9 — Docs

- [ ] Update `docs/sd_logger-spec.md` §3 (H3 pins, H5 card power) with the real
      pin map and mark M0 done.
- [ ] Add the shared-bus mode to the component's config reference (Phase 2 of
      the roadmap).
- [ ] File two corrections against the PCB repo's `TESTING.md`:
      line 255 claims U18's EN reaches GPIO7 "via JP14 (open)" — the netlist
      shows `U18 pin 1 = GPIO7` directly, and JP14 is the GPIO22↔FUSB_SCL
      jumper; and the missing GPIO4 pull-up (R1/R2 both on J1 pin 4).

---

## Not covered by software

- **Hold-up energy (spec H6) does not exist on this board.** D6 is a shunt clamp
  across VCC, not a series element, and total 3V3 bulk (~120 µF) is worth about
  1 ms at 50 mA. The emergency close is best-effort until a series Schottky plus
  ~1000 µF is added on VCC at J8 — that buys ~54 mJ, roughly 90 ms at 0.6 W.
- The backlight is hard-wired to 3V3; no pin remains for dimming.
