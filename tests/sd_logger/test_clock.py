"""The wall-clock basis (docs/sd_logger-clock-spec.md): the schema half and the one source rule.

C9  `time_id:` takes one clock or a list of them.
C6  no estimate ever reaches the system clock: the component never calls settimeofday().
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
