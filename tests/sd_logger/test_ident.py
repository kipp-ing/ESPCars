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
