"""Extract only repository-owned runtime code, following the RulePatch pattern."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "tests/host/build"
HEADER = (ROOT / "components/can_gateway/can_gateway.h").read_text()
SOURCE = (ROOT / "components/can_gateway/can_gateway.cpp").read_text()


def extract(text: str, start: str, end: str) -> str:
    """Fail loudly if a block's unique opening or closing marker has drifted."""
    if text.count(start) != 1:
        raise ValueError(f"Expected one production block starting with {start!r}")
    begin = text.index(start)
    closing = re.search(end, text[begin:], re.MULTILINE)
    if closing is None:
        raise ValueError(f"Unclosed production block: {start!r}")
    return text[begin : begin + closing.end()] + "\n"


for filename, marker in (
    ("gateway_switch.inc", "class CanGatewaySwitch :"),
    ("gateway_frame_data.inc", "struct CanGatewayFrameData {"),
):
    (BUILD / filename).write_text(extract(HEADER, marker, r"^};"))

for name, signature in (
    ("rx_taps_active_", "bool rx_taps_active_() const {"),
    ("run_rx_taps_", "void run_rx_taps_(uint32_t can_id,"),
):
    marker = f"  CAN_GATEWAY_CORE_INLINE {signature}"
    (BUILD / f"gateway_{name}.inc").write_text(extract(HEADER, marker, r"^  }"))

(BUILD / "gateway_route_less.inc").write_text(
    extract(
        SOURCE,
        "  GatewayRoute *route = this->route_out_;",
        r"(?=^  // These three gates)",
    )
)

for name, marker in (
    ("port_start", "bool GatewayPort::start_("),
    ("offload", "void GatewayPort::try_hw_filter_offload_("),
    ("inject", "bool GatewayPort::inject("),
    ("port_loop", "void GatewayPort::loop_("),
    ("setup", "void CanGateway::setup()"),
    ("decode_sensor", "void CanGatewayDecodeSensor::on_frame("),
    ("decode_binary", "void CanGatewayDecodeBinarySensor::on_frame("),
    ("decode_text", "void CanGatewayDecodeTextSensor::on_frame("),
):
    (BUILD / f"gateway_{name}.inc").write_text(extract(SOURCE, marker, r"^}"))

for name in (
    "CanGatewayDecodeSensor",
    "CanGatewayDecodeBinarySensor",
    "CanGatewayDecodeTextSensor",
):
    (BUILD / f"gateway_{name}.inc").write_text(
        extract(HEADER, f"class {name} :", r"^};")
    )
