#pragma once

/// TapRecord (can_gateway, written in the RX ISR) -> LogRecord (sd_logger, written to the card).
///
/// This lives in its own ESPHome-free, IDF-free header for the same reason `gateway_core.h` does:
/// so the host harness in `tests/host/` can run it. The bench cannot reach this translation
/// usefully — a wrong flag bit produces a log that is perfectly well-formed and quietly wrong,
/// which is exactly the failure a hardware run does not catch.
///
/// The hazard it exists to contain:
///
///     can_gateway::TAP_FLAG_SHED == 0x04       "received but not forwarded"
///     sd_logger::REC_FLAG_TX     == 0x04       "this node transmitted it"
///
/// Same bit, opposite meanings. So the two records are translated field by field and **never**
/// memcpy'd, even though they are near-identical in shape. `source` differs too: the tap carries
/// the gateway's port index, the log carries the tag the user assigned that port in `can_ports:`.

#include <cstdint>
#include <cstring>

// This header must compile to nothing in a build without the tap. The generated
// esphome.h includes every component header unconditionally, so an unguarded
// include of can_gateway would break any sd_logger build that does not also
// pull in can_gateway — which is most of them.
#ifdef SD_LOGGER_HOST_TEST
#include "gateway_core.h"
#define SD_LOGGER_TAP_TRANSLATE
#else
#include "esphome/core/defines.h"
#ifdef USE_SD_LOGGER_CAN_TAP
#include "esphome/components/can_gateway/gateway_core.h"
#define SD_LOGGER_TAP_TRANSLATE
#endif
#endif

#ifdef SD_LOGGER_TAP_TRANSLATE

#include "log_record.h"

namespace esphome {
namespace sd_logger {

/// Translate one tap record. `source` is the logger's tag for the originating port, not the
/// gateway's port index. Every field of `out` is written, so an uninitialised LogRecord is fine.
inline void tap_to_log_record(const can_gateway::TapRecord &tap, uint8_t source, LogRecord &out) {
  out.t_us = tap.t_us;
  out.id = tap.can_id;
  out.source = source;
  out.flags = 0;
  if (tap.flags & can_gateway::TAP_FLAG_EXTENDED)
    out.flags |= REC_FLAG_EXTENDED;
  if (tap.flags & can_gateway::TAP_FLAG_RTR)
    out.flags |= REC_FLAG_RTR;
  if (tap.flags & can_gateway::TAP_FLAG_SHED)
    out.flags |= REC_FLAG_SHED;
  // REC_FLAG_TX is never set here: a tap record is by definition something this node received.
  out.len = tap.dlc > 8 ? 8 : tap.dlc;
  std::memcpy(out.data, tap.data, sizeof(out.data));
}

}  // namespace sd_logger
}  // namespace esphome

#endif  // SD_LOGGER_TAP_TRANSLATE
