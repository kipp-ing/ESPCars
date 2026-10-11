"""The wall-clock basis (docs/sd_logger-clock-spec.md): the schema half and the one source rule.

C9  `time_id:` takes one clock or a list of them.
C6  no estimate ever reaches the system clock: the component never calls settimeofday().
C12 a time-sync callback that finds the clock untouched since setup() is not a sync.
"""

from __future__ import annotations

from pathlib import Path
import re

from esphome.core import ID

from .common import sd_logger, setup_c6, validate

COMPONENT = Path(__file__).resolve().parents[2] / "components" / "sd_logger"


def _ids(validated) -> list[str]:
    return [item.id if isinstance(item, ID) else str(item) for item in validated["time_id"]]


def test_c9_a_single_time_id_is_still_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(time_id="sntp_time"))
    assert _ids(validated) == ["sntp_time"]


def test_c9_several_clocks_are_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(time_id=["rtc_time", "sntp_time"]))
    assert _ids(validated) == ["rtc_time", "sntp_time"]


def test_c9_time_id_stays_optional(set_core_config) -> None:
    setup_c6(set_core_config)
    assert validate(sd_logger()).get("time_id", []) == []


def test_c6_the_component_never_sets_the_system_clock() -> None:
    # A T3 card floor written into the system clock would come back after the next warm reset as a
    # T2 "reading" — an estimate laundered into a measurement. Only a time source may set the clock.
    offenders = []
    for path in sorted(COMPONENT.rglob("*")):
        if path.suffix not in {".h", ".cpp", ".py"}:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            code = line.split("//", 1)[0]
            if re.search(r"\b(settimeofday|clock_settime|sntp_sync_time)\s*\(", code):
                offenders.append(f"{path.name}:{number}: {line.strip()}")
    assert not offenders, "\n".join(offenders)


def _body(name: str) -> str:
    """The text of `SdLogger::<name>(...) { ... }`, by brace counting from its definition."""
    text = (COMPONENT / "sd_logger.cpp").read_text(encoding="utf-8")
    match = re.search(rf"^\S[^\n]*SdLogger::{name}\([^)]*\)\s*\{{", text, re.MULTILINE)
    assert match, f"SdLogger::{name} not found"
    depth, i = 1, match.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[match.end() : i - 1]


def test_c12_an_unstepped_callback_returns_before_it_can_upgrade_the_basis() -> None:
    # Field test 2026-10-11: ESPHome's sntp first loop() fires on_time_sync after every warm reset
    # and wake, and the logger wrote `utc/sync` for a clock nobody had set. The predicate is
    # host-tested (c12_* in tests/host/test_clock_basis.cpp); this pins that it guards the anchor.
    body = _body("on_time_sync")
    guard = body.index("clock_unstepped_since(this->setup_clock_, utc_us, boot_us)")
    assert body.index("return;", guard) < body.index("this->set_utc_anchor(")
    # The sample is taken in setup(), before anything a loop() can fire.
    setup = _body("setup")
    assert setup.index("this->setup_clock_.utc_us = gettimeofday_us();") < setup.index("this->capture_ident_();")
