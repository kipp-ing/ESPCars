// `#ident` (components/sd_logger/file_ident.h, docs/sd_logger-spec.md §6 F1j).
//
// Host tier, because a wrong identity line is a well-formed line: a MAC with a dropped leading zero,
// a date a day off at a month boundary or an estimate written without its basis all parse, and no
// bench run turns red over them. IDs in the case names refer to F1j's requirement table. All values
// are fictional.

#include "file_ident.h"
#include "harness.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using esphome::sd_logger::ClockBasis;
using esphome::sd_logger::ClockPoint;
using esphome::sd_logger::ClockSrc;
using esphome::sd_logger::FileIdent;
using esphome::sd_logger::format_ident;
using esphome::sd_logger::ident_set_elf;
using esphome::sd_logger::ident_set_mac;
using esphome::sd_logger::LineWriter;
using esphome::sd_logger::SD_LOG_IDENT_ELF_CHARS;
using esphome::sd_logger::SD_LOG_MAX_LINE;
using esphome::sd_logger::write_iso8601_utc;

namespace {

const uint64_t OPEN_S = 1791720003ull;   // 2026-10-11T12:00:03Z
const uint64_t BUILD_S = 1791105153ull;  // 2026-10-04T09:12:33Z

FileIdent full_ident() {
  FileIdent ident;
  ident.id = "logger-bench";
  const uint8_t mac[6] = {0x02, 0x00, 0x5E, 0x10, 0x20, 0x30};
  for (size_t i = 0; i < 6; i++)
    ident.mac[i] = mac[i];
  ident.has_mac = true;
  ident.build_s = BUILD_S;
  std::strcpy(ident.elf, "1a2b3c4d5e6f7a8b");
  ident.cfg = 0x5eed1234;
  ident.has_cfg = true;
  ident.project = "acme.logger";
  ident.project_version = "1.2.0";
  ident.has_card = true;
  ident.card_mid = 0x03;
  ident.card_oid = 0x5344;
  ident.card_psn = 0x0badcafe;
  return ident;
}

ClockPoint at(uint64_t utc_s, ClockBasis basis, ClockSrc src) {
  ClockPoint p;
  p.t_us = 0x64960;
  p.utc_us = utc_s * 1000000ull + 999999;  // the fraction is dropped, never rounded up
  p.basis = basis;
  p.src = src;
  return p;
}

std::string ident_line(const FileIdent &ident, const ClockPoint &now, size_t room = SD_LOG_MAX_LINE) {
  char buf[SD_LOG_MAX_LINE];
  const size_t n = format_ident(buf, room, ident, now);
  return std::string(buf, n);
}

std::string iso(uint64_t utc_s) {
  char buf[64];
  LineWriter w(buf, sizeof(buf));
  write_iso8601_utc(w, utc_s);
  const size_t n = w.finish();
  return std::string(buf, n - 1);  // without the terminator
}

}  // namespace

TEST(ident_i2_full_line_is_the_documented_one) {
  // The spec's own example line (F1j), byte for byte.
  CHECK_EQ(ident_line(full_ident(), at(OPEN_S, ClockBasis::UTC, ClockSrc::SYNC)),
           std::string("#ident,id=logger-bench,mac=02:00:5e:10:20:30,date=2026-10-11T12:00:03Z,clock=utc/sync,"
                       "build=2026-10-04T09:12:33Z,elf=1a2b3c4d5e6f7a8b,cfg=5eed1234,project=acme.logger@1.2.0,"
                       "card=03-5344-0badcafe\n"));
}

TEST(ident_i1_unknowns_are_spelled_out_never_left_out) {
  // Nothing captured at all: the always-present keys are still all there, and the optional ones are
  // not invented.
  CHECK_EQ(ident_line(FileIdent{}, ClockPoint{}),
           std::string("#ident,id=,mac=-,date=-,clock=none,build=-,elf=-,cfg=-\n"));
}

TEST(ident_i3_every_basis_keeps_its_label_next_to_the_date) {
  FileIdent ident;
  ident.id = "n";
  const struct {
    ClockBasis basis;
    ClockSrc src;
    const char *clock;
  } cases[] = {{ClockBasis::UTC, ClockSrc::SYNC, "utc/sync"},
               {ClockBasis::UTC, ClockSrc::WARM, "utc/warm"},
               {ClockBasis::DRIFT, ClockSrc::SLEEP, "drift/sleep"},
               {ClockBasis::EST, ClockSrc::CARD, "est/card"},
               {ClockBasis::EST, ClockSrc::BUILD, "est/build"}};
  for (const auto &c : cases) {
    CHECK_EQ(ident_line(ident, at(OPEN_S, c.basis, c.src)),
             std::string("#ident,id=n,mac=-,date=2026-10-11T12:00:03Z,clock=") + c.clock +
                 ",build=-,elf=-,cfg=-\n");
  }
}

TEST(ident_i3_dates_across_the_calendar_edges) {
  CHECK_EQ(iso(0), std::string("1970-01-01T00:00:00Z"));
  CHECK_EQ(iso(951827696ull), std::string("2000-02-29T12:34:56Z"));
  CHECK_EQ(iso(1709251199ull), std::string("2024-02-29T23:59:59Z"));
  CHECK_EQ(iso(1709251200ull), std::string("2024-03-01T00:00:00Z"));
  CHECK_EQ(iso(1791676800ull), std::string("2026-10-11T00:00:00Z"));
  CHECK_EQ(iso(1798761599ull), std::string("2026-12-31T23:59:59Z"));
  CHECK_EQ(iso(1798761600ull), std::string("2027-01-01T00:00:00Z"));
  CHECK_EQ(iso(4107542400ull), std::string("2100-03-01T00:00:00Z"));  // 2100 is not a leap year
  // Every day of four years against a running civil count: the algorithm, not a table of answers.
  int year = 2023, month = 1, day = 1;
  const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  for (uint64_t s = 1672531200ull; s < 1672531200ull + 4 * 366 * 86400ull; s += 86400) {
    char want[32];
    std::snprintf(want, sizeof(want), "%04d-%02d-%02dT00:00:00Z", year, month, day);
    CHECK_EQ(iso(s), std::string(want));
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int in_month = mdays[month - 1] + (month == 2 && leap ? 1 : 0);
    if (++day > in_month) {
      day = 1;
      if (++month > 12) {
        month = 1;
        year++;
      }
    }
  }
}

TEST(ident_i2_untrusted_strings_are_escaped_and_cannot_add_a_field) {
  FileIdent ident;
  ident.id = "a,b\nc";
  ident.project = "x,y";
  ident.project_version = "1,0";
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=a_b\\nc,mac=-,date=-,clock=none,build=-,elf=-,cfg=-,project=x_y@1_0\n"));
}

TEST(ident_i2_project_without_version_and_a_malformed_elf) {
  FileIdent ident;
  ident.id = "n";
  ident.project = "acme.logger";
  std::strcpy(ident.elf, "12G4");  // stops at the first non-hex byte
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=-,date=-,clock=none,build=-,elf=12,cfg=-,project=acme.logger\n"));
}

TEST(ident_i2_leading_zeros_are_kept) {
  FileIdent ident;
  ident.id = "n";
  ident.has_mac = true;  // all-zero MAC
  ident.has_cfg = true;
  ident.cfg = 0x1;
  ident.has_card = true;
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=00:00:00:00:00:00,date=-,clock=none,build=-,elf=-,cfg=00000001,"
                       "card=00-0000-00000000\n"));
}

TEST(ident_i2_an_overlong_line_is_marked_and_terminated) {
  std::string id(600, 'n');
  FileIdent ident;
  ident.id = id.c_str();
  const std::string line = ident_line(ident, ClockPoint{});
  CHECK_EQ(line.size(), SD_LOG_MAX_LINE);
  CHECK_EQ(line.substr(line.size() - 2), std::string("~\n"));
  CHECK_EQ(line.find('\n'), line.size() - 1);

  // A short room: still one terminated line, never a bare cut.
  const std::string tight = ident_line(full_ident(), at(OPEN_S, ClockBasis::UTC, ClockSrc::SYNC), 40);
  CHECK_EQ(tight.size(), static_cast<size_t>(40));
  CHECK_EQ(tight.substr(tight.size() - 2), std::string("~\n"));
}

// Field test 2026-10-11 (ESP32-C6): `esp_efuse_mac_get_default()` handed over the EUI-64
// 20:6e:f1:ff:fe:0a:d0:f0 and the line spelled its first six bytes, 20:6e:f1:ff:fe:0a, while the
// device's MAC is 20:6e:f1:0a:d0:f0. ident_set_mac takes exactly six bytes (an 8-byte array does not
// compile against it); the device side passes get_mac_address_raw(), pinned by tests/sd_logger/test_ident.py.
TEST(ident_i2_mac_is_the_six_byte_mac_48) {
  FileIdent ident;
  ident.id = "n";
  const uint8_t mac[6] = {0x20, 0x6E, 0xF1, 0x0A, 0xD0, 0xF0};
  ident_set_mac(ident, mac);
  CHECK(ident.has_mac);
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=20:6e:f1:0a:d0:f0,date=-,clock=none,build=-,elf=-,cfg=-\n"));

  // All zeros is ESPHome's "could not read one": unknown, not a MAC.
  const uint8_t none[6] = {};
  ident_set_mac(ident, none);
  CHECK(!ident.has_mac);
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=-,date=-,clock=none,build=-,elf=-,cfg=-\n"));
}

// Same field test: `elf=f81b322c5`, nine digits, because esp_app_get_elf_sha256() copies from a RAM
// string cut to CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default 9). The line takes 16 from the binary digest.
TEST(ident_i2_elf_is_sixteen_digits_of_the_binary_digest) {
  uint8_t digest[32];
  for (size_t i = 0; i < sizeof(digest); i++)
    digest[i] = static_cast<uint8_t>(0xF8 - i * 0x11);
  FileIdent ident;
  ident.id = "n";
  ident_set_elf(ident, digest, sizeof(digest));
  CHECK_EQ(std::string(ident.elf), std::string("f8e7d6c5b4a39281"));
  CHECK_EQ(std::strlen(ident.elf), SD_LOG_IDENT_ELF_CHARS);
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=-,date=-,clock=none,build=-,elf=f8e7d6c5b4a39281,cfg=-\n"));

  // Leading zero nibbles are kept; a short digest gives what it has; none gives `-`.
  const uint8_t zeros[2] = {0x00, 0x0A};
  ident_set_elf(ident, zeros, sizeof(zeros));
  CHECK_EQ(std::string(ident.elf), std::string("000a"));
  ident_set_elf(ident, nullptr, 32);
  CHECK_EQ(std::string(ident.elf), std::string(""));
  CHECK_EQ(ident_line(ident, ClockPoint{}),
           std::string("#ident,id=n,mac=-,date=-,clock=none,build=-,elf=-,cfg=-\n"));
}
