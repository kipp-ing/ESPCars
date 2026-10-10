#include "harness.h"
#include "sd_diagnostics.h"

#include <cstdint>
#include <cstring>

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
  CHECK_EQ(ring.write_index(), 0u);
  CHECK_EQ(ring.count(ring.write_index()), 0u);
  CHECK_EQ(ring.dropped(), 0u);
}
