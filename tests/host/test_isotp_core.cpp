// Frame-level exchanges between TxTransfer and RxTransfer (components/isotp/isotp_core.h).
//
// This file exists because the bench found a hole no other tier covered: on
// 2026-07-30 every ISO-TP response that fits within ONE flow-control block
// (~62 bytes at block size 8) decoded, and every response needing a SECOND
// flow control — sizes a real defensive telemetry response reaches easily —
// never arrived. isotp_core.h had no host tests at all at that point: the "61
// consecutive frames, 8 flow controls" transfer had literally never run
// anywhere before the bench asked for it. These cases make the core's two
// machines meet each other at frame granularity, so whatever the bench shows
// is attributable: if they pass here, the core is exonerated and the defect
// lives in the glue (isotp.cpp / can_gateway); if they fail here, the core is
// convicted with a red case in hand.
//
// The payload is the responder rig's deterministic pattern (byte i = i*7+3),
// for the rig's reason: a reassembly slip is a specific wrong byte, not a
// plausible number.

#include "harness.h"
#include "isotp_core.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace esphome::isotp;

namespace {

struct ExchangeResult {
  bool complete{false};
  IsoTpError tx_error{IsoTpError::NONE};
  IsoTpError rx_error{IsoTpError::NONE};
  uint16_t delivered{0};
  bool bytes_ok{false};
  uint32_t fc_count{0};   // flow controls the receiver issued
  uint32_t frames{0};     // data frames the sender put on the "wire"
  uint32_t elapsed_ms{0};
};

/// Run one message sender-to-receiver, one frame per simulated millisecond,
/// with the receiver granting `block_size`/`st_min_raw`. `drop_fc_after`
/// simulates the glue losing every flow control after the Nth (0 = lose none).
ExchangeResult exchange(uint16_t size, uint8_t block_size, uint8_t st_min_raw, uint32_t drop_fc_after = 0) {
  IsoTpConfig tx_cfg;  // the sender learns pacing from the peer's FC, not from its own config
  IsoTpConfig rx_cfg;
  rx_cfg.block_size = block_size;
  rx_cfg.st_min_raw = st_min_raw;

  static uint8_t tx_buf[512], rx_buf[512];
  TxTransfer tx;
  tx.init(tx_buf, sizeof(tx_buf));
  RxTransfer rx;
  rx.init(rx_buf, sizeof(rx_buf));

  std::vector<uint8_t> msg(size);
  for (uint16_t i = 0; i < size; i++)
    msg[i] = static_cast<uint8_t>(i * 7 + 3);

  ExchangeResult r;
  uint32_t now = 1000;
  if (!tx.begin(msg.data(), size, now))
    return r;

  // Generous bound: 430 bytes at STmin 20 ms is ~1.3 s of simulated time.
  for (uint32_t step = 0; step < 200000; step++) {
    Frame f;
    const TxAction ta = tx.poll(tx_cfg, now, &f);
    if (ta == TxAction::SEND_FRAME) {
      tx.confirm_sent(tx_cfg, now);  // the host wire never refuses a frame
      r.frames++;
      Frame fc;
      const RxAction ra = rx.on_frame(rx_cfg, f.data, f.dlc, now, &fc);
      if (ra == RxAction::SEND_FLOW_CONTROL) {
        r.fc_count++;
        if (drop_fc_after == 0 || r.fc_count <= drop_fc_after) {
          const PciInfo info = decode_pci(tx_cfg, fc.data, fc.dlc);
          tx.on_flow_control(info, now);
        }
      } else if (ra == RxAction::MESSAGE_COMPLETE) {
        r.complete = true;
        r.delivered = rx.size();
        r.bytes_ok = r.delivered == size && std::memcmp(rx.data(), msg.data(), size) == 0;
        r.elapsed_ms = now - 1000;
        return r;
      } else if (ra == RxAction::FAILED) {
        r.rx_error = rx.error();
        return r;
      }
    } else if (ta == TxAction::COMPLETE) {
      // Single frames complete on the TX side in the same step the RX delivers;
      // reaching here means the RX did not deliver, which the checks will show.
    } else if (ta == TxAction::FAILED) {
      r.tx_error = tx.error();
      // Keep polling the receiver's clock so its own timeout is also observable.
    }
    const RxAction rp = rx.poll(rx_cfg, now);
    if (rp == RxAction::FAILED) {
      r.rx_error = rx.error();
      return r;
    }
    if (ta == TxAction::FAILED)
      return r;
    now += 1;
  }
  return r;
}

}  // namespace

TEST(isotp_core_single_frame_roundtrip) {
  const ExchangeResult r = exchange(7, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 1u);
  CHECK_EQ(r.fc_count, 0u);
}

TEST(isotp_core_one_block_17_bytes_the_size_the_bench_proved) {
  // 17 bytes = FF(6) + 2 CFs: one flow control total.
  const ExchangeResult r = exchange(17, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 3u);
  CHECK_EQ(r.fc_count, 1u);
}

TEST(isotp_core_exactly_one_full_block_62_bytes) {
  // FF(6) + 8 CFs x 7 = 62: the last CF of the block is also the last of the
  // message. Neither side may demand or emit a further flow control.
  const ExchangeResult r = exchange(62, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 9u);
  CHECK_EQ(r.fc_count, 1u);
}

TEST(isotp_core_second_block_begins_at_63_bytes) {
  // The smallest message that needs a SECOND flow control — the first size the
  // bench saw die. 9 CFs: 8 in block one, 1 in block two.
  const ExchangeResult r = exchange(63, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 10u);
  CHECK_EQ(r.fc_count, 2u);
}

TEST(isotp_core_four_flow_controls_192_bytes) {
  // A 192-byte response: FF(6) + 27 CFs, flow controls after the FF and
  // after each of blocks 1..3 — 4 in total. This is the size class of a
  // real array-shaped response the rig can reproduce on demand.
  const ExchangeResult r = exchange(192, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 28u);
  CHECK_EQ(r.fc_count, 4u);
}

TEST(isotp_core_eight_flow_controls_430_bytes) {
  // A 430-byte response: FF(6) + 61 CFs, 8 flow controls — the full stretch
  // the handover names, at the rig's granted pacing.
  const ExchangeResult r = exchange(430, 8, 20);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.frames, 62u);
  CHECK_EQ(r.fc_count, 8u);
  // STmin pacing: the first CF of each block is deliberately exempt (the FC
  // itself spaced it), so 61 CFs in 8 blocks means 53 paced gaps of 20 ms.
  // If this drops to ~0 the pacing died; if it explodes the block bookkeeping
  // is resending.
  CHECK(r.elapsed_ms >= 53u * 20u);
}

TEST(isotp_core_block_size_1_paces_every_frame) {
  // BS 1 is the most FC-hungry grant: one flow control per consecutive frame.
  const ExchangeResult r = exchange(63, 1, 0);
  CHECK(r.complete);
  CHECK(r.bytes_ok);
  CHECK_EQ(r.fc_count, 9u);
}

TEST(isotp_core_a_lost_second_fc_is_TIMEOUT_FC_then_TIMEOUT_CF) {
  // The glue failing to transmit flow control #2 must surface on BOTH sides,
  // with these exact errors — this is the symptom signature to look for in a
  // bench log whenever a multi-block transfer dies silently.
  const ExchangeResult r = exchange(63, 8, 20, /*drop_fc_after=*/1);
  CHECK(!r.complete);
  CHECK_EQ(static_cast<int>(r.tx_error), static_cast<int>(IsoTpError::TIMEOUT_FC));
  CHECK_EQ(static_cast<int>(r.rx_error), static_cast<int>(IsoTpError::TIMEOUT_CF));
}
