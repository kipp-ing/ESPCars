#pragma once

/// `#ident` (docs/sd_logger-spec.md §6 F1j): every file names the device, the build and the date it
/// was written with. A chunk name is not an identity — `seq` is per card and restarts on a fresh
/// card, cards move between devices, devices get re-flashed — so the file has to say it itself.
///
/// ESPHome-free and IDF-free like `log_format.h`, so the exact line is host-tested: the device side
/// only gathers the values (`SdLogger::capture_ident_()`), and everything about their spelling lives
/// here.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "clock_basis.h"
#include "log_format.h"

namespace esphome {
namespace sd_logger {

/// Hex digits of the ELF SHA-256 the line carries. 64 bits: enough to tell any two builds apart. The
/// boot log's `ELF file SHA256` line shows a shorter prefix of the same digest (9 digits by default).
static const size_t SD_LOG_IDENT_ELF_CHARS = 16;

/// What `#ident` states. Captured once (MAC, build, ELF, config hash, project in `setup()`; the CID
/// at mount), so writing the line costs no lookups. Every string is borrowed: the node name and the
/// project strings are static for the life of the firmware.
struct FileIdent {
  const char *id{nullptr};  ///< ESPHome node name
  uint8_t mac[6]{};
  bool has_mac{false};
  uint64_t build_s{0};                     ///< UTC seconds; 0 = unknown
  char elf[SD_LOG_IDENT_ELF_CHARS + 1]{};  ///< lowercase hex prefix; empty = unknown
  uint32_t cfg{0};
  bool has_cfg{false};
  const char *project{nullptr};  ///< `esphome: project: name`, or nullptr
  const char *project_version{nullptr};
  bool has_card{false};
  uint8_t card_mid{0};
  uint16_t card_oid{0};
  uint32_t card_psn{0};
};

// The line spells exactly six octets. An EUI-64 has no place in it (see `ident_set_mac`).
static_assert(sizeof(FileIdent::mac) == 6, "#ident's mac is a MAC-48");

namespace ident_detail {

static const char LOWER_HEX[] = "0123456789abcdef";

/// Exactly `digits` lowercase hex digits, zero-padded: these are names (MAC, CID, hashes), spelled
/// the way the tools that print them elsewhere spell them.
inline void hex_fixed(LineWriter &w, uint64_t v, unsigned digits) {
  for (unsigned i = digits; i > 0; i--)
    w.ch(LOWER_HEX[(v >> (4 * (i - 1))) & 0xF]);
}

inline void dec_fixed(LineWriter &w, uint64_t v, unsigned digits) {
  char tmp[20];
  for (unsigned i = 0; i < digits; i++) {
    tmp[digits - 1 - i] = static_cast<char>('0' + v % 10);
    v /= 10;
  }
  // A year past 9999 keeps its leading digits rather than being silently cut to four.
  if (v != 0)
    w.u64(v);
  for (unsigned i = 0; i < digits; i++)
    w.ch(tmp[i]);
}

}  // namespace ident_detail

/// `mac`: the 6-byte MAC-48 ESPHome reports as the device's (`get_mac_address_raw()`, the WiFi
/// "Local MAC"). The array reference is the point: an 8-byte buffer cannot bind to it. On chips with
/// IEEE 802.15.4 (ESP32-C6, -H2) `esp_efuse_mac_get_default()` returns the EUI-64 — the MAC-48 with
/// `ff:fe` inserted after the OUI — so its first six bytes are `aa:bb:cc:ff:fe:dd`, not the MAC, and
/// the two extra bytes land past a 6-byte buffer (field test, 2026-10-11). All zeros is ESPHome's
/// "could not read one" and writes `mac=-`.
inline void ident_set_mac(FileIdent &ident, const uint8_t (&mac)[6]) {
  bool any = false;
  for (size_t i = 0; i < 6; i++) {
    ident.mac[i] = mac[i];
    any = any || mac[i] != 0;
  }
  ident.has_mac = any;
}

/// `elf`: the first SD_LOG_IDENT_ELF_CHARS hex digits of the binary ELF SHA-256 (the app
/// descriptor's `app_elf_sha256`). Not `esp_app_get_elf_sha256()`: that copies from a RAM string IDF
/// cuts to CONFIG_APP_RETRIEVE_LEN_ELF_SHA characters (default 9), whatever buffer it is given — which
/// is how the field test saw `elf=f81b322c5`. `nullptr` or no bytes writes `elf=-`.
inline void ident_set_elf(FileIdent &ident, const uint8_t *digest, size_t len) {
  size_t n = 0;
  for (size_t i = 0; digest != nullptr && i < len && n + 2 <= SD_LOG_IDENT_ELF_CHARS; i++) {
    ident.elf[n++] = ident_detail::LOWER_HEX[digest[i] >> 4];
    ident.elf[n++] = ident_detail::LOWER_HEX[digest[i] & 0xF];
  }
  ident.elf[n] = '\0';
}

/// `YYYY-MM-DDTHH:MM:SSZ` for UTC seconds since the epoch. Days to civil date by Howard Hinnant's
/// `civil_from_days` (http://howardhinnant.github.io/date_algorithms.html#civil_from_days),
/// unsigned because the input is never before 1970: no `gmtime_r`, no TZ, no locale.
inline void write_iso8601_utc(LineWriter &w, uint64_t utc_s) {
  using ident_detail::dec_fixed;
  const uint64_t days = utc_s / 86400;
  const uint64_t sod = utc_s % 86400;
  const uint64_t z = days + 719468;
  const uint64_t era = z / 146097;
  const uint64_t doe = z - era * 146097;                                       // [0, 146096]
  const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const uint64_t mp = (5 * doy + 2) / 153;                                     // [0, 11], March-based
  const uint64_t day = doy - (153 * mp + 2) / 5 + 1;
  const uint64_t month = mp < 10 ? mp + 3 : mp - 9;
  const uint64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  dec_fixed(w, year, 4);
  w.ch('-');
  dec_fixed(w, month, 2);
  w.ch('-');
  dec_fixed(w, day, 2);
  w.ch('T');
  dec_fixed(w, sod / 3600, 2);
  w.ch(':');
  dec_fixed(w, (sod / 60) % 60, 2);
  w.ch(':');
  dec_fixed(w, sod % 60, 2);
  w.ch('Z');
}

/// `#ident,id=…,mac=…,date=…,clock=…,build=…,elf=…,cfg=…[,project=…][,card=…]` (F1j).
///
/// `now` is the header's `#clock` statement already projected to the file-open instant, so `date`
/// and `#clock` can never disagree; an invalid point writes `date=-,clock=none` rather than a date
/// the device does not have. The always-present keys come first and the optional strings last, so a
/// line cut at the cap loses the least.
inline size_t format_ident(char *out, size_t room, const FileIdent &ident, const ClockPoint &now) {
  using ident_detail::hex_fixed;
  if (room < SD_LOG_MIN_ROOM)
    return 0;
  LineWriter w(out, room < SD_LOG_MAX_LINE ? room : SD_LOG_MAX_LINE);
  w.str("#ident,id=");
  const char *id = ident.id == nullptr ? "" : ident.id;
  w.escaped(id, std::strlen(id), /*comma_to_underscore=*/true);

  w.str(",mac=");
  if (ident.has_mac) {
    for (size_t i = 0; i < 6; i++) {
      if (i != 0)
        w.ch(':');
      hex_fixed(w, ident.mac[i], 2);
    }
  } else {
    w.ch('-');
  }

  w.str(",date=");
  if (now.valid()) {
    write_iso8601_utc(w, now.utc_us / 1000000ull);
    w.str(",clock=");
    w.str(clock_basis_str(now.basis));
    w.ch('/');
    w.str(clock_src_str(now.src));
  } else {
    w.str("-,clock=none");
  }

  w.str(",build=");
  if (ident.build_s != 0)
    write_iso8601_utc(w, ident.build_s);
  else
    w.ch('-');

  w.str(",elf=");
  size_t elf_len = 0;
  // Hex only, whatever the buffer holds: this field is matched against other tools' output, and a
  // stray byte here would make it match nothing without anyone noticing.
  while (elf_len < SD_LOG_IDENT_ELF_CHARS && ident.elf[elf_len] != '\0' &&
         std::strchr(ident_detail::LOWER_HEX, ident.elf[elf_len]) != nullptr)
    elf_len++;
  if (elf_len > 0)
    w.raw(ident.elf, elf_len);
  else
    w.ch('-');

  w.str(",cfg=");
  if (ident.has_cfg)
    hex_fixed(w, ident.cfg, 8);
  else
    w.ch('-');

  if (ident.project != nullptr && ident.project[0] != '\0') {
    w.str(",project=");
    w.escaped(ident.project, std::strlen(ident.project), /*comma_to_underscore=*/true);
    if (ident.project_version != nullptr && ident.project_version[0] != '\0') {
      w.ch('@');
      w.escaped(ident.project_version, std::strlen(ident.project_version), /*comma_to_underscore=*/true);
    }
  }

  if (ident.has_card) {
    w.str(",card=");
    hex_fixed(w, ident.card_mid, 2);
    w.ch('-');
    hex_fixed(w, ident.card_oid, 4);
    w.ch('-');
    hex_fixed(w, ident.card_psn, 8);
  }
  return w.finish();
}

}  // namespace sd_logger
}  // namespace esphome
