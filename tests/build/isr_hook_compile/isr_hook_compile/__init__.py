import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.components import can_gateway

DEPENDENCIES = ["can_gateway"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required("port"): cv.use_id(can_gateway.GatewayPort),
    }
)


async def to_code(config):
    # This test-only consumer owns the opt-in define, exactly as a real
    # consumer must. can_gateway itself never emits it.
    cg.add_define("USE_CAN_GATEWAY_ISR_HOOK")
    port = await cg.get_variable(config["port"])
    cg.add(port.set_isr_frame_hook(cg.RawExpression("nullptr"), cg.RawExpression("nullptr")))
