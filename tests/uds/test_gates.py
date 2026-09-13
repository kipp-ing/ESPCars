"""The gates — U4, U10, U11, U12, U13.

Everything here needs the *full* config rather than one block, which is why it lives in
FINAL_VALIDATE_SCHEMA and gets its own suite. Two of the five are safety rules and are the reason
this component may transmit at all (design §6): the scheduler and `uds.read` are SAFE_READ-only
whatever else is configured, and everything that can change ECU state is behind an explicit flag.

The other three prevent quiet failures rather than hazards. U12 in particular: a real multi-block
response against a too-small `max_message_size: 256` is an `OVERFLOW_LOCAL`, which on a bench looks
exactly like a flaky ECU — so it is a build error naming both numbers.
"""

from __future__ import annotations

import pytest

import esphome.config_validation as cv
from esphome.core import TimePeriodMilliseconds

from .glue import (
    MINI,
    catalog,
    hub,
    isotp_entry,
    partition_catalog,
    run_hub_final_validate,
    sensor_entry,
    setup_c6,
    validate,
    validate_sensor,
    validate_text_sensor,
)


def _bound(field: str, *, text: bool = False, **overrides):
    """One entity as it appears in the full config: validated, with `platform:` put back."""
    entry = sensor_entry(field=field, **overrides)
    validated = validate_text_sensor(entry) if text else validate_sensor(entry)
    return {**validated, "platform": "uds"}


def _read(hub_id, service):
    return {"uds.read": {"id": hub_id, "service": service}}


def _execute(hub_id, service):
    return {"uds.execute": {"id": hub_id, "service": service}}


def _raw(hub_id):
    return {"uds.raw": {"id": hub_id, "data": [0x22, 0x01, 0x01]}}


def _automation(*actions):
    """The shape a validated `esphome: on_boot:` leaves in the full config."""
    return {"esphome": {"on_boot": [{"priority": 600.0, "then": list(actions)}]}}


# ------------------------------------------------------------------------------ U4: `ecu:`


def test_single_ecu_catalog_needs_no_ecu_key(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    assert run_hub_final_validate(config) is config


def test_named_ecu_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), ecu="MINI"))
    assert run_hub_final_validate(config) is config


def test_unknown_ecu_lists_what_the_catalog_holds(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), ecu="EGS"))
    with pytest.raises(cv.Invalid) as excinfo:
        run_hub_final_validate(config)
    message = str(excinfo.value)
    assert "unknown ecu 'EGS'" in message
    assert "MINI" in message


# -------------------------------------------------------------- U10: uds.read is SAFE_READ only


def test_read_of_a_safe_group_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    assert run_hub_final_validate(
        config, extra=_automation(_read(config["id"], "DT_Mini_Contactor_State"))
    ) is config


def test_read_of_an_unsafe_group_rejected(set_core_config) -> None:
    """`RT_Mini_Routine_Start` is SID 0x31 — a routine, not a read. The flag does not unlock it."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    with pytest.raises(cv.Invalid) as excinfo:
        run_hub_final_validate(config, extra=_automation(_read(config["id"], "RT_Mini_Routine_Start")))
    message = str(excinfo.value)
    assert "SAFE_READ groups only" in message
    assert "0x31" in message
    assert "uds.execute" in message  # and where it would belong


def test_read_of_an_unsafe_group_rejected_even_with_the_flag(set_core_config) -> None:
    """This is the point of the rule: `uds.read` runs unattended, so it is bounded absolutely."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), allow_active_services=True))
    with pytest.raises(cv.Invalid, match="SAFE_READ groups only"):
        run_hub_final_validate(config, extra=_automation(_read(config["id"], "RT_Mini_Routine_Start")))


# --------------------------------------------------- U11: execute and raw need the explicit flag


@pytest.mark.parametrize("make_action", [_execute, lambda h, _s: _raw(h)])
def test_active_actions_rejected_without_the_flag(set_core_config, make_action) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    with pytest.raises(cv.Invalid) as excinfo:
        run_hub_final_validate(
            config, extra=_automation(make_action(config["id"], "RT_Mini_Routine_Start"))
        )
    message = str(excinfo.value)
    assert "allow_active_services: true" in message
    assert "can change ECU state" in message


@pytest.mark.parametrize("make_action", [_execute, lambda h, _s: _raw(h)])
def test_active_actions_accepted_with_the_flag(set_core_config, make_action) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), allow_active_services=True))
    assert run_hub_final_validate(
        config, extra=_automation(make_action(config["id"], "RT_Mini_Routine_Start"))
    ) is config


def test_another_hubs_flag_does_not_unlock_this_one(set_core_config) -> None:
    """The gate is per hub, so a permissive tester on one bus cannot speak for another."""
    setup_c6(set_core_config)
    strict = validate(hub(catalog=catalog(MINI)))
    with pytest.raises(cv.Invalid, match="allow_active_services"):
        run_hub_final_validate(strict, extra=_automation(_execute(strict["id"], "RT_Mini_Routine_Start")))


def test_actions_nested_deep_in_an_automation_are_still_gated(set_core_config) -> None:
    """Actions have no final-validate hook of their own, so the walk has to reach any depth."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    nested = {
        "interval": [
            {
                "interval": TimePeriodMilliseconds(milliseconds=5000),
                "then": [
                    {
                        "if": {
                            "condition": [],
                            "then": [_execute(config["id"], "RT_Mini_Routine_Start")],
                        }
                    }
                ],
            }
        ]
    }
    with pytest.raises(cv.Invalid, match="allow_active_services"):
        run_hub_final_validate(config, extra=nested)


# ------------------------------------------------------------- U12: the transport must be big enough


def test_transport_too_small_names_both_numbers(set_core_config) -> None:
    """Mini's DID 0x0208 answers at least 11 bytes; an 8-byte buffer is an OVERFLOW_LOCAL."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    bound = _bound("PRES_Mini_Cell_2Byte", update_interval="60s")
    with pytest.raises(cv.Invalid) as excinfo:
        run_hub_final_validate(config, isotp=[isotp_entry(max_message_size=8)], sensors=[bound])
    message = str(excinfo.value)
    assert "11" in message  # what the group answers with
    assert "8" in message  # what the transport holds
    assert "OVERFLOW_LOCAL" in message  # and what it would look like on the bench


def test_transport_big_enough_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    bound = _bound("PRES_Mini_Cell_2Byte", update_interval="60s")
    assert run_hub_final_validate(config, isotp=[isotp_entry(max_message_size=16)], sensors=[bound]) is config


def test_only_the_groups_the_config_reads_are_measured(set_core_config) -> None:
    """Mini has an 11-byte group; a config that reads none of it needs no 11-byte buffer."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    bound = _bound("PRES_Mini_State", update_interval="5s")
    assert run_hub_final_validate(config, isotp=[isotp_entry(max_message_size=4)], sensors=[bound]) is config


def test_a_group_reached_only_by_an_action_still_counts(set_core_config) -> None:
    """`uds.read` puts the same bytes on the wire as the poller, so it constrains the buffer too."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    with pytest.raises(cv.Invalid, match="11"):
        run_hub_final_validate(
            config,
            isotp=[isotp_entry(max_message_size=8)],
            extra=_automation(_read(config["id"], 0x0208)),
        )


# Real-catalog transport-sizing cases against an actual wide group live in
# private/tests/test_bms_extra.py (docs/CONVENTIONS.md) — they hardcode real
# field names/DIDs, which is the thing this rule exists to keep out of the
# tracked repo.


def test_missing_isotp_config_is_not_an_error(set_core_config) -> None:
    """The id resolved at schema time, so a miss means a shape we do not model — isotp's to report."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI)))
    assert run_hub_final_validate(config, isotp=[]) is config


def test_short_n_cr_timeout_warns_but_builds(set_core_config, caplog) -> None:
    """An ECU that answers 0x78 intends to take longer than P2*, but a slow client is still legal."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), ecu="MINI"))
    with caplog.at_level("WARNING"):
        assert run_hub_final_validate(
            config, isotp=[isotp_entry(n_cr_timeout=TimePeriodMilliseconds(milliseconds=500))]
        ) is config
    assert "P2*" in caplog.text


def test_block_size_mismatch_is_information_only(set_core_config, caplog) -> None:
    """CP_BLOCKSIZE_SUG is a suggestion in the database; a different grant is still conforming."""
    setup_c6(set_core_config)
    config = validate(hub(catalog=catalog(MINI), ecu="MINI"))
    with caplog.at_level("INFO"):
        assert run_hub_final_validate(config, isotp=[isotp_entry(block_size=4)]) is config
    assert "block_size" in caplog.text


# ---------------------------------------------------------- U13: one hub per transport, one size


def test_two_hubs_on_one_isotp_instance_rejected(set_core_config) -> None:
    """IsoTpProtocol::set_consumer() takes exactly one consumer: the second hub would go deaf."""
    setup_c6(set_core_config)
    first = validate(hub(catalog=catalog(MINI)))
    second = validate(hub(id="other", catalog=catalog(MINI)))
    import esphome.final_validate as fv

    from .glue import full_config
    from esphome.components.uds import _final_validate

    token = fv.full_config.set(full_config([first, second]))
    try:
        with pytest.raises(cv.Invalid) as excinfo:
            _final_validate(first)
    finally:
        fv.full_config.reset(token)
    message = str(excinfo.value)
    assert "set_consumer" in message
    assert "never receive a response" in message


def test_two_hubs_on_separate_transports_accepted(set_core_config) -> None:
    setup_c6(set_core_config)
    first = validate(hub(catalog=catalog(MINI)))
    second = validate(hub(id="other", isotp_id="tp2", catalog=catalog(MINI)))
    import esphome.final_validate as fv

    from .glue import full_config
    from esphome.components.uds import _final_validate

    token = fv.full_config.set(
        full_config([first, second], isotp=[isotp_entry(), isotp_entry(id="tp2")])
    )
    try:
        assert _final_validate(first) is first
        assert _final_validate(second) is second
    finally:
        fv.full_config.reset(token)


def test_hubs_may_share_one_catalog_partition(set_core_config) -> None:
    """The bench shape: a real ECU and a responder rig reading one flashed catalog."""
    setup_c6(set_core_config)
    first = validate(hub(catalog=partition_catalog(MINI)))
    second = validate(hub(id="other", isotp_id="tp2", catalog=partition_catalog(MINI)))
    import esphome.final_validate as fv

    from .glue import full_config
    from esphome.components.uds import _final_validate

    token = fv.full_config.set(
        full_config([first, second], isotp=[isotp_entry(), isotp_entry(id="tp2")])
    )
    try:
        assert _final_validate(first) is first
    finally:
        fv.full_config.reset(token)


def test_shared_partition_with_two_sizes_rejected(set_core_config) -> None:
    """One partition has one size, and the table is written once."""
    setup_c6(set_core_config)
    first = validate(hub(catalog=partition_catalog(MINI, size="256KB")))
    second = validate(hub(id="other", isotp_id="tp2", catalog=partition_catalog(MINI, size="128KB")))
    import esphome.final_validate as fv

    from .glue import full_config
    from esphome.components.uds import _final_validate

    token = fv.full_config.set(
        full_config([first, second], isotp=[isotp_entry(), isotp_entry(id="tp2")])
    )
    try:
        with pytest.raises(cv.Invalid, match="One partition has one size"):
            _final_validate(first)
    finally:
        fv.full_config.reset(token)
