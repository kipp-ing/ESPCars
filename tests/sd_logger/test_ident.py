"""`#ident` (docs/sd_logger-spec.md §6 F1j): the source half of I1 and I4.

The line itself is host-tested (tests/host/test_file_ident.cpp). What a host case cannot see is
whether the writer emits it into every header, so these pin the call sites in the component source.
"""

from __future__ import annotations

from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[2] / "components" / "sd_logger" / "sd_logger.cpp"


def _body(name: str) -> str:
    """The text of `SdLogger::<name>(...) { ... }`, by brace counting from its definition."""
    text = SOURCE.read_text(encoding="utf-8")
    match = re.search(rf"^\S[^\n]*SdLogger::{name}\([^)]*\)\s*\{{", text, re.MULTILINE)
    assert match, f"SdLogger::{name} not found"
    depth, i = 1, match.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[match.end() : i - 1]


def test_i1_every_header_carries_ident_directly_after_sdlog() -> None:
    body = _body("write_file_header_")
    commits = [m.group(1) for m in re.finditer(r"commit_line_\(\s*(format_\w+)", body)]
    assert commits[:2] == ["format_header", "format_ident"], commits
    # Unconditional: the only guard on the line is room in the block, as for `#sdlog` itself.
    ident_at = body.index("format_ident")
    guard = body.rfind("\n", 0, body.rfind("this->commit_line_", 0, ident_at))
    guard_line = body[body.rfind("\n", 0, guard) + 1 : guard].strip()
    assert guard_line == "if (this->ensure_room_(SD_LOG_MAX_LINE))", guard_line


def test_i4_identity_is_captured_once_and_the_card_per_mount() -> None:
    assert "this->capture_ident_();" in _body("setup")
    assert "this->capture_card_ident_();" in _body("mount_card_")
    # Nothing per file but the line: no lookups on the header path.
    header = _body("write_file_header_")
    for call in ("esp_efuse_mac_get_default", "esp_app_get_elf_sha256", "get_config_hash", "snprintf"):
        assert call not in header, call


def test_i2_mac_and_elf_come_from_sources_that_are_right_on_every_variant() -> None:
    """Field test 2026-10-11 on an ESP32-C6: `esp_efuse_mac_get_default()` is the 8-byte EUI-64 on
    chips with IEEE 802.15.4 (C6, H2), so the line named `ff:fe` as part of the MAC; and
    `esp_app_get_elf_sha256()` is cut to CONFIG_APP_RETRIEVE_LEN_ELF_SHA (default 9) digits."""
    capture = _body("capture_ident_")
    assert "get_mac_address_raw(mac);" in capture
    assert "ident_set_mac(this->ident_, mac);" in capture
    assert "esp_app_get_description()" in capture and "app_elf_sha256" in capture
    code = re.sub(r"//[^\n]*", "", SOURCE.read_text(encoding="utf-8"))
    for call in ("esp_efuse_mac_get_default", "esp_app_get_elf_sha256", "esp_read_mac"):
        assert not re.search(rf"\b{call}\s*\(", code), call
