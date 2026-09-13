"""sd_logger schema tests (V1-V26, plus defaults, size parsing and vcc_monitor).

V1/V8 pins must be distinct.
V2   clock capped at the SD-over-SPI ceiling (20 MHz) on the C6.
V3   vcc_monitor.adc_pin must be an ADC1 GPIO (0-6).
V5   buffer_depth is a power of two in range.
V6   sync_interval is a positive duration.
V7   can_ports entries reference declared can_gateway ports.
V11  tapped ports and their source tags are unique.
V12  a tapped port has `log_tap: true` on the can_gateway side (final validate).

Format v1 (spec §6):
V13  label matches [A-Za-z0-9_-]{1,8} and is unique across can_ports and sources.
V14  sources[].tag is 0-255 and disjoint from every can_ports tag; kind is known.
V15  esphome_logs: requires a `logger:` block (final validate).
V16  warn when logger.level is more restrictive than esphome_logs.level.
V17  warn when logger.task_log_buffer_size is 0.
V18  esphome_logs.buffer_depth is a power of two in [8, 256].

Card recovery (spec §7 Layer C):
V19  recovery.power_cycle requires card_power_pin, and defaults from its presence.
V20  recovery delays are in [100ms, 300s] and max_delay >= initial_delay.
V21  warn that format_if_mount_failed deliberately does not apply to retries.

Chunk collection (docs/sdlog-collection-design.md, M6):
V22  a `collection:` block requires `wifi:` in the config (final validate).
V23  max_file_seconds is in [1s, 3600s].
V24  collection.retention_percent is strictly between 1 and 99.
V25  collection.port must not collide with web_server's (final validate).
V26  collection.max_chunks is in [16, 2048], defaulting to 256.

V10 is still deferred.
"""

from __future__ import annotations

from typing import Any

import pytest

from esphome import config_validation as cv
from esphome.core import ID

from .common import (
    logger_config,
    recovery,
    sd_logger,
    setup_c6,
    validate,
    vcc_monitor,
)

# --------------------------------------------------------------------- defaults


def test_defaults(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger())
    assert validated["clock"] == 20_000_000
    assert validated["mount_point"] == "/sdcard"
    assert validated["buffer_depth"] == 2048
    assert validated["sync_interval"].total_milliseconds == 2000
    assert validated["max_file_size"] == 16 * 1024 * 1024
    assert validated["format_if_mount_failed"] is False


def test_pins_are_resolved_to_numbers(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger())
    assert validated["clk_pin"] == 6
    assert validated["cs_pin"] == 11


@pytest.mark.parametrize("key", ["clk_pin", "mosi_pin", "miso_pin", "cs_pin"])
def test_spi_pins_required(set_core_config, key: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="required key not provided"):
        validate(sd_logger(**{key: None}))


# --------------------------------------------------------------------- V1 / V8


def test_v1_duplicate_spi_pins_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="distinct"):
        validate(sd_logger(mosi_pin="GPIO6"))  # == clk_pin


def test_v8_card_power_pin_must_differ(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="distinct"):
        validate(sd_logger(card_power_pin="GPIO11"))  # == cs_pin


def test_v8_adc_pin_must_differ_from_spi(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="distinct"):
        # adc_pin 6 collides with clk_pin GPIO6
        validate(sd_logger(vcc_monitor=vcc_monitor(adc_pin=6)))


def test_distinct_pins_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(card_power_pin="GPIO2"))
    assert validated["card_power_pin"] == 2


# --------------------------------------------------------------------------- V2


@pytest.mark.parametrize(
    ("value", "hz"),
    [("20MHz", 20_000_000), ("400kHz", 400_000), ("1MHz", 1_000_000)],
)
def test_v2_clock_accepted(set_core_config, value: str, hz: int) -> None:
    setup_c6(set_core_config)
    assert validate(sd_logger(clock=value))["clock"] == hz


@pytest.mark.parametrize("value", ["25MHz", "40MHz", "100kHz"])
def test_v2_clock_out_of_range_rejected(set_core_config, value: str) -> None:
    """Above 20 MHz exceeds the SD-over-SPI cap; below 400 kHz is not usable."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="20MHz"):
        validate(sd_logger(clock=value))


# --------------------------------------------------------------------------- V3


@pytest.mark.parametrize("pin", [0, 3, 5])
def test_v3_adc_pin_accepted(set_core_config, pin: int) -> None:
    setup_c6(set_core_config)
    # Off the SPI pins (clk=GPIO6) to isolate the ADC-range check.
    validated = validate(sd_logger(vcc_monitor=vcc_monitor(adc_pin=pin)))
    assert validated["vcc_monitor"]["adc_pin"] == pin


@pytest.mark.parametrize("pin", [7, 10, 21])
def test_v3_adc_pin_off_adc1_rejected(set_core_config, pin: int) -> None:
    """The C6 SAR ADC1 only reaches GPIO0-GPIO6."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(vcc_monitor=vcc_monitor(adc_pin=pin)))


def test_vcc_monitor_requires_threshold_and_divider(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="required key not provided"):
        validate(sd_logger(vcc_monitor={"adc_pin": 0}))


def test_vcc_monitor_absent_by_default(set_core_config) -> None:
    setup_c6(set_core_config)
    assert "vcc_monitor" not in validate(sd_logger())


# --------------------------------------------------------------------------- V5


@pytest.mark.parametrize("depth", [256, 2048, 16384])
def test_v5_buffer_depth_accepted(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    assert validate(sd_logger(buffer_depth=depth))["buffer_depth"] == depth


@pytest.mark.parametrize("depth", [1000, 3000])
def test_v5_buffer_depth_not_power_of_two_rejected(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="power of two"):
        validate(sd_logger(buffer_depth=depth))


@pytest.mark.parametrize("depth", [128, 32768])
def test_v5_buffer_depth_out_of_range_rejected(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 256 and 16384"):
        validate(sd_logger(buffer_depth=depth))


# --------------------------------------------------------------------------- V6


def test_v6_sync_interval_overridable(set_core_config) -> None:
    setup_c6(set_core_config)
    assert validate(sd_logger(sync_interval="5s"))["sync_interval"].total_milliseconds == 5000


# ------------------------------------------------------------------- size parse


@pytest.mark.parametrize(
    ("value", "expected"),
    [("16MB", 16 * 1024 * 1024), ("512KB", 512 * 1024), (1_048_576, 1_048_576)],
)
def test_max_file_size_units(set_core_config, value, expected: int) -> None:
    setup_c6(set_core_config)
    assert validate(sd_logger(max_file_size=value))["max_file_size"] == expected


def test_max_file_size_too_small_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(max_file_size="1KB"))


# ----------------------------------------------------------------- misc schema


def test_unknown_key_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(not_a_real_key=1))


def test_statistics_optional_with_default_interval(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(statistics={}))
    assert validated["statistics"]["log_interval"].total_milliseconds == 60000


# ----------------------------------------------------------------- V7/V11/V12
#
# The native can_gateway tap. `can_ports:` names the ports whose RX-ISR tap ring
# the writer task drains; each gets a `source` tag so both segments stay
# distinguishable inside one CSV.


def test_v7_can_ports_bare_ids_get_sequential_source_tags(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(can_ports=["seg1", "seg2"]))
    entries = validated["can_ports"]
    assert [str(entry["port"]) for entry in entries] == ["seg1", "seg2"]
    # Source 0 stays reserved for the sd_logger.log action (S1).
    assert [entry["source"] for entry in entries] == [1, 2]


def test_v7_can_ports_explicit_source(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(can_ports=[{"port": "seg1", "source": 7}, "seg2"])
    )
    assert [entry["source"] for entry in validated["can_ports"]] == [7, 2]


def test_v11_source_zero_rejected(set_core_config) -> None:
    """0 is the action's tag; a port claiming it would make records ambiguous."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(can_ports=[{"port": "seg1", "source": 0}]))


def test_v11_duplicate_port_rejected(set_core_config) -> None:
    """One port has one ring; two entries would split its frames across two tags."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="tapped twice"):
        validate(sd_logger(can_ports=["seg1", "seg1"]))


def test_v11_duplicate_source_tag_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="distinct source"):
        validate(
            sd_logger(
                can_ports=[
                    {"port": "seg1", "source": 3},
                    {"port": "seg2", "source": 3},
                ]
            )
        )


def test_v11_explicit_tag_colliding_with_a_default_rejected(set_core_config) -> None:
    """seg1 defaults to 1; seg2 asking for 1 explicitly must still be caught."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="distinct source"):
        validate(sd_logger(can_ports=["seg1", {"port": "seg2", "source": 1}]))


def test_can_ports_empty_list_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(can_ports=[]))


def _gateway_full_config(
    *log_tap_flags: bool,
    logger: dict | None = None,
    wifi: dict | None = None,
    web_server: dict | None = None,
    ethernet: dict | None = None,
) -> dict:
    """A stand-in resolved config with one can_gateway port per flag.

    `wifi`, `web_server` and `ethernet` are the other components M6's final
    validation has to look at (V22, V25); all are absent unless a test asks.
    """
    full: dict = {
        "can_gateway": {
            "id": "gw",
            "ports": [
                {
                    "id": ID(f"seg{index + 1}", is_declaration=True),
                    "rx_pin": "GPIO3",
                    "tx_pin": "GPIO2",
                    "bit_rate": 500000,
                    "log_tap": flag,
                }
                for index, flag in enumerate(log_tap_flags)
            ],
        }
    }
    if logger is not None:
        full["logger"] = logger
    if wifi is not None:
        full["wifi"] = wifi
    if web_server is not None:
        full["web_server"] = web_server
    if ethernet is not None:
        full["ethernet"] = ethernet
    return full


def _run_final_validate(
    config: dict,
    *log_tap_flags: bool,
    logger: dict | None = None,
    wifi: dict | None = None,
    web_server: dict | None = None,
    ethernet: dict | None = None,
):
    import esphome.final_validate as fv
    from esphome.components.sd_logger import _final_validate

    token = fv.full_config.set(
        _gateway_full_config(
            *log_tap_flags,
            logger=logger,
            wifi=wifi,
            web_server=web_server,
            ethernet=ethernet,
        )
    )
    try:
        return _final_validate(config)
    finally:
        fv.full_config.reset(token)


def test_v12_tapped_port_needs_log_tap(set_core_config) -> None:
    """Without `log_tap: true` the port allocates no ring, and the failure looks
    exactly like a silent bus — worth a config error, not a warning."""
    setup_c6(set_core_config)
    config = validate(sd_logger(can_ports=["seg1", "seg2"]))
    with pytest.raises(cv.Invalid, match="log_tap"):
        _run_final_validate(config, True, False)


def test_v12_accepts_ports_with_log_tap(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(can_ports=["seg1", "seg2"]))
    assert _run_final_validate(config, True, True) is config


def test_v12_noop_without_can_ports(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger())
    assert _run_final_validate(config) is config


# ------------------------------------------------------- VFS directory support


@pytest.mark.parametrize("can_ports", [None, ["seg1", "seg2"]])
def test_final_validate_requires_vfs_dir(set_core_config, can_ports) -> None:
    """The component must declare its need for CONFIG_VFS_SUPPORT_DIR.

    ESPHome disables that option by default, and with it off `opendir()` is
    compiled out and returns nullptr *without setting errno*. `scan_next_seq_()`
    then silently restarts at sequence 0 on every boot, and because
    `open_next_file_()` seeds `file_bytes_` from a real `ftell()`, every
    already-full file trips the rotate check at once: the writer walks the whole
    existing file set one `fopen` at a time and drops everything produced
    meanwhile. Measured on the bench before the fix: 201 693 records lost across
    a 397-file walk, growing every boot. After: 1 rotation, `dropped == 0`.

    Two things this pins down, both of which were the actual bug:
      * it happens in a validator, not `to_code()` — the esp32 component reads
        the flag inside its own `to_code()` and the two are not priority-ordered,
        so declaring it from ours lands too late to have any effect;
      * it happens *before* the `can_ports` early return, so a plain S1 logger
        with no CAN tap still gets directory support.
    """
    from esphome.components.esp32 import KEY_VFS_DIR_REQUIRED
    from esphome.core import CORE

    setup_c6(set_core_config)
    CORE.data.pop(KEY_VFS_DIR_REQUIRED, None)
    config = validate(sd_logger(can_ports=can_ports))
    _run_final_validate(config, True, True)
    assert CORE.data.get(KEY_VFS_DIR_REQUIRED) is True


# ------------------------------------------------------------------------- V13
#
# Labels. This is the column that names a bus on every single line, and the file
# is the only artifact that survives the run — so an ambiguous one is not a
# cosmetic problem.


def test_v13_can_port_label_defaults_to_the_interface_number(set_core_config) -> None:
    """`C1`, `C2`, ... 1-based, in declaration order — not the port's id.

    The label is on every record, so it is the most expensive field on the card:
    `seg1` -> `C1` is 2 B at ~3600 rec/s, ~7 KB/s. Positional also means the number
    is readable straight off the config, and legal by construction."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(can_ports=["seg1", "seg2"]))
    assert [entry["label"] for entry in validated["can_ports"]] == ["C1", "C2"]


def test_v13_the_interface_number_follows_declaration_order_not_the_id(
    set_core_config,
) -> None:
    """The number is the position in `can_ports:`, so it does not move when the
    ports are named differently — and reading `C2` off a card means "the second
    entry in the config", whatever it is wired to."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(can_ports=["seg2", "seg1"]))
    assert [entry["label"] for entry in validated["can_ports"]] == ["C1", "C2"]


def test_v13_explicit_label_wins(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(can_ports=[{"port": "seg1", "label": "diag"}, "seg2"])
    )
    # Only the *default* changed: an explicit label still overrides, and the entry
    # that did not ask for one still gets its own interface number.
    assert [entry["label"] for entry in validated["can_ports"]] == ["diag", "C2"]


def test_v13_an_explicit_label_that_collides_with_a_default_is_rejected(
    set_core_config,
) -> None:
    """Positional defaults are unique among themselves, which is most of the point
    — but an explicit label can still walk into one. `label: C2` on the first
    interface is exactly the collision the numbering makes easy to write by
    accident, and two wires answering to one name makes every line ambiguous."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="claimed by both"):
        validate(sd_logger(can_ports=[{"port": "seg1", "label": "C2"}, "seg2"]))


@pytest.mark.parametrize("label", ["a", "seg1", "A-b_9", "abcdefgh"])
def test_v13_label_accepted(set_core_config, label: str) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(sources=[{"tag": 5, "kind": "lin", "label": label}])
    )
    assert validated["sources"][0]["label"] == label


@pytest.mark.parametrize(
    "label",
    [
        "",  # empty
        "abcdefghi",  # nine characters — over the byte budget
        "has space",
        "comma,here",  # would shift every later field
        "new\nline",  # would split the line in two
        "dot.ted",
    ],
)
def test_v13_bad_label_rejected(set_core_config, label: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="label"):
        validate(sd_logger(sources=[{"tag": 5, "kind": "lin", "label": label}]))


def test_v13_duplicate_label_across_can_ports_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="claimed by both"):
        validate(
            sd_logger(
                can_ports=[
                    {"port": "seg1", "label": "bus"},
                    {"port": "seg2", "label": "bus"},
                ]
            )
        )


def test_v13_duplicate_label_across_can_ports_and_sources_rejected(
    set_core_config,
) -> None:
    """The cross-block case: two wires answering to one label makes every line
    that carries it ambiguous, and neither block alone can see the collision."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="claimed by both"):
        validate(
            sd_logger(
                can_ports=[{"port": "seg1", "label": "lin"}],
                sources=[{"tag": 5, "kind": "lin", "label": "lin"}],
            )
        )


def test_v13_a_port_id_no_longer_has_to_be_a_usable_label(set_core_config) -> None:
    """PINNED: the id-derived default's failure mode is *gone*, not moved.

    It used to reject an id longer than 8 characters, because truncating one risked
    two ports silently sharing a label — so a config could be invalid for a reason
    that had nothing to do with logging. With the label coming from the interface
    number the id never reaches the file, so this config is simply valid."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(can_ports=["a_very_long_port_id"]))
    assert [entry["label"] for entry in validated["can_ports"]] == ["C1"]


# ------------------------------------------------------------------------- V14


def test_v14_sources_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(
            sources=[
                {"tag": 0, "kind": "user", "label": "act"},
                {"tag": 5, "kind": "lin", "label": "lin"},
            ]
        )
    )
    assert [entry["tag"] for entry in validated["sources"]] == [0, 5]
    assert [entry["kind"] for entry in validated["sources"]] == ["user", "lin"]


def test_v14_tag_zero_is_allowed_for_a_declared_source(set_core_config) -> None:
    """0 is the action's own tag; declaring it is how the action's records stop
    reading `U,0` and start reading by name."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(sources=[{"tag": 0, "kind": "user", "label": "act"}]))
    assert validated["sources"][0]["tag"] == 0


@pytest.mark.parametrize("kind", ["spi", "canfd", "isotp", ""])
def test_v14_unknown_kind_rejected(set_core_config, kind: str) -> None:
    """`isotp` is in the grammar as the `I` letter (S5) but has no producer yet,
    so accepting it here would promise a source that never writes a line."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(sources=[{"tag": 5, "kind": kind, "label": "x"}]))


@pytest.mark.parametrize("kind", ["CAN", "Lin"])
def test_v14_kind_is_case_insensitive(set_core_config, kind: str) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(sources=[{"tag": 5, "kind": kind, "label": "x"}]))
    assert validated["sources"][0]["kind"] == kind.lower()


@pytest.mark.parametrize("tag", [-1, 256])
def test_v14_tag_out_of_range_rejected(set_core_config, tag: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(sources=[{"tag": tag, "kind": "lin", "label": "x"}]))


def test_v14_tag_colliding_with_a_can_port_rejected(set_core_config) -> None:
    """seg1 defaults to tag 1; a source claiming 1 too would make one tag name
    two producers, and the `#src` header would contradict the lines below it."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="claimed by both"):
        validate(
            sd_logger(
                can_ports=["seg1"],
                sources=[{"tag": 1, "kind": "lin", "label": "lin"}],
            )
        )


def test_v14_duplicate_tag_within_sources_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="claimed by both"):
        validate(
            sd_logger(
                sources=[
                    {"tag": 5, "kind": "lin", "label": "a"},
                    {"tag": 5, "kind": "can", "label": "b"},
                ]
            )
        )


def test_sources_absent_by_default(set_core_config) -> None:
    setup_c6(set_core_config)
    assert "sources" not in validate(sd_logger())


def test_sources_empty_list_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(sources=[]))


# --------------------------------------------------------------------- V15-V17
#
# All three are ways the ESPHome log capture (S4) fails *silently*: the firmware
# boots, mounts, logs bus traffic, and simply has no `X` lines — which reads
# exactly like a quiet run.


def test_v15_esphome_logs_requires_a_logger_block(set_core_config) -> None:
    """Without `logger:`, request_log_listener() is never called, so
    add_log_callback() compiles to an empty function and capture does nothing."""
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={}))
    with pytest.raises(cv.Invalid, match="logger"):
        _run_final_validate(config, logger=None)


def test_v15_accepts_a_config_with_a_logger(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={}))
    assert _run_final_validate(config, logger=logger_config()) is config


def test_v15_noop_without_esphome_logs(set_core_config) -> None:
    """A logger with no capture configured must not require `logger:` at all."""
    setup_c6(set_core_config)
    config = validate(sd_logger())
    assert _run_final_validate(config, logger=None) is config


def test_v16_warns_when_the_logger_is_more_restrictive(set_core_config, caplog) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={"level": "DEBUG"}))
    _run_final_validate(config, logger=logger_config(level="WARN"))
    assert "only ever sees what the logger already let through" in caplog.text


def test_v16_quiet_when_the_logger_is_permissive_enough(
    set_core_config, caplog
) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={"level": "INFO"}))
    _run_final_validate(config, logger=logger_config(level="DEBUG"))
    assert "let through" not in caplog.text


def test_v17_warns_when_the_task_log_buffer_is_disabled(
    set_core_config, caplog
) -> None:
    """With it at 0, messages from non-main tasks bypass listeners entirely —
    which is exactly sd_logger's own writer and VCC-monitor tasks."""
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={}))
    _run_final_validate(config, logger=logger_config(task_log_buffer_size=0))
    assert "task_log_buffer_size" in caplog.text


def test_v17_quiet_with_a_task_log_buffer(set_core_config, caplog) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(esphome_logs={}))
    _run_final_validate(config, logger=logger_config(task_log_buffer_size=768))
    assert "task_log_buffer_size" not in caplog.text


# ------------------------------------------------------------------------- V18


def test_v18_defaults(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(esphome_logs={}))
    assert validated["esphome_logs"]["level"] == "INFO"
    assert validated["esphome_logs"]["buffer_depth"] == 32


def test_v18_config_level_is_accepted(set_core_config) -> None:
    """CONFIG is a real ESPHome level and gets its own letter in the file, even
    though logger's own LOG_LEVELS map omits it."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(esphome_logs={"level": "CONFIG"}))
    assert validated["esphome_logs"]["level"] == "CONFIG"


@pytest.mark.parametrize("depth", [8, 32, 256])
def test_v18_buffer_depth_accepted(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(esphome_logs={"buffer_depth": depth}))
    assert validated["esphome_logs"]["buffer_depth"] == depth


@pytest.mark.parametrize("depth", [24, 100])
def test_v18_buffer_depth_not_power_of_two_rejected(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="power of two"):
        validate(sd_logger(esphome_logs={"buffer_depth": depth}))


@pytest.mark.parametrize("depth", [4, 512])
def test_v18_buffer_depth_out_of_range_rejected(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 8 and 256"):
        validate(sd_logger(esphome_logs={"buffer_depth": depth}))


def test_esphome_logs_absent_by_default(set_core_config) -> None:
    setup_c6(set_core_config)
    assert "esphome_logs" not in validate(sd_logger())


# ------------------------------------------------------------------- V19 - V21
#
# Card recovery. The behaviour these guard is the difference between a bench day
# that survives a wedged card and one that ends: before recovery existed, any
# card failure — including the one a reflash causes by resetting the board
# mid-write — left `mounted=0` until someone cycled 12 V on J8.


def test_recovery_is_on_by_default(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger())
    assert validated["recovery"]["enabled"] is True
    assert validated["recovery"]["initial_delay"].total_milliseconds == 1000
    assert validated["recovery"]["max_delay"].total_milliseconds == 30_000
    assert validated["recovery"]["max_attempts"] == 0  # unlimited


def test_recovery_can_be_turned_off(set_core_config) -> None:
    """The pre-recovery behaviour has to stay reachable: a card failure is then a
    one-way trip for the run, which is what every earlier release did."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(recovery=recovery(enabled=False)))
    assert validated["recovery"]["enabled"] is False


@pytest.mark.parametrize(
    ("delay", "milliseconds"),
    [("100ms", 100), ("1s", 1000), ("45s", 45_000), ("300s", 300_000)],
)
def test_v20_delays_accepted(set_core_config, delay: str, milliseconds: int) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(recovery=recovery(initial_delay=delay, max_delay="300s"))
    )
    assert validated["recovery"]["initial_delay"].total_milliseconds == milliseconds


@pytest.mark.parametrize("delay", ["10ms", "99ms", "301s", "10min"])
def test_v20_delays_out_of_range_rejected(set_core_config, delay: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 100ms and 300s"):
        validate(sd_logger(recovery=recovery(initial_delay=delay)))


@pytest.mark.parametrize("key", ["initial_delay", "max_delay"])
def test_v20_both_delays_are_range_checked(set_core_config, key: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 100ms and 300s"):
        validate(sd_logger(recovery=recovery(**{key: "1ms"})))


def test_v20_max_delay_below_initial_rejected(set_core_config) -> None:
    """The ladder doubles from initial up to max, so a lower max steps backwards."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="ladder step backwards"):
        validate(
            sd_logger(recovery=recovery(initial_delay="10s", max_delay="1s"))
        )


def test_v20_max_delay_equal_to_initial_accepted(set_core_config) -> None:
    """Equal is a constant retry interval, which is a legitimate choice."""
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(recovery=recovery(initial_delay="5s", max_delay="5s"))
    )
    assert validated["recovery"]["max_delay"].total_milliseconds == 5000


@pytest.mark.parametrize("attempts", [0, 1, 10, 1000])
def test_recovery_max_attempts_accepted(set_core_config, attempts: int) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(recovery=recovery(max_attempts=attempts)))
    assert validated["recovery"]["max_attempts"] == attempts


@pytest.mark.parametrize("attempts", [-1, 1001])
def test_recovery_max_attempts_out_of_range_rejected(
    set_core_config, attempts: int
) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(recovery=recovery(max_attempts=attempts)))


def test_v19_power_cycle_defaults_on_with_a_card_power_pin(set_core_config) -> None:
    """Cycling the card is the whole reason the switch is on the board (spec §7
    H5), so its presence is the default answer."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(card_power_pin="GPIO2"))
    assert validated["recovery"]["power_cycle"] is True


def test_v19_power_cycle_defaults_off_without_one(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger())
    assert validated["recovery"]["power_cycle"] is False


def test_v19_power_cycle_true_without_a_pin_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="needs 'card_power_pin:'"):
        validate(sd_logger(recovery=recovery(power_cycle=True)))


def test_v19_power_cycle_can_be_declined_with_a_pin_present(set_core_config) -> None:
    """A board whose switch drives something else must be able to say so."""
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(card_power_pin="GPIO2", recovery=recovery(power_cycle=False))
    )
    assert validated["recovery"]["power_cycle"] is False


def test_v19_power_cycle_false_without_a_pin_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(recovery=recovery(power_cycle=False)))
    assert validated["recovery"]["power_cycle"] is False


def test_v21_warns_that_retries_never_format(set_core_config, caplog) -> None:
    """A retry ladder that reformatted would erase the logs it is being run to
    save, once per attempt — so it deliberately does not, and says so."""
    setup_c6(set_core_config)
    validate(sd_logger(format_if_mount_failed=True))
    assert "never formats" in caplog.text


def test_v21_quiet_without_format_if_mount_failed(set_core_config, caplog) -> None:
    setup_c6(set_core_config)
    validate(sd_logger(format_if_mount_failed=True, recovery=recovery(enabled=False)))
    assert "never formats" not in caplog.text
    validate(sd_logger())
    assert "never formats" not in caplog.text


# ------------------------------------------------------------------- V22 - V25
#
# Chunk collection (docs/sdlog-collection-design.md). Two new surfaces: a time
# bound on rotation, because an OPEN file is never servable and a quiet bus would
# otherwise trap the newest data indefinitely (design §4); and a `collection:`
# block for the private esp_http_server that serves sealed chunks (design §5).


def collection(**overrides: Any) -> dict[str, Any]:
    """A `collection:` block. Empty by default, so the block's own defaults apply.

    A value of ``None`` removes the key, so a test can assert what defaulting does.
    """
    return {key: value for key, value in overrides.items() if value is not None}


def wifi_config(**overrides: Any) -> dict[str, Any]:
    """A stand-in resolved `wifi:` block, as V22 sees it in the full config."""
    defaults: dict[str, Any] = {"ssid": "bench", "password": "bench-password"}
    return {**defaults, **overrides}


def web_server_config(**overrides: Any) -> dict[str, Any]:
    """A stand-in resolved `web_server:` block. `port` always carries a value in a
    resolved config — the component defaults it to 80 — so V25 can always read it."""
    defaults: dict[str, Any] = {"port": 80}
    return {**defaults, **overrides}


# ------------------------------------------------------------ V23 rotation time


def test_max_file_seconds_absent_by_default(set_core_config) -> None:
    """No default on purpose. `max_file_size` already guarantees rotation has a
    bound, so defaulting the time bound to '60s' would silently start rotating
    every minute on every already-released config — and rotation is the one path
    that has never fired on hardware (design §4)."""
    setup_c6(set_core_config)
    assert "max_file_seconds" not in validate(sd_logger())


@pytest.mark.parametrize(
    ("value", "seconds"),
    [
        (1, 1),
        (60, 60),
        ("60s", 60),
        ("2min", 120),
        ("1h", 3600),
        (3600, 3600),
        (60.0, 60),  # a whole number that YAML happened to read as a float
    ],
)
def test_v23_max_file_seconds_accepted(set_core_config, value, seconds: int) -> None:
    """A bare number of seconds or a duration string, exactly like `max_file_size`
    takes a bare byte count or '16MB'. Normalises to whole seconds."""
    setup_c6(set_core_config)
    assert validate(sd_logger(max_file_seconds=value))["max_file_seconds"] == seconds


@pytest.mark.parametrize(("value", "seconds"), [("1", 1), ("60", 60), ("3600", 3600)])
def test_v23_max_file_seconds_accepts_the_string_form(
    set_core_config, value: str, seconds: int
) -> None:
    """The spelling every `${substitution}` produces, and every quoted scalar.

    `max_file_seconds: ${chunk_seconds}` hands the validator the *string* `'60'`,
    never the int — substitution is textual and runs before validation. The first
    cut sent that straight to the duration parser, which reads a bare number as
    **milliseconds**, and then reported `'60'` as outside [1s, 3600s]: a true value
    rejected with a false reason, and unreachable from the YAML that produced it.
    `max_file_size` has always taken `'16777216'` as well as `16777216`, so the two
    rotation bounds now agree on the spelling too.
    """
    setup_c6(set_core_config)
    assert validate(sd_logger(max_file_seconds=value))["max_file_seconds"] == seconds


@pytest.mark.parametrize("value", [0, -1, 3601, "2h", "500ms", "0", "-5", 0.5, 3600.9])
def test_v23_max_file_seconds_out_of_range_rejected(set_core_config, value) -> None:
    """Below 1 s the writer would rotate faster than it can seal, and above an hour
    the chunk is no longer a cheap retry over car WiFi — 1-4 MB is the target.

    `3600.9` is here rather than in the precision case on purpose: it is genuinely
    past the ceiling, and it used to be truncated to a silently-accepted 3600.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 1s and 3600s"):
        validate(sd_logger(max_file_seconds=value))


@pytest.mark.parametrize("value", ["1500ms", "2500ms", "2.5s", 1.5, 1.9, 59.5])
def test_v23_max_file_seconds_sub_second_precision_rejected(
    set_core_config, value
) -> None:
    """In range, but finer than the writer can act on — and that is what it says.

    1.5 s sits comfortably inside [1s, 3600s]; the reason it cannot be honoured is
    that the rotation deadline is tested once per drain pass. Reporting it as out
    of range (which the first cut did, because every non-integer spelling fell into
    the same handler) sends the user looking for a bound that is not the problem.
    `1.9` is the same defect wearing a number: it used to truncate to 1 s silently.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="whole number of seconds"):
        validate(sd_logger(max_file_seconds=value))


@pytest.mark.parametrize(
    "value", [float("inf"), float("-inf"), float("nan"), ".inf", ".nan", "1e400"]
)
def test_v23_max_file_seconds_non_finite_rejected(set_core_config, value) -> None:
    """`.inf` and `.nan` are legal YAML, and both used to escape the validator.

    `int(float('inf'))` raises OverflowError — not `cv.Invalid`, not even
    `ValueError` — so voluptuous never converted it and `esphome config` ended in a
    traceback instead of a config error. `.nan` raised `ValueError` and surfaced as
    the generic "not a valid value for dictionary value @ data['max_file_seconds']",
    which never names the rule. Both must come back as V23, in the key's own words.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 1s and 3600s"):
        validate(sd_logger(max_file_seconds=value))


@pytest.mark.parametrize("value", [True, False])
def test_v23_max_file_seconds_boolean_rejected(set_core_config, value: bool) -> None:
    """`true` is an `int` in Python, so without the guard it validates as 1 s."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 1s and 3600s"):
        validate(sd_logger(max_file_seconds=value))


def test_v23_time_and_size_bounds_coexist(set_core_config) -> None:
    """Whichever fires first — the time bound does not replace the size bound."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(max_file_size="4MB", max_file_seconds="30s"))
    assert validated["max_file_size"] == 4 * 1024 * 1024
    assert validated["max_file_seconds"] == 30


# -------------------------------------------------------- the collection block


def test_collection_absent_by_default(set_core_config) -> None:
    """Serving log chunks off the card is opt-in: it costs a socket, a task and a
    port, and V22 makes it need WiFi."""
    setup_c6(set_core_config)
    assert "collection" not in validate(sd_logger())


def test_collection_defaults(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection()))["collection"]
    assert validated["enabled"] is True
    assert validated["port"] == 8080
    assert validated["retention_percent"] == 80
    assert validated["max_chunks"] == 256


def test_collection_can_be_declared_and_disabled(set_core_config) -> None:
    """`enabled: false` keeps the port and threshold written down while the server
    stays down — the same shape `recovery:` uses."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection(enabled=False)))["collection"]
    assert validated["enabled"] is False


def test_collection_serve_defaults_to_true(set_core_config) -> None:
    """The network half defaults on, so no config that validated before `serve:`
    existed changes meaning: a bare `collection:` still asks for a server and still
    has to satisfy V22 and V25."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection()))["collection"]
    assert validated["serve"] is True


def test_collection_serve_false_keeps_the_card_half(set_core_config) -> None:
    """`serve: false` is not `enabled: false`.

    `enabled` bounds the *card* — the chunk index, "drop oldest un-collected" and
    the `#gap` line that states the loss in-band. `serve` is only whether a puller
    ever comes to fetch the chunks. Collapsing the two is what left retention and
    `#gap` unreachable on a bench that must not run WiFi (design §9.1).
    """
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(collection=collection(serve=False, retention_percent=5))
    )["collection"]
    assert validated["enabled"] is True
    assert validated["serve"] is False
    assert validated["retention_percent"] == 5


@pytest.mark.parametrize("value", ["maybe", 2, None])
def test_collection_serve_rejects_a_non_boolean(set_core_config, value: Any) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid):
        validate(sd_logger(collection={"serve": value}))


def test_collection_unknown_key_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    # Guard: the block itself has to validate, or a rejection of the whole
    # `collection:` key would make this pass for entirely the wrong reason.
    validate(sd_logger(collection=collection()))
    with pytest.raises(cv.Invalid):
        validate(sd_logger(collection={"retention_pct": 50}))


# ------------------------------------------------------------------------- V24


@pytest.mark.parametrize("percent", [2, 50, 80, 98])
def test_v24_retention_percent_accepted(set_core_config, percent: int) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection(retention_percent=percent)))
    assert validated["collection"]["retention_percent"] == percent


@pytest.mark.parametrize("percent", [-1, 0, 1, 99, 100, 150])
def test_v24_retention_percent_out_of_range_rejected(
    set_core_config, percent: int
) -> None:
    """Both ends are exclusive. At 99 % the headroom left is smaller than one
    chunk, so retention would only arm once the card is already full and logging
    has stopped; at 1 % it would delete chunks as fast as the writer seals them,
    which is a permanent gap dressed up as a policy."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="strictly between 1 and 99"):
        validate(sd_logger(collection=collection(retention_percent=percent)))


# ------------------------------------------------------------- collection.port


@pytest.mark.parametrize("port", [1, 80, 8080, 65535])
def test_collection_port_accepted(set_core_config, port: int) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection(port=port)))
    assert validated["collection"]["port"] == port


@pytest.mark.parametrize("port", [-1, 0, 65536])
def test_collection_port_out_of_range_rejected(set_core_config, port: int) -> None:
    setup_c6(set_core_config)
    # Same guard as above: without it a rejection of the whole block would read
    # as a working port range check.
    validate(sd_logger(collection=collection(port=8080)))
    with pytest.raises(cv.Invalid):
        validate(sd_logger(collection=collection(port=port)))


# ------------------------------------------------------------------------- V22
#
# Final validate: the collector is reached over WiFi, so a `collection:` block
# without a `wifi:` block is a server nobody can ever connect to — and it fails
# the way every other silent M6 failure does, by looking like a healthy boot.


def test_v22_collection_requires_wifi(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection()))
    with pytest.raises(cv.Invalid, match="wifi"):
        _run_final_validate(config, wifi=None)


def test_v22_accepts_a_config_with_wifi(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection()))
    assert _run_final_validate(config, wifi=wifi_config()) is config


def test_v22_noop_without_collection(set_core_config) -> None:
    """A logger that only writes to the card must not start needing WiFi."""
    setup_c6(set_core_config)
    config = validate(sd_logger())
    assert _run_final_validate(config, wifi=None) is config


def test_v22_disabled_collection_does_not_need_wifi(set_core_config) -> None:
    """`enabled: false` is declared-but-down, and V22 is about a running server.

    The block keeps the port and the threshold written down while no socket is
    opened — the state `CollectionPolicy::disable()` exists for and the shape
    `recovery:` already uses. Demanding `wifi:` for a server that will never start
    is an argument about nothing, and it makes commenting the block out the only
    way to boot without WiFi, which throws the settings away instead of parking
    them. V21 gates its own diagnostic on `recovery[enabled]` for the same reason.
    """
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(enabled=False, port=8080)))
    assert _run_final_validate(config, wifi=None) is config


def test_v22_still_fires_when_the_block_is_re_enabled(set_core_config) -> None:
    """The other half of the gate: `enabled: false` is the exemption, not the key
    being present. Flip it back and the same config is rejected again."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(enabled=True, port=8080)))
    with pytest.raises(cv.Invalid, match="wifi"):
        _run_final_validate(config, wifi=None)


def test_v22_serve_false_does_not_need_wifi(set_core_config) -> None:
    """The exemption the bench runs on, and the reason `serve:` exists.

    V22 is about a server that will actually start. `serve: false` asks for none,
    while keeping the half of the block that has nothing to do with the network:
    the chunk index, retention and the `#gap` line. Without this, arming retention
    required associating to WiFi — whose tasks sit above the LIN task's priority
    and whose effect on bus timing nobody has measured (design §9.1) — so the only
    way to soak rotation and `#gap` on hardware was to confound it with a second
    unknown. That is why no bench config carried either key.
    """
    setup_c6(set_core_config)
    config = validate(
        sd_logger(collection=collection(serve=False, retention_percent=5))
    )
    assert _run_final_validate(config, wifi=None) is config


def test_v22_still_fires_when_serve_is_turned_back_on(set_core_config) -> None:
    """The other half of that gate: `serve: false` is the exemption, not the block
    being present. Ask for a server again and the same config is rejected again."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(serve=True, port=8080)))
    with pytest.raises(cv.Invalid, match="wifi"):
        _run_final_validate(config, wifi=None)


def test_v22_ethernet_does_not_satisfy_the_wifi_requirement(set_core_config) -> None:
    """Recording a decision rather than discovering one.

    V22 asks for `wifi:` by name (design §11), so a board with an SPI Ethernet
    controller and a perfectly good IP stack is still rejected. That is deliberate
    for now — the reference PCB and every bench board is WiFi-only, the design doc
    scopes collection to WiFi throughout (§9.1 measures WiFi against the LIN task's
    priority), and a rule that accepted any network component would have to make a
    claim about SPI contention between a W5500 and the card on the same bus that
    nobody has measured. If an Ethernet C6 ever needs collection, widen the rule
    here and in the design doc — do not delete this test, replace it.
    """
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection()))
    with pytest.raises(cv.Invalid, match="wifi"):
        _run_final_validate(config, wifi=None, ethernet={"type": "W5500"})


# ------------------------------------------------------------------------- V25
#
# The collection server is a private esp_http_server instance, deliberately not
# esphome's `web_server` (design §5a). Two servers binding one port is a runtime
# failure with no config-time symptom, so it is caught here.


def test_v25_port_colliding_with_web_server_rejected(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=80)))
    with pytest.raises(cv.Invalid, match="web_server"):
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )


def test_v25_collision_with_the_web_server_default_port_rejected(
    set_core_config,
) -> None:
    """web_server's port defaults to 80 and is never written out by the user, so
    the collision that actually happens is against a port nobody typed."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=80)))
    with pytest.raises(cv.Invalid, match="web_server"):
        _run_final_validate(config, wifi=wifi_config(), web_server=web_server_config())


def test_v25_distinct_ports_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=8080)))
    assert (
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )
        is config
    )


def test_v25_noop_without_web_server(set_core_config) -> None:
    """Port 80 is a perfectly good choice when nothing else wants it."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=80)))
    assert _run_final_validate(config, wifi=wifi_config(), web_server=None) is config


def test_v25_noop_without_collection(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger())
    assert (
        _run_final_validate(config, web_server=web_server_config(port=80)) is config
    )


def test_v25_disabled_collection_may_name_the_web_server_port(set_core_config) -> None:
    """No socket is opened, so there is no bind to lose.

    Same gate as V22: with `enabled: false` the port is a parked setting, not a
    claim on the machine, and rejecting it invents a race between one server and a
    server that does not start.
    """
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(enabled=False, port=80)))
    assert (
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )
        is config
    )


def test_v25_still_fires_when_the_block_is_re_enabled(set_core_config) -> None:
    """And the moment that parked port is turned back on, it collides again."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(enabled=True, port=80)))
    with pytest.raises(cv.Invalid, match="web_server"):
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )


def test_v25_serve_false_may_name_the_web_server_port(set_core_config) -> None:
    """Same gate as V22 and for the same reason: `serve: false` binds nothing, so
    the port is a parked setting rather than a claim on the machine."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(serve=False, port=80)))
    assert (
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )
        is config
    )


def test_v25_web_server_given_as_a_list_is_still_checked(set_core_config) -> None:
    """The normalising arm, which no test reached before.

    Against pinned esphome (2026.7.0) `web_server:` is single-instance and resolves
    to a dict, so this shape cannot occur today — which is exactly why the branch
    needs a test rather than a hope: it is the arm that would be silently skipping
    the check if `web_server` ever became MULTI_CONF, and a dead branch that stops
    being dead is not something a config error would announce.
    """
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=80)))
    with pytest.raises(cv.Invalid, match="web_server"):
        _run_final_validate(
            config, wifi=wifi_config(), web_server=[web_server_config(port=80)]
        )


def test_v25_web_server_list_without_a_collision_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=8080)))
    assert (
        _run_final_validate(
            config,
            wifi=wifi_config(),
            web_server=[web_server_config(port=80), web_server_config(port=8081)],
        )
        is config
    )


# ------------------------------------------------------------------------- V26
#
# `collection: max_chunks:` sizes the static chunk index — `SD_LOG_MAX_CHUNKS`,
# emitted as a define from the component's Python because the index is a static
# array in collection_policy.h and an external component may not touch esphome
# core's defines.h. It is RAM against coverage: 12 B an entry, so 3 KB at the
# default and 24 KB at the ceiling.
#
# The reason it is a key at all rather than a constant: a 12-minute PERF soak on
# the bench (2026-07-28) rotated 29 times and found the then-fixed 64-entry index
# **already full at the first rotation**. Every chunk of that run was untracked,
# so nothing was listed, nothing was servable and retention could reclaim nothing
# — while the card kept filling. `add()` refuses rather than evicting, because
# forgetting a file that still exists is the one failure retention cannot come
# back from. That makes the capacity a policy number a config has to be able to
# state, and this range is what keeps a typo from asking for 768 KB of it.


@pytest.mark.parametrize("chunks", [16, 32, 64, 256, 1024, 2048])
def test_v26_max_chunks_accepted(set_core_config, chunks: int) -> None:
    """Both bounds are inclusive, and 256 is the default written out."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection(max_chunks=chunks)))
    assert validated["collection"]["max_chunks"] == chunks


def test_v26_max_chunks_defaults_to_256(set_core_config) -> None:
    """Absent means 256, which is the number the soak that produced this key
    settled on: at the 4 MB / 60 s rotation bounds a chunk lands roughly every
    25 s, so 64 entries was ~27 minutes of cumulative logging and 256 is ~1.8
    hours. A `collection:` block that says nothing still gets a usable index."""
    setup_c6(set_core_config)
    assert validate(sd_logger(collection=collection()))["collection"]["max_chunks"] == 256


def test_v26_the_schema_default_matches_the_headers_own_fallback(
    set_core_config,
) -> None:
    """The two defaults have to be the same number, and nothing else would say so.

    `SD_LOG_MAX_CHUNKS` is `#define`d in collection_policy.h behind `#ifndef`, and
    the schema's default is what `to_code()` emits over it. A firmware build always
    takes the schema's value, but the host tests in tests/host/ compile the header
    alone and take the fallback — so if the two drift, the index the C++ cases
    reason about stops being the index the bench runs, and both sides still build.
    The comment on the key in `__init__.py` states the promise ("a config that omits
    the key and one that writes `max_chunks: 256` produce the same firmware"); this
    is the only thing that checks it.

    The schema range must also fit inside what the data structure can represent:
    `count_` is a uint16_t, and the header's static_assert bounds it at 65535.
    """
    import re
    from pathlib import Path

    from esphome.components import sd_logger as module

    setup_c6(set_core_config)
    header = (Path(module.__file__).parent / "collection_policy.h").read_text()
    match = re.search(r"^#define\s+SD_LOG_MAX_CHUNKS\s+(\d+)\s*$", header, re.M)
    assert match is not None, "collection_policy.h no longer defines SD_LOG_MAX_CHUNKS"
    schema_default = validate(sd_logger(collection=collection()))["collection"][
        "max_chunks"
    ]
    assert int(match.group(1)) == schema_default, (
        f"collection_policy.h falls back to SD_LOG_MAX_CHUNKS "
        f"{match.group(1)} but `collection: max_chunks:` defaults to "
        f"{schema_default}. A firmware build takes the schema's number and the "
        f"host tests take the header's, so the two must agree or tests/host/ is "
        f"reasoning about a different index than the bench runs."
    )
    assert module.MAX_CHUNKS_MIN >= 1 and module.MAX_CHUNKS_MAX <= 65535, (
        "the config range must stay inside the header's static_assert [1, 65535]; "
        "`count_` is a uint16_t"
    )


@pytest.mark.parametrize("chunks", [15, 2049, 0, -1, -256, 8, 4096, 65535])
def test_v26_max_chunks_out_of_range_rejected(set_core_config, chunks: int) -> None:
    """The floor is 16 rather than 1 because the boot scan reserves one slot for the
    file it is about to open and retention needs a history to choose a victim from:
    an index of 2 is not a small index, it is a broken one, and its refusals look
    exactly like the bug this range exists to prevent. The ceiling is 24 KB of a
    chip with ~100-200 KB free."""
    setup_c6(set_core_config)
    # Guard: without it a rejection of the whole `collection:` block would read as
    # a working range check. Same shape as the port and retention_percent cases.
    validate(sd_logger(collection=collection(max_chunks=256)))
    with pytest.raises(cv.Invalid, match="must be between 16 and 2048"):
        validate(sd_logger(collection=collection(max_chunks=chunks)))


def test_v26_the_rejection_says_what_the_number_costs_and_what_it_buys(
    set_core_config,
) -> None:
    """A range alone would not have prevented the bug that produced this key.

    "must be between 16 and 2048" tells someone whose index filled nothing they can
    act on: the question they have is "what do I set it to", and the answer depends
    on how long the device runs between collections and how fast chunks land. So the
    message has to carry the offending value, the cost per entry, the consequence of
    being too small, and a rate to size against — otherwise the next person to hit a
    full index reads a bound and guesses, which is how 64 survived as long as it did.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid) as excinfo:
        validate(sd_logger(collection=collection(max_chunks=4096)))
    message = str(excinfo.value)
    assert "max_chunks" in message  # the key, by name
    assert "4096" in message  # what was actually written
    assert "12 bytes" in message  # the cost per entry
    assert "24 KB" in message  # what the ceiling costs
    # The consequence, which is the whole reason the index refuses rather than evicts.
    assert "never listed" in message and "reclaimed by retention" in message
    assert "25 s" in message  # a rate to size against


@pytest.mark.parametrize("value", ["256", "16", "2048"])
def test_v26_max_chunks_accepts_the_string_form(set_core_config, value: str) -> None:
    """The spelling every `${substitution}` produces.

    `max_chunks: ${chunk_index}` hands the validator the *string* `'256'`, never the
    int — substitution is textual and runs before validation. Every bench rig sizes
    its collection block through substitutions (`retention_at` in
    tests/hil/stress-orange-sdrotate.yaml), so this is the spelling that actually
    reaches the schema on hardware, and V23 already had to learn it the hard way.
    """
    setup_c6(set_core_config)
    validated = validate(sd_logger(collection=collection(max_chunks=value)))
    assert validated["collection"]["max_chunks"] == int(value)


@pytest.mark.parametrize("value", ["many", "", None, [256], {"chunks": 256}, "16 KB"])
def test_v26_max_chunks_non_integer_rejected(set_core_config, value: Any) -> None:
    """Not a number at all. The message still names the key, which is what makes it
    findable from the YAML that produced it.

    Written as a raw dict rather than through `collection()`, which drops `None`
    values so a test can assert what defaulting does — going through it would test
    the default instead of the rejection, which is how `max_chunks: ~` would have
    slipped past as a silent 256.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="max_chunks"):
        validate(sd_logger(collection={"max_chunks": value}))


@pytest.mark.parametrize("value", [True, False])
def test_v26_max_chunks_boolean_rejected(set_core_config, value: bool) -> None:
    """`true` is an `int` in Python, so it reaches the range check as 1 and is
    refused there — which is the right answer, and not one the validator has to
    special-case the way V23's does, because the floor is 16 and not 1."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="must be between 16 and 2048"):
        validate(sd_logger(collection=collection(max_chunks=value)))


@pytest.mark.parametrize("value", [256.5, 15.5, 0.5])
def test_v26_max_chunks_fractional_rejected(set_core_config, value: float) -> None:
    """Rejected, and told the truth about why — the index is a static array and half
    an entry is not a thing to allocate.

    The message matters here and this test exists to pin it. `256.5` must NOT come
    back as "must be between 16 and 2048", because 256.5 *is* between 16 and 2048:
    a bounds message sends the reader to check a bound that is not the problem.
    `_validate_max_chunks` originally wrapped every `cv.Invalid` from `cv.int_range`
    in the range message and did exactly that; it now asks the value whether it is
    whole and picks the reason to match. V23 split the same two apart in
    test_v23_max_file_seconds_sub_second_precision_rejected.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="must be a whole number of chunks"):
        validate(sd_logger(collection=collection(max_chunks=value)))


@pytest.mark.parametrize("value", [float("inf"), float("-inf")])
def test_v26_max_chunks_non_finite_rejected(set_core_config, value: float) -> None:
    """`.inf` and `-.inf` are legal YAML and must come back as V26, in the key's own
    words — a config error a user can act on, not a Python traceback.

    This was a real defect when the test was written: `.inf` reached `cv.int_`, whose
    `int(value) == value` raises OverflowError — neither `cv.Invalid` nor `ValueError`,
    so voluptuous does not convert it and the wrapper's `except cv.Invalid` never saw
    it. V23 had already found and fixed the identical defect for `max_file_seconds`;
    V26 reintroduced it, and `_validate_max_chunks` now rejects non-finite floats
    explicitly, before `cv.int_range` can see them.

    `.nan` already works, and for an accident: `int(float('nan'))` raises ValueError,
    which voluptuous converts to Invalid on its way out of `cv.int_`, so the wrapper
    catches it. OverflowError has no such conversion. Verified against pinned esphome
    with `esphome config` on a real yaml, so this is not a fixture artefact.
    """
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="must be between 16 and 2048"):
        validate(sd_logger(collection=collection(max_chunks=value)))


def test_v26_nan_is_rejected_as_a_config_error(set_core_config) -> None:
    """The sibling of the case above that does behave: `.nan` is legal YAML too."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="must be between 16 and 2048"):
        validate(sd_logger(collection=collection(max_chunks=float("nan"))))


def test_v26_max_chunks_is_a_collection_key_not_a_top_level_one(
    set_core_config,
) -> None:
    """It sizes the chunk index, and the chunk index is what `collection:` is.

    Written at the top level it is rejected as an unknown key — a plain "extra keys
    not allowed", which is the right answer but not a helpful one, so pin it: a user
    who indents it wrong gets told the key does not exist there rather than having it
    quietly ignored, and the guard below is what proves the same value is fine one
    level in.
    """
    setup_c6(set_core_config)
    assert (
        validate(sd_logger(collection=collection(max_chunks=512)))["collection"][
            "max_chunks"
        ]
        == 512
    )
    with pytest.raises(cv.Invalid, match="extra keys not allowed"):
        validate(sd_logger(max_chunks=512))


def test_v26_no_collection_block_means_no_max_chunks(set_core_config) -> None:
    """No block, no key, and so no `SD_LOG_MAX_CHUNKS` define: `to_code()` emits it
    from inside the `collection:` branch, so a logger that only writes to the card
    keeps the header's own fallback."""
    setup_c6(set_core_config)
    assert "collection" not in validate(sd_logger())


# --- V26 against the other collection validators -----------------------------
#
# `enabled` is the card half and `serve` the network half (design §11), and
# `max_chunks` belongs squarely to the card half: it sizes the index that
# retention chooses victims from. So it has to keep working in exactly the shapes
# V22 and V25 exempt — which are the shapes the bench actually soaks in.


def test_v26_max_chunks_survives_the_serve_false_shape(set_core_config) -> None:
    """The bench shape, and the one that matters most for this key.

    `serve: false` is how tests/hil/stress-orange-sdrotate.yaml soaks rotation and
    retention without WiFi (whose tasks sit above the LIN task's priority, design
    §9.1). That is also the run that found the index full at the first of 29
    rotations — so if `max_chunks` were only honoured in the serving shape, the one
    configuration that can reproduce the bug would be the one that cannot fix it.
    V22 must stay exempt with the key present.
    """
    setup_c6(set_core_config)
    config = validate(
        sd_logger(
            collection=collection(serve=False, retention_percent=5, max_chunks=2048)
        )
    )
    assert config["collection"]["max_chunks"] == 2048
    assert config["collection"]["enabled"] is True
    assert config["collection"]["serve"] is False
    assert _run_final_validate(config, wifi=None) is config


def test_v26_max_chunks_does_not_excuse_a_missing_wifi(set_core_config) -> None:
    """The other half of that gate: sizing the index is not asking for a server, and
    it is not declining one either. With `serve:` left at its default, V22 fires
    exactly as it did before the key existed."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(max_chunks=2048)))
    with pytest.raises(cv.Invalid, match="wifi"):
        _run_final_validate(config, wifi=None)


def test_v26_max_chunks_is_kept_while_the_block_is_disabled(set_core_config) -> None:
    """`enabled: false` is declared-but-down: the index is not built, but the number
    stays written down, the same way the port and the retention threshold do.
    Dropping it here would make turning collection back on a silent resize."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(enabled=False, max_chunks=16)))
    assert config["collection"]["max_chunks"] == 16
    assert _run_final_validate(config, wifi=None) is config


def test_v26_max_chunks_does_not_excuse_a_v25_port_collision(set_core_config) -> None:
    """Two servers on one port is still two servers on one port. A large index is not
    a claim about the network, so V25 must not read it as one."""
    setup_c6(set_core_config)
    config = validate(sd_logger(collection=collection(port=80, max_chunks=2048)))
    with pytest.raises(cv.Invalid, match="web_server"):
        _run_final_validate(
            config, wifi=wifi_config(), web_server=web_server_config(port=80)
        )


def test_v26_the_full_card_half_validates_together(set_core_config) -> None:
    """V24 and V26 are one decision in practice — how much history is kept and how
    much of it the index can see — so pin them holding their values side by side in
    the shape the bench runs, rather than only one at a time.

    A card whose retention arms at 2 % and whose index tracks 2048 chunks is the
    soak config: retention fires on the first pass, and the index is wide enough
    that what it reclaims is chosen from the real history rather than from whatever
    the boot scan happened to fit.
    """
    setup_c6(set_core_config)
    config = validate(
        sd_logger(
            max_file_size="4MB",
            max_file_seconds="60s",
            collection=collection(
                enabled=True, serve=False, retention_percent=2, max_chunks=2048
            ),
        )
    )
    assert config["max_file_size"] == 4 * 1024 * 1024
    assert config["max_file_seconds"] == 60
    assert config["collection"] == {
        "enabled": True,
        "serve": False,
        "port": 8080,
        "retention_percent": 2,
        "max_chunks": 2048,
    }
    assert _run_final_validate(config, wifi=None) is config


# ------------------------------------------------- the M6 codegen seam (deferred)


def test_m6_keys_are_wired_into_codegen_exactly_when_the_setters_exist(
    set_core_config,
) -> None:
    """`max_file_seconds:` and `collection:` validate but generate nothing yet.

    This round is schema and validation only: the C++ setters do not exist, and
    emitting calls to them would break `esphome compile` for everyone. So
    `tests/build/sd_logger/common.yaml` carries both keys and the firmware ignores
    them — acceptable while it is true on *both* sides, and invisible the moment it
    is not. A build that accepts `retention_percent: 80` and never arms retention
    looks exactly like one that does.

    So this asserts the two sides agree rather than asserting today's answer: it
    passes now (neither side has them), passes once the runtime lands and `to_code`
    is wired, and fails only in the state nobody else would notice — a declared
    setter that is never called. Comment lines in `to_code()` do not count as a
    call, which is what makes the TODO block there honest instead of load-bearing.
    """
    import inspect
    from pathlib import Path

    from esphome.components import sd_logger as module

    setup_c6(set_core_config)
    header = (Path(module.__file__).parent / "sd_logger.h").read_text()
    body = [
        line
        for line in inspect.getsource(module.to_code).splitlines()
        if not line.lstrip().startswith("#")
    ]
    for setter in ("set_max_file_seconds", "set_collection"):
        declared = f"{setter}(" in header
        called = any(setter in line for line in body)
        assert declared == called, (
            f"SdLogger::{setter}() is declared={declared} in sd_logger.h but "
            f"called={called} from sd_logger's to_code(). The M6 keys are wired "
            f"into codegen exactly when the runtime setters exist: if the setter "
            f"just landed, uncomment the matching call in the TODO(M6) block in "
            f"components/sd_logger/__init__.py, because the config key is "
            f"otherwise accepted and silently ignored."
        )


# ------------------------------------------------- the action's source spelling


def _resolve(value):
    from esphome.components.sd_logger import _resolve_action_source

    return _resolve_action_source(value)


def test_action_source_accepts_a_declared_label(set_core_config) -> None:
    """`source: lin` and `source: 5` must produce the same tag — the label is a
    spelling, not a second numbering."""
    setup_c6(set_core_config)
    config = validate(
        sd_logger(
            can_ports=["seg1"],
            sources=[{"tag": 5, "kind": "lin", "label": "lin"}],
        )
    )
    _run_final_validate(config, True)
    assert _resolve("lin") == 5
    # A tapped port answers to the label it writes into the file — its interface
    # number — and not to the ESPHome id of the port, which never reaches the card.
    assert _resolve("C1") == 1
    assert _resolve(5) == 5
    with pytest.raises(cv.Invalid, match="does not name a declared source"):
        _resolve("seg1")


def test_action_source_rejects_an_undeclared_label(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(sd_logger(sources=[{"tag": 5, "kind": "lin", "label": "lin"}]))
    _run_final_validate(config)
    with pytest.raises(cv.Invalid, match="does not name a declared source"):
        _resolve("nope")


# ------------------------------------------------------------------- V27 - V28
#
# The in-band card reset. This is the recovery step that needs no hardware, and
# on the reference board it is the only one that can work at all: `card_power_pin`
# reaches header J5.6 and nothing else, so every "power-cycling the card" line the
# bench has ever printed was toggling a pin with no switch behind it. What a reset
# mid-write actually leaves is a multi-block write the card is still waiting to
# have ended, and the budget for waiting it out is what these keys set.


def test_in_band_reset_is_on_by_default(set_core_config) -> None:
    setup_c6(set_core_config)
    validated = validate(sd_logger())
    assert validated["recovery"]["in_band_reset"] is True
    assert validated["recovery"]["busy_timeout"].total_milliseconds == 2000
    assert validated["recovery"]["max_busy_timeout"].total_milliseconds == 10_000


def test_in_band_reset_can_be_turned_off(set_core_config) -> None:
    """The escape hatch for a board where something else shares the SPI pins."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(recovery=recovery(in_band_reset=False)))
    assert validated["recovery"]["in_band_reset"] is False


def test_in_band_reset_survives_recovery_disabled(set_core_config) -> None:
    """`enabled: false` stops the ladder; the reset keys still validate, because
    the C++ takes them either way and a config that flips recovery back on must
    not have to restate them."""
    setup_c6(set_core_config)
    validated = validate(sd_logger(recovery=recovery(enabled=False)))
    assert validated["recovery"]["in_band_reset"] is True


@pytest.mark.parametrize(
    ("timeout", "milliseconds"),
    [("100ms", 100), ("2s", 2000), ("30s", 30_000), ("60s", 60_000)],
)
def test_v27_busy_timeouts_accepted(
    set_core_config, timeout: str, milliseconds: int
) -> None:
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(recovery=recovery(busy_timeout=timeout, max_busy_timeout="60s"))
    )
    assert validated["recovery"]["busy_timeout"].total_milliseconds == milliseconds


@pytest.mark.parametrize("timeout", ["10ms", "99ms", "61s", "5min"])
def test_v27_busy_timeouts_out_of_range_rejected(set_core_config, timeout: str) -> None:
    """Below 100 ms it is no more patient than the driver's own 40 ms — the exact
    limitation the sequence exists to lift — and above 60 s one attempt outlasts
    what an orderly shutdown can wait for."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 100ms and 60s"):
        validate(sd_logger(recovery=recovery(busy_timeout=timeout)))


@pytest.mark.parametrize("key", ["busy_timeout", "max_busy_timeout"])
def test_v27_both_budgets_are_range_checked(set_core_config, key: str) -> None:
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="between 100ms and 60s"):
        validate(sd_logger(recovery=recovery(**{key: "1ms"})))


def test_v28_max_busy_below_initial_rejected(set_core_config) -> None:
    """The budget doubles up to the maximum on each further attempt, so a lower
    maximum gives a card that needs more patience less of it."""
    setup_c6(set_core_config)
    with pytest.raises(cv.Invalid, match="more patience get less of it"):
        validate(
            sd_logger(recovery=recovery(busy_timeout="10s", max_busy_timeout="1s"))
        )


def test_v28_max_busy_equal_to_initial_accepted(set_core_config) -> None:
    """Equal is a constant budget on every attempt, which is a legitimate choice."""
    setup_c6(set_core_config)
    validated = validate(
        sd_logger(recovery=recovery(busy_timeout="5s", max_busy_timeout="5s"))
    )
    assert validated["recovery"]["max_busy_timeout"].total_milliseconds == 5000
