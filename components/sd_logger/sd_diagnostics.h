#pragma once

// The two SD read-path instruments share these small, dependency-free pieces so
// their boundary and ring behaviour can run in tests/host without ESP-IDF.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome {
namespace sd_logger {

static constexpr uint32_t SD_RAW_READ_MAX_SECTORS = 128;
static constexpr uint32_t SD_CHAIN_PAGE_MAX_LINKS = 20;
static constexpr size_t SD_SPI_TRACE_ENTRIES = 512;
// SDMMC/SDSPI raw-sector commands always address these physical 512-byte blocks. FAT may expose
// larger logical sectors, but its BPB itself still resides in the first physical block.
static constexpr size_t SD_LOGGER_CARD_SECTOR_BYTES = 512;

struct RawSectorRequest {
  uint32_t lba{0};
  uint32_t count{1};
};

struct ChainPageRequest {
  char name[13]{};
  uint32_t from{0};
  uint32_t count{SD_CHAIN_PAGE_MAX_LINKS};
};

/// Parse exactly the numeric query parameters used by `/sdlog/raw`. Values are
/// decimal uint32s; an absent count is one sector. Unknown parameters are
/// ignored so a browser cache-buster cannot alter the requested raw read.
inline bool parse_raw_sector_query(const char *query, RawSectorRequest *out) {
  if (query == nullptr || out == nullptr)
    return false;
  bool got_lba = false;
  bool got_count = false;
  RawSectorRequest request{};
  const char *p = query;
  while (*p != '\0') {
    const char *key = p;
    while (*p != '\0' && *p != '=' && *p != '&')
      p++;
    const size_t key_len = static_cast<size_t>(p - key);
    if (*p != '=')
      return false;
    p++;
    const bool is_lba = key_len == 3 && std::memcmp(key, "lba", 3) == 0;
    const bool is_count = key_len == 5 && std::memcmp(key, "count", 5) == 0;
    uint64_t value = 0;
    if (is_lba || is_count) {
      if (*p < '0' || *p > '9')
        return false;
      while (*p >= '0' && *p <= '9') {
        value = value * 10 + static_cast<uint64_t>(*p - '0');
        if (value > 0xFFFFFFFFull)
          return false;
        p++;
      }
    } else {
      while (*p != '\0' && *p != '&')
        p++;
    }
    if (is_lba) {
      if (got_lba)
        return false;
      request.lba = static_cast<uint32_t>(value);
      got_lba = true;
    } else if (is_count) {
      if (got_count)
        return false;
      request.count = static_cast<uint32_t>(value);
      got_count = true;
    }
    if (*p == '\0')
      break;
    if (*p != '&')
      return false;
    p++;
    if (*p == '\0')
      return false;
  }
  if (!got_lba || request.count == 0 || request.count > SD_RAW_READ_MAX_SECTORS)
    return false;
  *out = request;
  return true;
}

/// Parse the bounded page controls for `/sdlog/chain`. `name` remains subject
/// to the component's normal 8.3 chunk-name validation after this transport
/// parser has copied it. An omitted `from` starts at the first link; an omitted
/// `count` returns one fixed-size page.
inline bool parse_chain_page_query(const char *query, ChainPageRequest *out) {
  if (query == nullptr || out == nullptr)
    return false;
  bool got_name = false;
  bool got_from = false;
  bool got_count = false;
  ChainPageRequest request{};
  const char *p = query;
  while (*p != '\0') {
    const char *key = p;
    while (*p != '\0' && *p != '=' && *p != '&')
      p++;
    const size_t key_len = static_cast<size_t>(p - key);
    if (*p != '=')
      return false;
    p++;
    const char *value_start = p;
    while (*p != '\0' && *p != '&')
      p++;
    const size_t value_len = static_cast<size_t>(p - value_start);
    const bool is_name = key_len == 4 && std::memcmp(key, "name", 4) == 0;
    const bool is_from = key_len == 4 && std::memcmp(key, "from", 4) == 0;
    const bool is_count = key_len == 5 && std::memcmp(key, "count", 5) == 0;
    if (is_name) {
      if (got_name || value_len != sizeof(request.name) - 1)
        return false;
      std::memcpy(request.name, value_start, value_len);
      got_name = true;
    } else if (is_from || is_count) {
      if (value_len == 0)
        return false;
      uint64_t value = 0;
      for (size_t i = 0; i < value_len; i++) {
        if (value_start[i] < '0' || value_start[i] > '9')
          return false;
        value = value * 10 + static_cast<uint64_t>(value_start[i] - '0');
        if (value > 0xFFFFFFFFull)
          return false;
      }
      if (is_from) {
        if (got_from)
          return false;
        request.from = static_cast<uint32_t>(value);
        got_from = true;
      } else {
        if (got_count)
          return false;
        request.count = static_cast<uint32_t>(value);
        got_count = true;
      }
    }
    if (*p == '\0')
      break;
    p++;
    if (*p == '\0')
      return false;
  }
  if (!got_name || request.count == 0 || request.count > SD_CHAIN_PAGE_MAX_LINKS)
    return false;
  *out = request;
  return true;
}

struct SdSpiTraceEntry {
  uint32_t seq{0};
  int64_t us{0};
  uint32_t arg{0};
  uint32_t blklen{0};
  uint32_t datalen{0};
  uint32_t flags{0};
  int32_t err{0};
  int32_t opcode{0};
  char task[9]{};
};

/// A lossy append-only ring. Readers snapshot `write_index()` and may observe
/// an overwritten or in-progress tail entry; the sequence in every entry lets
/// the CSV consumer detect either case without making SDSPI wait on a reader.
class SdSpiTraceRing {
 public:
  void append(const SdSpiTraceEntry &entry) {
    const uint32_t seq = this->write_index_.fetch_add(1, std::memory_order_relaxed);
    TraceSlot &slot = this->entries_[seq % SD_SPI_TRACE_ENTRIES];
    slot.us_low.store(static_cast<uint32_t>(entry.us), std::memory_order_relaxed);
    slot.us_high.store(static_cast<uint32_t>(static_cast<uint64_t>(entry.us) >> 32), std::memory_order_relaxed);
    slot.arg.store(entry.arg, std::memory_order_relaxed);
    slot.blklen.store(entry.blklen, std::memory_order_relaxed);
    slot.datalen.store(entry.datalen, std::memory_order_relaxed);
    slot.flags.store(entry.flags, std::memory_order_relaxed);
    slot.err.store(entry.err, std::memory_order_relaxed);
    slot.opcode.store(entry.opcode, std::memory_order_relaxed);
    uint32_t task_low = 0;
    uint32_t task_high = 0;
    std::memcpy(&task_low, entry.task, sizeof(task_low));
    std::memcpy(&task_high, entry.task + sizeof(task_low), sizeof(task_high));
    slot.task_low.store(task_low, std::memory_order_relaxed);
    slot.task_high.store(task_high, std::memory_order_relaxed);
    // Last so a reader can compare this sequence with the CSV row it receives. The individual
    // fields are atomic too: a tail may be mixed during a wrap, but it is never a C++ data race.
    slot.seq.store(seq, std::memory_order_relaxed);
    if (seq >= SD_SPI_TRACE_ENTRIES)
      this->dropped_.store(seq - static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES) + 1, std::memory_order_relaxed);
  }

  uint32_t write_index() const { return this->write_index_.load(std::memory_order_relaxed); }
  uint32_t dropped() const { return this->dropped_.load(std::memory_order_relaxed); }
  uint32_t count(uint32_t write_index) const {
    return write_index < SD_SPI_TRACE_ENTRIES ? write_index : static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES);
  }
  uint32_t oldest(uint32_t write_index) const { return write_index - this->count(write_index); }
  SdSpiTraceEntry read(uint32_t seq) const {
    const TraceSlot &slot = this->entries_[seq % SD_SPI_TRACE_ENTRIES];
    SdSpiTraceEntry entry{};
    entry.seq = slot.seq.load(std::memory_order_relaxed);
    const uint64_t us = static_cast<uint64_t>(slot.us_low.load(std::memory_order_relaxed)) |
                        (static_cast<uint64_t>(slot.us_high.load(std::memory_order_relaxed)) << 32);
    entry.us = static_cast<int64_t>(us);
    entry.arg = slot.arg.load(std::memory_order_relaxed);
    entry.blklen = slot.blklen.load(std::memory_order_relaxed);
    entry.datalen = slot.datalen.load(std::memory_order_relaxed);
    entry.flags = slot.flags.load(std::memory_order_relaxed);
    entry.err = slot.err.load(std::memory_order_relaxed);
    entry.opcode = slot.opcode.load(std::memory_order_relaxed);
    const uint32_t task_low = slot.task_low.load(std::memory_order_relaxed);
    const uint32_t task_high = slot.task_high.load(std::memory_order_relaxed);
    std::memcpy(entry.task, &task_low, sizeof(task_low));
    std::memcpy(entry.task + sizeof(task_low), &task_high, sizeof(task_high));
    entry.task[sizeof(entry.task) - 1] = '\0';
    return entry;
  }
  void clear() {
    this->write_index_.store(0, std::memory_order_relaxed);
    this->dropped_.store(0, std::memory_order_relaxed);
  }

 private:
  struct TraceSlot {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> us_low{0};
    std::atomic<uint32_t> us_high{0};
    std::atomic<uint32_t> arg{0};
    std::atomic<uint32_t> blklen{0};
    std::atomic<uint32_t> datalen{0};
    std::atomic<uint32_t> flags{0};
    std::atomic<int32_t> err{0};
    std::atomic<int32_t> opcode{0};
    std::atomic<uint32_t> task_low{0};
    std::atomic<uint32_t> task_high{0};
  };

  TraceSlot entries_[SD_SPI_TRACE_ENTRIES]{};
  std::atomic<uint32_t> write_index_{0};
  std::atomic<uint32_t> dropped_{0};
};

}  // namespace sd_logger
}  // namespace esphome
