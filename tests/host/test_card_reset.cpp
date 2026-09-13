// The in-band card reset (components/sd_logger/card_reset.h).
//
// Why this is a host test and not a bench check: staging one case costs a real wedge. You reset the
// board mid-write, hope the card lands in the state you wanted, and then wait out a retry ladder to
// see what happened — and the card, by definition, cannot record any of it. Every case below is a
// card state that has cost this project a bench session at least once:
//
//   * a card sitting in an open CMD25, which answers no command until the stop token arrives —
//     the wedge itself, and the reason CMD0-and-clock-pulses alone never cleared it;
//   * a card that is merely busy for longer than the driver's fixed 40 ms, which is
//     indistinguishable from the above at the mount API and needs the opposite fix (patience);
//   * a card that is absent or unpowered, which must never be reported as "wedged" — that
//     confusion sent three sessions after the cabling;
//   * a busy card that lets a single non-zero byte slip out mid-window, which a one-byte ready
//     test accepts and then talks over.
//
// The fake card checks CMD0's CRC against the spec's 0x95, so a broken sd_crc7() fails here rather
// than on the bench as "the card ignored us".

#include "harness.h"
#include "card_reset.h"

#include <cstdint>
#include <vector>

using esphome::sd_logger::card_reset;
using esphome::sd_logger::CardResetConfig;
using esphome::sd_logger::CardResetIo;
using esphome::sd_logger::CardResetOutcome;
using esphome::sd_logger::CardResetStep;
using esphome::sd_logger::sd_crc7;
using esphome::sd_logger::SD_SPI_STOP_TRAN_TOKEN;

namespace {

/// A microSD card in SPI mode, as much of one as these cases need.
///
/// Time advances one millisecond per byte clocked, which is not a real bus rate but makes every
/// duration in a case readable as "bytes of patience". The card drives the bus only while selected,
/// exactly like the real thing — that is what makes the "measure busy with CS asserted" ordering in
/// card_reset() a testable property rather than a comment.
class FakeCard : public CardResetIo {
 public:
  struct Config {
    /// An aborted CMD25 is still open: the card is in Receive-Data state and treats every byte as
    /// data until it sees the stop token. This is the wedge.
    bool open_write{false};
    uint32_t busy_ms{0};             ///< DO held low from t=0
    uint32_t busy_after_stop_ms{0};  ///< programming time after the stop token
    bool answers_cmd0{true};         ///< false: replies "illegal command" forever
    bool present{true};              ///< false: nothing drives MISO, every read is 0xFF
    uint8_t status_byte{0};          ///< CMD13 R2 second byte
    uint32_t glitch_at_ms{0};        ///< if non-zero, one stray 0xFF at this time while busy
    uint32_t abort_at_ms{0};         ///< if non-zero, aborted() turns true at this time
  };

  explicit FakeCard(Config cfg) : cfg_(cfg), open_write_(cfg.open_write), busy_until_(cfg.busy_ms) {}

  void cs(bool assert) override { selected_ = assert; }

  uint8_t xfer(uint8_t out) override {
    now_ms_++;
    bytes_++;
    if (!cfg_.present || !selected_)
      return 0xFF;  // the card drives DO only when selected; an absent one never does

    if (now_ms_ < busy_until_) {
      // One stray non-zero byte inside the busy window: a real card can be sampled mid-transition,
      // which is why the ready test wants two in a row.
      if (cfg_.glitch_at_ms != 0 && now_ms_ == cfg_.glitch_at_ms)
        return 0xFF;
      return 0x00;
    }

    if (open_write_) {
      // Receive-Data state. Command frames are consumed as write data and answered by nothing.
      if (out == SD_SPI_STOP_TRAN_TOKEN) {
        open_write_ = false;
        stop_token_seen_ = true;
        busy_until_ = now_ms_ + cfg_.busy_after_stop_ms;
      }
      return 0xFF;
    }

    if (resp_pos_ < resp_.size())
      return resp_[resp_pos_++];

    if (frame_len_ > 0 || (out & 0xC0) == 0x40) {
      frame_[frame_len_++] = out;
      if (frame_len_ == 6) {
        handle_command_();
        frame_len_ = 0;
      }
    }
    return 0xFF;
  }

  void delay_ms(uint32_t ms) override { now_ms_ += ms; }
  uint32_t now_ms() override { return now_ms_; }
  bool aborted() override { return cfg_.abort_at_ms != 0 && now_ms_ >= cfg_.abort_at_ms; }

  bool stop_token_seen() const { return stop_token_seen_; }
  bool open_write() const { return open_write_; }
  uint32_t bytes() const { return bytes_; }
  uint8_t commands_seen() const { return commands_; }

 private:
  void handle_command_() {
    const uint8_t index = frame_[0] & 0x3F;
    commands_++;
    resp_.clear();
    resp_pos_ = 0;
    switch (index) {
      case 0:
        // The spec's own CRC for CMD0 with a zero argument. Hard-coded rather than recomputed with
        // the function under test, so a wrong sd_crc7() cannot agree with itself.
        if (frame_[5] != 0x95) {
          resp_.push_back(0x09);  // CRC error + idle
          break;
        }
        resp_.push_back(cfg_.answers_cmd0 ? 0x01 : 0x04);
        break;
      case 12:
        resp_.push_back(0x00);
        break;
      case 13:
        resp_.push_back(0x00);
        resp_.push_back(cfg_.status_byte);
        break;
      default:
        resp_.push_back(0x04);  // illegal command
        break;
    }
  }

  Config cfg_;
  bool selected_{false};
  bool open_write_{false};
  bool stop_token_seen_{false};
  uint32_t now_ms_{0};
  uint32_t busy_until_{0};
  uint32_t bytes_{0};
  uint8_t commands_{0};
  uint8_t frame_[6]{};
  uint8_t frame_len_{0};
  std::vector<uint8_t> resp_;
  size_t resp_pos_{0};
};

CardResetConfig fast_config(uint32_t busy_timeout_ms = 2000) {
  CardResetConfig cfg;
  cfg.busy_timeout_ms = busy_timeout_ms;
  cfg.cmd0_attempts = 4;
  cfg.cmd0_gap_ms = 10;
  return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------------------- CRC7

TEST(card_reset_crc7_matches_the_spec_vectors) {
  // The two frames whose CRC the card actually checks in SPI mode. If either of these is wrong the
  // card answers "CRC error" to the one command that is supposed to reset it, and the recovery
  // fails for a reason entirely of our own making.
  const uint8_t cmd0[5] = {0x40, 0x00, 0x00, 0x00, 0x00};
  CHECK_EQ(sd_crc7(cmd0, 5), static_cast<uint8_t>(0x95));

  const uint8_t cmd8[5] = {0x48, 0x00, 0x00, 0x01, 0xAA};
  CHECK_EQ(sd_crc7(cmd8, 5), static_cast<uint8_t>(0x87));
}

// ------------------------------------------------------------------------------- the happy paths

TEST(card_reset_clean_card_reaches_idle) {
  FakeCard card{FakeCard::Config{}};
  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
  CHECK_EQ(static_cast<int>(result.reached), static_cast<int>(CardResetStep::STATUS));
  CHECK_EQ(result.r1, static_cast<uint8_t>(0x01));
  CHECK_EQ(result.cmd0_tries, static_cast<uint8_t>(1));
  // Not zero: proving a card is ready costs the two byte-times the criterion asks for. What matters
  // is that nothing was *waited* for.
  CHECK(result.busy_ms <= 2u);
  CHECK(!result.saw_busy);
}

TEST(card_reset_waits_out_a_busy_card) {
  // 500 ms of programming against a 2 s budget: the case the driver's fixed 40 ms cannot survive.
  FakeCard::Config cfg;
  cfg.busy_ms = 500;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
  CHECK(result.saw_busy);
  CHECK(result.busy_ms >= 500u);
  CHECK(result.busy_ms < 520u);
}

TEST(card_reset_needs_two_nonzero_bytes_to_call_a_card_ready) {
  // A single 0xFF inside the busy window must not end the wait: the next command would land in a
  // card that is still programming, and the whole sequence would fail one step later for a reason
  // that looks nothing like this one.
  FakeCard::Config cfg;
  cfg.busy_ms = 400;
  cfg.glitch_at_ms = 120;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
  CHECK(result.busy_ms >= 400u);
}

// -------------------------------------------------------------------------------------- the wedge

TEST(card_reset_ends_an_open_multi_block_write) {
  // THE case. A card left inside CMD25 answers no command — not CMD0, not after any number of
  // clock pulses — until the stop token arrives. If the token is ever removed from card_reset(),
  // this card stays in Receive-Data state and the outcome becomes NO_RESPONSE.
  FakeCard::Config cfg;
  cfg.open_write = true;
  cfg.busy_after_stop_ms = 80;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK(card.stop_token_seen());
  CHECK(!card.open_write());
  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
  CHECK(result.busy_after_stop_ms >= 80u);
  CHECK_EQ(result.r1, static_cast<uint8_t>(0x01));
}

TEST(card_reset_open_write_that_is_also_busy) {
  // The realistic shape of a reset landing mid-CMD25: the card is still programming the block it
  // had accepted, *and* the write is open. Busy is waited out first, then the token lands.
  FakeCard::Config cfg;
  cfg.open_write = true;
  cfg.busy_ms = 300;
  cfg.busy_after_stop_ms = 50;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK(result.saw_busy);
  CHECK(result.busy_ms >= 300u);
  CHECK(card.stop_token_seen());
  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
}

// ------------------------------------------------------------------------------- the failure modes

TEST(card_reset_reports_still_busy_rather_than_dead) {
  // Busy for longer than the budget. This must NOT read as a dead card: the correct response is a
  // longer budget on the next attempt, which is the opposite of what "mute" would call for.
  FakeCard::Config cfg;
  cfg.busy_ms = 5000;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config(/*busy_timeout_ms=*/600));

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::STILL_BUSY));
  CHECK_EQ(static_cast<int>(result.reached), static_cast<int>(CardResetStep::BUSY_WAIT));
  CHECK(result.saw_busy);
  CHECK(result.busy_ms >= 600u);
  CHECK_EQ(card.commands_seen(), static_cast<uint8_t>(0));
}

TEST(card_reset_absent_card_is_mute_not_busy) {
  // Nothing drives MISO. Reporting this as a wedge is how a power/transceiver fault gets chased as
  // a cabling fault; `saw_busy` is the bit that separates them.
  FakeCard::Config cfg;
  cfg.present = false;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::MUTE));
  CHECK(!result.saw_busy);
  CHECK_EQ(result.r1, static_cast<uint8_t>(0xFF));
  CHECK_EQ(result.cmd0_tries, static_cast<uint8_t>(4));  // every attempt spent
}

TEST(card_reset_card_that_answers_but_never_idles) {
  // Present, listening, and refusing CMD0. Not mute — the distinction the log line has to make.
  FakeCard::Config cfg;
  cfg.answers_cmd0 = false;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::NO_RESPONSE));
  CHECK(result.saw_busy);
  CHECK_EQ(result.r1, static_cast<uint8_t>(0x04));
  CHECK_EQ(result.cmd0_tries, static_cast<uint8_t>(4));
}

TEST(card_reset_reports_the_status_byte) {
  // CMD13's second R2 byte: bit3 is the card's own "internal controller error". A recovery that
  // succeeds against a card reporting this is worth knowing about before the next drive.
  FakeCard::Config cfg;
  cfg.status_byte = 0x08;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config());

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::IDLE));
  CHECK(result.status_valid);
  CHECK_EQ(result.status, static_cast<uint8_t>(0x08));
}

// ------------------------------------------------------------------------------------- shutdown

TEST(card_reset_aborts_mid_busy_wait) {
  // on_shutdown() waits 200 ms for the writer. A ten-second busy wait that ignored `dying_` would
  // eat the `#close` line, which is the one piece of evidence a clean stop leaves on the card.
  FakeCard::Config cfg;
  cfg.busy_ms = 100000;
  cfg.abort_at_ms = 150;
  FakeCard card{cfg};

  const auto result = card_reset(card, fast_config(/*busy_timeout_ms=*/60000));

  CHECK_EQ(static_cast<int>(result.outcome), static_cast<int>(CardResetOutcome::ABORTED));
  CHECK(card.bytes() < 400u);  // it stopped when asked, not when the budget ran out
}
