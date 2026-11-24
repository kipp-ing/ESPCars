"""Compile the real transport glue with host substitutes for ESPHome and CAN IO.

Like RulePatch's extraction in Makefile, only platform includes/actions are removed;
the hub declarations and every runtime method are copied verbatim, never reimplemented.
"""

from pathlib import Path
import re


def generate() -> None:
    root = Path(__file__).resolve().parents[2]
    output = root / "tests/host/build/transport_runtime.h"
    parts = [
        '#include "transport_stubs.h"',
        '#include "ccp_proto.h"',
        '#include "isotp_core.h"',
    ]
    for name, marker in (
        ("ccp", "template<typename... Ts> class CcpConnectAction"),
        ("isotp", "#ifdef USE_SENSOR\n/// Optional diagnostic"),
    ):
        folder = root / "components" / name
        header = (folder / f"{name}.h").read_text().split(marker)[0]
        source = (folder / f"{name}.cpp").read_text()
        parts.extend(
            re.sub(r'^#include "[^"\n]+"\n', "", text, flags=re.MULTILINE)
            for text in (header + f"\n}}  // namespace esphome::{name}\n", source)
        )
    output.write_text("\n".join(parts))


if __name__ == "__main__":
    generate()
