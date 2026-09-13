#pragma once

/// The in-band recovery a card interrupted mid-write actually needs, and which no layer below this
/// one performs (docs/sd_logger-spec.md §7 Layer C).
///
/// The stack already runs the SD spec's reset-without-repower procedure on every mount attempt:
/// ESP-IDF's SDSPI driver waits for MISO to go high, clocks 80 cycles with CS deasserted, then
/// sends CMD0 → CMD8 → ACMD41 (`sdspi_host.c`, `go_idle_clockout()` / `poll_busy()`). That is the
/// correct procedure for a card that is *busy* or that has not yet entered SPI mode. It is not the
/// procedure for a card sitting inside an open CMD25: such a card is in Receive-Data state, waiting
/// for either another data token (0xFE/0xFC) or the Stop Tran token (0xFD), and a CMD0 frame is
/// neither — its bytes are consumed as data and never answered. That is the wedge: identical
/// `sdmmc_card_init failed (0x107)` on every boot, cleared only by removing the card's power.
///
/// So this sequence sends the one byte nobody sends — 0xFD, which is the only thing that ends a
/// multi-block write — and waits for busy on a budget the caller chooses instead of the driver's
/// fixed 40 ms (`wait_for_miso` defaults to 0 → 40 ms, and a card doing internal write recovery can
/// need far longer). Everything else here is diagnosis: the measured busy time and the CMD13 status
/// byte are the two numbers that say whether a card that did not come back was busy, mute, or
/// reporting an internal error, and no bench session so far has had either of them.
///
/// ESPHome-free and IDF-free like `recovery_policy.h` and `log_format.h`, for the same reason: every
/// interesting case in here costs a *staged card wedge* to reach on the bench — reset the board
/// mid-write, then wait out a ladder — and a microsecond to reach in
/// `tests/host/test_card_reset.cpp`. A CRC7 that is wrong in the low bit, a busy poll that accepts
/// the first non-zero byte instead of two, and an abort that is checked only between steps are all
/// one-line bugs that present on hardware as "the card never came back", days apart, with no
/// evidence left. IO is injected for exactly that reason.

#include <cstdint>

namespace esphome {
namespace sd_logger {

/// MOSI idle level. Every byte clocked purely to make the card talk is this one.
static const uint8_t SD_SPI_IDLE_BYTE = 0xFF;
/// Ends a CMD25 multi-block write. The point of this whole header.
static const uint8_t SD_SPI_STOP_TRAN_TOKEN = 0xFD;
/// R1 with only "in idle state" set: the answer that means the card is ready to be initialised.
static const uint8_t SD_SPI_R1_IDLE = 0x01;

/// How far the sequence got. Reported even on success, because "recovered at CMD0 after the stop
/// token" and "recovered at the first busy wait" are different diagnoses of the same wedge.
enum class CardResetStep : uint8_t {
  NONE = 0,
  BUSY_WAIT,  ///< clocking with CS asserted, waiting for the card to release DO
  CLOCK_OUT,  ///< 80 cycles with CS deasserted
  STOP_TRAN,  ///< the 0xFD token that closes an open multi-block write
  STOP_CMD,   ///< CMD12, for the read side of the same state
  GO_IDLE,    ///< CMD0
  STATUS,     ///< CMD13
};

enum class CardResetOutcome : uint8_t {
  IDLE = 0,     ///< CMD0 answered 0x01. The card is reachable; mount it.
  STILL_BUSY,   ///< DO stayed low for the whole budget. Give it longer, or it is latched.
  MUTE,         ///< nothing ever drove MISO. Card absent, unpowered, or its TX side is dead.
  NO_RESPONSE,  ///< the card drove the bus but never answered CMD0.
  ABORTED,      ///< the caller withdrew (shutdown) mid-wait.
};

struct CardResetConfig {
  /// Budget for one busy wait. The driver's own is 40 ms; a card doing internal write recovery
  /// after an aborted CMD25 can need orders of magnitude more, which is the untested hypothesis
  /// this number exists to test.
  uint32_t busy_timeout_ms{2000};
  /// CMD0 is retried because the first one can land while the card is still digesting the abort.
  uint8_t cmd0_attempts{10};
  uint32_t cmd0_gap_ms{100};
};

struct CardResetResult {
  CardResetOutcome outcome{CardResetOutcome::MUTE};
  CardResetStep reached{CardResetStep::NONE};
  /// Measured, not assumed: how long the card held DO low before the stop token, and after it.
  uint32_t busy_ms{0};
  uint32_t busy_after_stop_ms{0};
  /// The last R1 seen for CMD0. 0xFF means "no response at all", which is not the same as an error.
  uint8_t r1{0xFF};
  /// Second byte of CMD13's R2: bit3 CC error, bit2 card ECC failed, bit0 card is locked, etc.
  /// Only meaningful when `status_valid`.
  uint8_t status{0};
  bool status_valid{false};
  uint8_t cmd0_tries{0};
  /// True once any byte read back as zero: the card was driving MISO low, i.e. it is present and
  /// busy rather than absent. The single bit that separates "wedged" from "not there".
  bool saw_busy{false};
};

/// One byte in, one byte out, plus the two lines the sequence needs to control itself.
/// Implemented on-device over a raw SPI device at 400 kHz with CS driven by hand; implemented in
/// the host tests by a card model.
struct CardResetIo {
  virtual ~CardResetIo() = default;
  /// true = CS asserted (low on the wire). The card only drives DO while selected.
  virtual void cs(bool assert) = 0;
  virtual uint8_t xfer(uint8_t out) = 0;
  virtual void delay_ms(uint32_t ms) = 0;
  virtual uint32_t now_ms() = 0;
  /// The writer's `dying_` flag. A ten-second busy wait must not outlive an orderly shutdown:
  /// `on_shutdown()` gives the writer 200 ms, and the `#close` line is worth more than the retry.
  virtual bool aborted() = 0;
};

/// CRC7 of a command frame, already shifted into its transmitted position (`<<1 | 1`).
/// Only CMD0 and CMD8 need a correct CRC in SPI mode, but computing it for every frame costs
/// nothing and removes one way for a recovery attempt to fail for a reason of its own making.
inline uint8_t sd_crc7(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < len; i++) {
    uint8_t byte = data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc <<= 1;
      if ((byte ^ crc) & 0x80)
        crc ^= 0x09;
      byte <<= 1;
    }
  }
  return static_cast<uint8_t>((crc << 1) | 1);
}

/// Clock idle bytes until the card releases DO, or the budget runs out.
///
/// The release criterion is two consecutive non-zero bytes, which is ESP-IDF's own
/// (`poll_busy()` in `sdspi_host.c`) and not arbitrary: a single non-zero read can be the last
/// bit-time of the busy window sampled early, and treating that as ready sends the next command
/// into a card that is still programming.
///
/// Caller must have CS asserted. `saw_busy` is accumulated, never cleared, so one wait that saw the
/// card drive low still proves presence to a later step that read only 0xFF.
inline bool sd_wait_not_busy(CardResetIo &io, uint32_t timeout_ms, uint32_t *elapsed_ms, bool *saw_busy) {
  const uint32_t start = io.now_ms();
  uint8_t nonzero = 0;
  for (;;) {
    const uint8_t got = io.xfer(SD_SPI_IDLE_BYTE);
    if (got == 0x00 && saw_busy != nullptr)
      *saw_busy = true;
    if (got != 0x00) {
      if (++nonzero == 2) {
        if (elapsed_ms != nullptr)
          *elapsed_ms = io.now_ms() - start;
        return true;
      }
    } else {
      nonzero = 0;
    }
    // Signed difference, never `now - start >= timeout`: the writer task has been running for as
    // long as the board has, so a recovery attempt can straddle the 49.7-day millis() wrap.
    if (static_cast<int32_t>(io.now_ms() - start) >= static_cast<int32_t>(timeout_ms) || io.aborted()) {
      if (elapsed_ms != nullptr)
        *elapsed_ms = io.now_ms() - start;
      return false;
    }
  }
}

/// Send a command frame and return its R1, or 0xFF if the card never answered.
/// Caller must have CS asserted. NCR is 0..8 bytes for SD; 16 is slack, not superstition.
inline uint8_t sd_send_command(CardResetIo &io, uint8_t index, uint32_t arg) {
  uint8_t frame[5];
  frame[0] = static_cast<uint8_t>(0x40 | (index & 0x3F));
  frame[1] = static_cast<uint8_t>(arg >> 24);
  frame[2] = static_cast<uint8_t>(arg >> 16);
  frame[3] = static_cast<uint8_t>(arg >> 8);
  frame[4] = static_cast<uint8_t>(arg);
  for (uint8_t i = 0; i < 5; i++)
    io.xfer(frame[i]);
  io.xfer(sd_crc7(frame, 5));

  for (uint8_t i = 0; i < 16; i++) {
    const uint8_t got = io.xfer(SD_SPI_IDLE_BYTE);
    if ((got & 0x80) == 0)
      return got;
  }
  return 0xFF;
}

/// Talk a card out of whatever an aborted write left it in, without touching its power.
///
/// The order is not interchangeable. Busy is waited out *first* because a card that is programming
/// will not accept the stop token either; the 80 clocks with CS deasserted come before the token
/// because that is what lets the card resynchronise to a byte boundary; CMD12 follows the token
/// because the same reset must also cover a read transfer left open; and CMD0 comes last because
/// it is the only step whose success means "mountable".
inline CardResetResult card_reset(CardResetIo &io, const CardResetConfig &cfg) {
  CardResetResult result;

  // --- Step 0: how long has this card been holding the bus? -----------------------------------
  // The card drives DO only while selected, so this is the one measurement that must happen with
  // CS asserted, and it is the number no previous session has had.
  io.cs(true);
  result.reached = CardResetStep::BUSY_WAIT;
  const bool released = sd_wait_not_busy(io, cfg.busy_timeout_ms, &result.busy_ms, &result.saw_busy);
  if (io.aborted()) {
    io.cs(false);
    result.outcome = CardResetOutcome::ABORTED;
    return result;
  }
  if (!released) {
    // Still programming, or latched holding DO low. Either way the steps below cannot be received,
    // and the caller's mount would fail for this reason and report it as a dead card.
    io.cs(false);
    io.xfer(SD_SPI_IDLE_BYTE);
    result.outcome = CardResetOutcome::STILL_BUSY;
    return result;
  }

  // --- Step 1: 80 clocks with CS deasserted ---------------------------------------------------
  result.reached = CardResetStep::CLOCK_OUT;
  io.cs(false);
  for (uint8_t i = 0; i < 10; i++)
    io.xfer(SD_SPI_IDLE_BYTE);

  // --- Step 2: end the multi-block write ------------------------------------------------------
  // A card with no open write ignores this token; a card inside CMD25 has been waiting for it since
  // before the reset. One idle byte precedes it (the token must land on a byte boundary) and one
  // follows it (the card takes a byte before it asserts busy) — both per the SPI-mode write
  // sequence, and both cheap enough that skipping them to save 16 clocks would be a poor trade.
  result.reached = CardResetStep::STOP_TRAN;
  io.cs(true);
  io.xfer(SD_SPI_IDLE_BYTE);
  io.xfer(SD_SPI_STOP_TRAN_TOKEN);
  io.xfer(SD_SPI_IDLE_BYTE);
  sd_wait_not_busy(io, cfg.busy_timeout_ms, &result.busy_after_stop_ms, &result.saw_busy);
  if (io.aborted()) {
    io.cs(false);
    result.outcome = CardResetOutcome::ABORTED;
    return result;
  }

  // --- Step 3: CMD12, the same abort for a read left open -------------------------------------
  // Outside a transfer this is an illegal-command error, which is a perfectly good answer: it means
  // the card is listening. Nothing branches on it.
  result.reached = CardResetStep::STOP_CMD;
  sd_send_command(io, 12, 0);
  sd_wait_not_busy(io, cfg.busy_timeout_ms, nullptr, &result.saw_busy);

  // --- Step 4: CMD0 --------------------------------------------------------------------------
  result.reached = CardResetStep::GO_IDLE;
  for (uint8_t attempt = 0; attempt < cfg.cmd0_attempts; attempt++) {
    io.cs(false);
    for (uint8_t i = 0; i < 10; i++)
      io.xfer(SD_SPI_IDLE_BYTE);
    io.cs(true);
    result.cmd0_tries = static_cast<uint8_t>(attempt + 1);
    result.r1 = sd_send_command(io, 0, 0);
    if (result.r1 == SD_SPI_R1_IDLE)
      break;
    if (result.r1 != 0xFF)
      result.saw_busy = true;  // it answered something: the card is present, just not idle yet
    io.cs(false);
    io.delay_ms(cfg.cmd0_gap_ms);
    if (io.aborted()) {
      result.outcome = CardResetOutcome::ABORTED;
      return result;
    }
  }

  if (result.r1 != SD_SPI_R1_IDLE) {
    io.cs(false);
    io.xfer(SD_SPI_IDLE_BYTE);
    result.outcome = result.saw_busy ? CardResetOutcome::NO_RESPONSE : CardResetOutcome::MUTE;
    return result;
  }

  // --- Step 5: what does the card say about itself? -------------------------------------------
  // R2: two bytes, the second carrying CC error / card ECC failed / locked. Logged, never acted on:
  // a status byte cannot tell us to do anything the caller is not already doing, but it is the
  // difference between "the card is failing" and "the card is fine and the FAT is not".
  result.reached = CardResetStep::STATUS;
  const uint8_t r2_first = sd_send_command(io, 13, 0);
  if (r2_first != 0xFF) {
    result.status = io.xfer(SD_SPI_IDLE_BYTE);
    result.status_valid = true;
  }

  // Deselect and clock one more byte: the card releases DO synchronously to SCLK, so without this
  // byte it keeps driving the line after CS goes high and the next host to use the bus sees it.
  io.cs(false);
  io.xfer(SD_SPI_IDLE_BYTE);
  result.outcome = CardResetOutcome::IDLE;
  return result;
}

/// For the one log line that has to say what happened.
inline const char *card_reset_outcome_str(CardResetOutcome outcome) {
  switch (outcome) {
    case CardResetOutcome::IDLE:
      return "idle";
    case CardResetOutcome::STILL_BUSY:
      return "still busy";
    case CardResetOutcome::MUTE:
      return "mute";
    case CardResetOutcome::NO_RESPONSE:
      return "no response";
    case CardResetOutcome::ABORTED:
      return "aborted";
  }
  return "?";
}

inline const char *card_reset_step_str(CardResetStep step) {
  switch (step) {
    case CardResetStep::NONE:
      return "none";
    case CardResetStep::BUSY_WAIT:
      return "busy wait";
    case CardResetStep::CLOCK_OUT:
      return "clock out";
    case CardResetStep::STOP_TRAN:
      return "stop tran";
    case CardResetStep::STOP_CMD:
      return "cmd12";
    case CardResetStep::GO_IDLE:
      return "cmd0";
    case CardResetStep::STATUS:
      return "cmd13";
  }
  return "?";
}

}  // namespace sd_logger
}  // namespace esphome
