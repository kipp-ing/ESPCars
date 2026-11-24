"""Extract repository-owned SD logger glue verbatim for the isolated host binary."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "components/sd_logger/sd_logger.cpp").read_text()
HEADER = (ROOT / "components/sd_logger/sd_logger.h").read_text()
BUILD = ROOT / "tests/host/build"


def function(marker):
    start = SOURCE.index(marker)
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end] + "\n"


parts = [
    function(marker)
    for marker in (
        "uint16_t read_le16(",
        "uint32_t read_le32(",
        "bool fat_bpb_cluster_bytes(",
        "bool mounted_volume_bounds(",
        "void log_fat_geometry(",
        "bool raw_read_sector(",
        "bool parse_raw_fat32_bpb(",
        "bool load_raw_fat32_geometry(",
        "void SdLogger::unmount_card_()",
        "void SdLogger::publish_debug_writer_frozen_(",
        "void SdLogger::debug_freeze_writer_()",
        "void SdLogger::on_shutdown()",
        "void SdLogger::notify_writer_()",
        "void SdLogger::finish_writer_()",
        "void SdLogger::drain_confirms_()",
        "SdLogger::ConfirmResult SdLogger::collection_confirm(",
        "bool SdLogger::push_record(",
        "bool SdLogger::pop_record_(",
        "bool SdLogger::pop_text_(",
        "void SdLogger::capture_write_failure_(",
        "void SdLogger::enter_failed_(",
        "bool SdLogger::collection_raw_read_sector(",
        "void SdLogger::tap_drain_loop_()",
    )
]
# These are real setup/restart branches, with every return preserved in a void wrapper.
for label, start in (
    ("setup_writer", SOURCE.index("void SdLogger::setup()")),
    ("restart_writer", SOURCE.index("void SdLogger::confine_format(")),
):
    begin = SOURCE.index(
        "  if (xTaskCreatePinnedToCore(&SdLogger::writer_trampoline", start
    )
    begin = SOURCE.rfind("  this->publish_runtime_status_();", start, begin)
    closing = "  portEXIT_CRITICAL(&this->writer_mux_);"
    end = SOURCE.index(closing, begin) + len(closing)
    parts.append(f"void SdLogger::{label}() {{\n{SOURCE[begin:end]}\n}}\n")
begin = (
    SOURCE.rfind(
        "\n", 0, SOURCE.index("xTaskCreatePinnedToCore(&SdLogger::monitor_trampoline")
    )
    + 1
)
end = SOURCE.index("\n      }", begin)
parts.append(f"void SdLogger::start_monitor() {{\n{SOURCE[begin:end]}\n}}\n")
# Execute header accounting and loop delivery without unrelated formatters/server/stats.
begin = SOURCE.index(
    "  // Never spend loss baselines", SOURCE.index("void SdLogger::write_file_header_")
)
end = SOURCE.index("\n}", begin)
parts.append(f"void SdLogger::header_baselines() {{\n{SOURCE[begin:end]}\n}}\n")
begin = SOURCE.index("void SdLogger::loop() {") + len("void SdLogger::loop() {")
end = SOURCE.index("  if (this->stats_log_interval_ms_", begin)
parts.append(f"void SdLogger::loop() {{\n{SOURCE[begin:end]}\n}}\n")
for marker in ("bool SdLogger::teardown()", "void SdLogger::publish_runtime_status_()"):
    if marker in SOURCE:
        parts.append(function(marker))
    elif "teardown" in marker:
        parts.append("bool SdLogger::teardown() { return true; }\n")
begin = HEADER.index("  void request_emergency_close(")
end = HEADER.index("\n  }", begin) + 4
request = (
    HEADER[begin:end]
    .replace(' = "emergency"', "")
    .replace("void request_emergency_close", "void SdLogger::request_emergency_close")
)
parts.append(request + "\n")
begin = HEADER.index("  uint8_t ring_fill_percent()")
end = HEADER.index("\n  }", begin) + 4
ring_fill = HEADER[begin:end]
# Production getters and nested facts type, preserving their synchronization.
begin = HEADER.index("  uint32_t get_write_failures()")
end = HEADER.index("  // Retention's lifetime losses", begin)
getters = HEADER[begin:end]
facts = HEADER[HEADER.index("  struct LastWriteFailure {") : begin]
(BUILD / "sd_logger_getters.inc").write_text(facts + "\n" + getters + "\n" + ring_fill)
(BUILD / "sd_logger_runtime.inc").write_text("\n".join(parts))
