#include "harness.h"
#include "sd_diagnostics.h"

#include <cstdint>
#include <cstring>
#include <thread>

using esphome::sd_logger::RawSectorRequest;
using esphome::sd_logger::ChainPageRequest;
using esphome::sd_logger::SdSpiTraceEntry;
using esphome::sd_logger::SdSpiTraceRing;
using esphome::sd_logger::SD_RAW_READ_MAX_SECTORS;
using esphome::sd_logger::SD_SPI_TRACE_ENTRIES;
using esphome::sd_logger::SD_CHAIN_PAGE_MAX_LINKS;
using esphome::sd_logger::parse_chain_page_query;
using esphome::sd_logger::parse_raw_sector_query;

TEST(sd_diagnostics_raw_query_bounds) {
  RawSectorRequest request{};
  CHECK(parse_raw_sector_query("lba=15064", &request));
  CHECK_EQ(request.lba, 15064u);
  CHECK_EQ(request.count, 1u);
  CHECK(parse_raw_sector_query("count=128&lba=4294967295&cache=ignored", &request));
  CHECK_EQ(request.lba, 0xFFFFFFFFu);
  CHECK_EQ(request.count, SD_RAW_READ_MAX_SECTORS);

  CHECK(!parse_raw_sector_query("count=1", &request));
  CHECK(!parse_raw_sector_query("lba=-1", &request));
  CHECK(!parse_raw_sector_query("lba=4294967296", &request));
  CHECK(!parse_raw_sector_query("lba=1&count=0", &request));
  CHECK(!parse_raw_sector_query("lba=1&count=129", &request));
  CHECK(!parse_raw_sector_query("lba=1&lba=2", &request));
  CHECK(!parse_raw_sector_query("lba=1&", &request));
}

TEST(sd_diagnostics_chain_query_bounds) {
  ChainPageRequest page{};
  CHECK(parse_chain_page_query("name=L0000001.LOG", &page));
  CHECK(std::strcmp(page.name, "L0000001.LOG") == 0);
  CHECK_EQ(page.from, 0u);
  CHECK_EQ(page.count, SD_CHAIN_PAGE_MAX_LINKS);
  CHECK(parse_chain_page_query("count=20&from=4294967295&name=L0000001.UPL", &page));
  CHECK_EQ(page.from, 0xFFFFFFFFu);
  CHECK_EQ(page.count, SD_CHAIN_PAGE_MAX_LINKS);

  CHECK(!parse_chain_page_query("from=0&count=1", &page));
  CHECK(!parse_chain_page_query("name=L0000001.LOG&count=0", &page));
  CHECK(!parse_chain_page_query("name=L0000001.LOG&count=21", &page));
  CHECK(!parse_chain_page_query("name=L0000001.LOG&from=4294967296", &page));
  CHECK(!parse_chain_page_query("name=L0000001.LOG&from=0&from=1", &page));
}

TEST(sd_diagnostics_trace_ring_wrap_and_drops) {
  SdSpiTraceRing ring;
  SdSpiTraceEntry entry{};
  for (uint32_t i = 0; i < static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES) + 3; i++) {
    entry.opcode = static_cast<int32_t>(i);
    entry.arg = i * 10;
    std::memcpy(entry.task, "httpd", 6);
    ring.append(entry);
  }

  const uint32_t end = ring.write_index();
  CHECK_EQ(end, static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES) + 3u);
  CHECK_EQ(ring.count(end), static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES));
  CHECK_EQ(ring.oldest(end), 3u);
  CHECK_EQ(ring.dropped(), 3u);
  const SdSpiTraceEntry oldest = ring.read(ring.oldest(end));
  const SdSpiTraceEntry newest = ring.read(end - 1);
  CHECK_EQ(oldest.seq, 3u);
  CHECK_EQ(oldest.opcode, 3);
  CHECK_EQ(newest.seq, end - 1);
  CHECK_EQ(newest.arg, (end - 1) * 10u);
  CHECK(std::strcmp(newest.task, "httpd") == 0);

  ring.clear();
  CHECK_EQ(ring.write_index(), end);
  CHECK_EQ(ring.count(ring.write_index()), 0u);
  CHECK_EQ(ring.dropped(), 0u);
}

TEST(sd_diagnostics_clear_between_writer_samples) {
  SdSpiTraceRing ring;
  for (unsigned i = 0; i < 20; i++)
    ring.append(SdSpiTraceEntry{});
  const uint32_t begin = ring.write_index();
  // The HTTP task clears the display while the writer is inside write().
  std::thread clearer([&]() { ring.clear(); });
  clearer.join();
  ring.append(SdSpiTraceEntry{});
  const uint32_t end = ring.write_index();
  CHECK_EQ(end - begin, 1u);
  // Avoid the old implementation's multi-billion-iteration loop on failure.
  if (end - begin != 1u)
    return;
  const auto summary = ring.summarize(begin, end);
  CHECK_EQ(summary.commands, 1u);
  CHECK_EQ(summary.available, 1u);
  CHECK_EQ(summary.missing, 0u);
  CHECK_EQ(ring.count(end), 1u);
  CHECK_EQ(ring.oldest(end), begin);
  CHECK_EQ(ring.dropped(), 0u);
}

TEST(sd_diagnostics_summary_work_is_bounded) {
  SdSpiTraceRing ring;
  // A reversed counter span must cost at most one ring, not 2^32 reads.
  const auto summary = ring.summarize(2, 1);
  CHECK_EQ(summary.commands, UINT32_MAX);
  CHECK_EQ(summary.available, 0u);
  CHECK_EQ(summary.missing, UINT32_MAX);

  ring.append(SdSpiTraceEntry{});
  const auto wrapped = ring.summarize(UINT32_MAX - 1, 1);
  CHECK_EQ(wrapped.commands, 3u);
  CHECK_EQ(wrapped.available, 1u);
  CHECK_EQ(wrapped.missing, 2u);
}

TEST(sd_diagnostics_summary_counts_overwritten_prefix) {
  SdSpiTraceRing ring;
  for (size_t i = 0; i < SD_SPI_TRACE_ENTRIES + 20; i++) {
    SdSpiTraceEntry entry{};
    entry.err = i == 0 ? -999 : -7;
    ring.append(entry);
  }
  const auto summary = ring.summarize(0, ring.write_index());
  CHECK_EQ(summary.commands, SD_SPI_TRACE_ENTRIES + 20);
  CHECK_EQ(summary.available, SD_SPI_TRACE_ENTRIES);
  CHECK_EQ(summary.missing, 20u);
  CHECK_EQ(summary.worst_err, -7);
}

TEST(sd_diagnostics_concurrent_wrap_reads_are_coherent) {
  SdSpiTraceRing ring;
  std::atomic<bool> ready{false};
  std::atomic<bool> done{false};
  std::thread writer([&]() {
    while (!ready.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (uint32_t i = 0; i < 1000000; i++) {
      SdSpiTraceEntry entry{};
      entry.us = static_cast<int64_t>((static_cast<uint64_t>(i) << 32) | i);
      entry.arg = entry.blklen = entry.datalen = entry.flags = entry.response_raw = entry.response = i;
      entry.err = entry.opcode = static_cast<int32_t>(i);
      std::memcpy(entry.task, &i, sizeof(i));
      std::memcpy(entry.task + sizeof(i), &i, sizeof(i));
      ring.append(entry);
    }
    done.store(true, std::memory_order_release);
  });
  ready.store(true, std::memory_order_release);
  uint32_t checked = 0;
  uint32_t torn = 0;
  do {
    const uint32_t end = ring.write_index();
    if (end <= SD_SPI_TRACE_ENTRIES)
      continue;
    // The next append overwrites this slot; racing it is the regression.
    const uint32_t seq = end - static_cast<uint32_t>(SD_SPI_TRACE_ENTRIES);
    const auto entry = ring.read(seq);
    if (entry.seq != seq)
      continue;
    checked++;
    uint32_t task_low, task_high;
    std::memcpy(&task_low, entry.task, sizeof(task_low));
    std::memcpy(&task_high, entry.task + sizeof(task_low), sizeof(task_high));
    if (static_cast<uint64_t>(entry.us) != ((static_cast<uint64_t>(seq) << 32) | seq) || entry.arg != seq ||
        entry.blklen != seq || entry.datalen != seq || entry.flags != seq || entry.response_raw != seq ||
        entry.response != seq || entry.err != static_cast<int32_t>(seq) || entry.opcode != static_cast<int32_t>(seq) ||
        task_low != seq || task_high != seq)
      torn++;
  } while (!done.load(std::memory_order_acquire));
  writer.join();
  CHECK(checked > 0);
  CHECK_EQ(torn, 0u);
}
