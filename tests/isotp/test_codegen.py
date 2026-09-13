"""Codegen: every instance stamps its YAML id into its log lines.

`isotp:` is MULTI_CONF and the bench tester runs two instances as a matter of
course, so a bare `[W][isotp]: transfer failed` is attributed to whichever
instance the reader already suspects — the trap that cost the uds side a whole
diagnosis of a bug that was never a bug (HANDOVER-uds.md §6c). The C++ default
for the name is the component tag, which only codegen can improve on: the YAML
id exists nowhere else. This pins that `set_log_name("<id>")` is emitted for
each instance, not just the first.
"""

from __future__ import annotations

import esphome.codegen as cg
from esphome.core import CORE

from .common import PORT_ID, isotp, setup_c6, validate


def _generate(*configs) -> str:
    """Run the component's to_code for each validated instance and return main.cpp."""
    from esphome.components.isotp import to_code

    for config in configs:
        # register_component checks the id against the full-config component scan,
        # which calling to_code directly bypasses.
        CORE.component_ids.add(str(config["id"]))
        CORE.add_job(to_code, config)
    CORE.flush_tasks()
    return "\n".join(str(statement) for statement in CORE.main_statements)


def test_codegen_names_each_instance(set_core_config) -> None:
    setup_c6(set_core_config)
    bms = validate(isotp(id="bms_isotp"))
    charger = validate(isotp(id="charger_isotp", tx_id=0x7E1, rx_id=0x7E9))
    # Stand in for the port variable the can_gateway block would have registered;
    # one port serves both instances, as on the bench tester.
    CORE.register_variable(bms["port_id"], cg.MockObj(PORT_ID, "->"))
    generated = _generate(bms, charger)
    assert 'bms_isotp->set_log_name("bms_isotp");' in generated
    assert 'charger_isotp->set_log_name("charger_isotp");' in generated
