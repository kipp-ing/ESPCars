"""CCP receive registration must execute before App.setup freezes gateway IDs."""

import esphome.codegen as cg
from esphome.core import CORE, ID, Lambda

from .common import hub, setup_c6, validate


def test_response_subscription_emitted_before_setup(set_core_config) -> None:
    from esphome.components.ccp import to_code

    setup_c6(set_core_config)
    config = validate(hub())
    CORE.register_variable(config["can_gateway_id"], cg.MockObj("diag_bus", "->"))
    CORE.component_ids.add(str(config["id"]))
    CORE.add_job(to_code, config)
    CORE.flush_tasks()
    generated = "\n".join(str(statement) for statement in CORE.main_statements)
    assert "bms_ccp->register_receive_id(0x701);" in generated


def test_static_daq_action_subscribes_dto_id_before_setup(set_core_config) -> None:
    from esphome import automation
    from esphome.components.ccp import CcpDaqReadAction, ccp_daq_read, to_code

    setup_c6(set_core_config)
    config = validate(hub())
    CORE.register_variable(config["can_gateway_id"], cg.MockObj("diag_bus", "->"))
    CORE.component_ids.add(str(config["id"]))
    CORE.add_job(to_code, config)
    action = automation.ACTION_REGISTRY["ccp.daq_read"].schema(
        {"id": "bms_ccp", "fields": [{"address": 0x1000, "size": 1}]}
    )
    CORE.add_job(
        ccp_daq_read,
        action,
        ID("daq_action", is_declaration=True, type=CcpDaqReadAction),
        cg.TemplateArguments(),
        [],
    )
    CORE.flush_tasks()
    generated = "\n".join(str(statement) for statement in CORE.main_statements)
    assert "bms_ccp->register_receive_id(0x702);" in generated


def test_dynamic_daq_uses_only_predeclared_receive_ids(set_core_config) -> None:
    from esphome import automation
    from esphome.components.ccp import CcpDaqReadAction, ccp_daq_read, to_code

    setup_c6(set_core_config)
    config = validate(hub(daq_ids=[0x704]))
    CORE.register_variable(config["can_gateway_id"], cg.MockObj("diag_bus", "->"))
    CORE.component_ids.add(str(config["id"]))
    CORE.add_job(to_code, config)
    action = automation.ACTION_REGISTRY["ccp.daq_read"].schema(
        {
            "id": "bms_ccp",
            "dto_id": Lambda("return 0x704;"),
            "fields": [{"address": 0x1000, "size": 1}],
        }
    )
    CORE.add_job(
        ccp_daq_read,
        action,
        ID("daq_action", is_declaration=True, type=CcpDaqReadAction),
        cg.TemplateArguments(),
        [],
    )
    CORE.flush_tasks()
    generated = "\n".join(str(statement) for statement in CORE.main_statements)
    assert generated.count("register_receive_id(0x704);") == 1
    assert generated.count("register_receive_id(") == 2  # response plus declared DAQ ID
