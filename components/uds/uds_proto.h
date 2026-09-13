#pragma once

/// Freestanding UDS request assembly and response classification, per docs/DESIGN-uds.md §4.
/// No ESPHome, no ESP-IDF, no allocation, no exceptions. This layer never touches the wire: it
/// copies request bytes into a caller buffer and renders a verdict on a reassembled response —
/// what to *do* about the verdict (extend a deadline, back off, stop polling) is the component's
/// and the scheduler's business.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "uds_catalog.h"

namespace esphome::uds {

/// First byte of every negative response: `7F <SID> <NRC>`.
static constexpr uint8_t UDS_NEGATIVE_SID = 0x7F;
/// A positive response echoes the request SID plus 0x40.
static constexpr uint8_t UDS_POSITIVE_OFFSET = 0x40;

// -------------------------------------------------------------------------------------------
// Request assembly
// -------------------------------------------------------------------------------------------

/// Copy the group's request bytes into the caller's buffer (isotp's TxTransfer::begin copies
/// again into its own send buffer, so borrowing the catalog pointer directly would also work —
/// this exists so the glue never hands flash pointers across component boundaries). Returns the
/// request length, 0 on any failure.
///
/// The group's `sid` must equal its first request byte — the format defines it that way (§4) and
/// a blob where they diverge has a compiler bug that response validation would then judge with
/// the wrong SID. Refused here, where it is cheap, rather than debugged on the bus.
inline uint8_t build_request(const Catalog &cat, const GroupView &g, uint8_t *out, size_t out_cap) {
  if (out == nullptr || g.req_len == 0 || out_cap < g.req_len)
    return 0;
  const uint8_t *req = cat.request_bytes(g);
  if (req == nullptr || req[0] != g.sid)
    return 0;
  std::memcpy(out, req, g.req_len);
  return g.req_len;
}

// -------------------------------------------------------------------------------------------
// Negative response classification (design §4, the NRC table)
// -------------------------------------------------------------------------------------------

/// Reaction classes for negative response codes. Classified rather than lumped together because
/// the correct reaction differs — 0x31 in particular: polling a DID this ECU does not implement
/// forever is how a diagnostic client makes itself a nuisance on a bus.
enum class NrcClass : uint8_t {
  NONE = 0,     ///< not a negative response
  PENDING,      ///< 0x78 — extend the deadline to the catalog's p2_ext_ms, keep waiting
  BUSY,         ///< 0x21, 0x22 — busy / conditions not correct: back off, retry, not an error
  UNSUPPORTED,  ///< 0x31 — permanent: mark the group unsupported, stop polling it, log once
  DENIED,       ///< 0x33, 0x35 — security: unsupported unless a session/security step is configured
  OTHER,        ///< anything else: count, back off
};

inline NrcClass classify_nrc(uint8_t nrc) {
  switch (nrc) {
    case 0x78:
      return NrcClass::PENDING;
    case 0x21:
    case 0x22:
      return NrcClass::BUSY;
    case 0x31:
      return NrcClass::UNSUPPORTED;
    case 0x33:
    case 0x35:
      return NrcClass::DENIED;
    default:
      return NrcClass::OTHER;
  }
}

// -------------------------------------------------------------------------------------------
// Response validation
// -------------------------------------------------------------------------------------------

enum class RespVerdict : uint8_t {
  ACCEPT,     ///< positive, echo verified, at least resp_min_len — every field is covered
  PARTIAL,    ///< positive, echo verified, but shorter than resp_min_len. **A success, not an
              ///< error**: the covered fields decode and the uncovered ones are skipped and
              ///< counted. See the note on resp_min_len below.
  NOT_OURS,   ///< wrong SID or mismatched identifier echo: a stale or foreign answer. Keep waiting
              ///< for ours; on a shared bus this is not a theoretical concern.
  TOO_SHORT,  ///< fewer than `1 + echo_len` bytes, so the SID and its echo cannot be checked. The
              ///< only length that genuinely rejects a response — below it there is nothing to
              ///< verify it against, so it can neither be trusted nor be proven foreign.
  NEGATIVE,   ///< `7F <our SID> <NRC>` — nrc / nrc_class carry the reaction
};

struct ResponseCheck {
  RespVerdict verdict;
  NrcClass nrc_class;  ///< NONE unless verdict == NEGATIVE
  uint8_t nrc;         ///< raw NRC byte when verdict == NEGATIVE, else 0
};

/// How many request bytes after the SID a positive response echoes back, per service.
///
/// The design names only the SID 0x22 case (`resp[1..2] == req[1..2]`); the local-identifier
/// reads echo their one-byte identifier and 0x19 echoes its sub-function. For everything else
/// the echo length is 0 — the SID check alone then guards those responses, which is the honest
/// floor: inventing echo rules for services we never poll would reject real ECUs. Reported as
/// under-specified for non-0x22 services.
inline uint8_t echo_len(uint8_t sid, uint8_t req_len) {
  uint8_t n = 0;
  switch (sid) {
    case 0x22:  // ReadDataByIdentifier: 16-bit DID
      n = 2;
      break;
    case 0x1A:  // ReadEcuIdentification: 8-bit identifier
    case 0x21:  // ReadDataByLocalIdentifier: 8-bit local identifier
    case 0x19:  // ReadDTCInformation: sub-function
      n = 1;
      break;
    default:
      n = 0;
      break;
  }
  const uint8_t avail = req_len > 0 ? static_cast<uint8_t>(req_len - 1) : 0;
  return n < avail ? n : avail;
}

/// Judge a reassembled response against the request it should answer (design §4, in order):
///   1. `resp_len >= 1 + echo_len` — enough to *be* a response. This is the only length that
///      rejects, because below it there is nothing to check the service identifier against;
///   2. positive SID echo: resp[0] == req[0] + 0x40 (or `7F <our SID>` for a negative);
///   3. byte-for-byte identifier echo — the stale-answer trap: an answer to the *previous*
///      request is otherwise indistinguishable from a fresh one.
///
/// **`resp_min_len` is a fast-path hint, not a fourth condition.** At or above it every field of
/// the group is covered, so the decode loop needs no per-field bounds arithmetic; below it the
/// verdict is PARTIAL and the caller decodes what is covered. Treating it as a gate was a defect
/// in the format document, and not a hypothetical one: a real factory database can declare more
/// array elements than a particular unit actually has wired, so a healthy ECU's response can fall
/// short of `resp_min_len` legitimately. As a gate that rejects the whole response, every element
/// reads unavailable against a perfectly healthy unit — the exact class of silent, plausible
/// failure this format exists to avoid. An ECU returning fewer elements than its database declares
/// is ordinary, not malformed.
///
/// `resp_min_len` comes from the group record; pass 0 for a raw exchange with no field claims,
/// which then never yields PARTIAL.
inline ResponseCheck classify_response(const uint8_t *req, size_t req_len, const uint8_t *resp, size_t resp_len,
                                       uint16_t resp_min_len) {
  ResponseCheck r{RespVerdict::NOT_OURS, NrcClass::NONE, 0};
  if (req == nullptr || req_len == 0 || resp == nullptr || resp_len == 0)
    return r;
  const uint8_t sid = req[0];
  if (resp[0] == UDS_NEGATIVE_SID) {
    // `7F <SID> <NRC>`. A 7F echoing a different SID answers someone else's request (NOT_OURS); a
    // 7F too short to carry its NRC is unreadable as a response at all (TOO_SHORT).
    if (resp_len < 3) {
      r.verdict = RespVerdict::TOO_SHORT;
      return r;
    }
    if (resp[1] != sid)
      return r;
    r.verdict = RespVerdict::NEGATIVE;
    r.nrc = resp[2];
    r.nrc_class = classify_nrc(resp[2]);
    return r;
  }
  // The SID is checkable from one byte, so it is judged first: a runt frame carrying somebody
  // else's service is provably NOT_OURS, and reporting it as TOO_SHORT would throw that away.
  if (resp[0] != static_cast<uint8_t>(sid + UDS_POSITIVE_OFFSET))
    return r;
  const uint8_t n = echo_len(sid, req_len > 255 ? 255 : static_cast<uint8_t>(req_len));
  if (resp_len < static_cast<size_t>(1) + n) {
    // Our SID, but too short to carry the echo — so it cannot be verified as the answer to *this*
    // request, and a stale answer to the previous one would be indistinguishable.
    r.verdict = RespVerdict::TOO_SHORT;
    return r;
  }
  if (n != 0 && std::memcmp(resp + 1, req + 1, n) != 0)
    return r;  // right SID, wrong identifier — the stale answer
  r.verdict = resp_len < resp_min_len ? RespVerdict::PARTIAL : RespVerdict::ACCEPT;
  return r;
}

}  // namespace esphome::uds
