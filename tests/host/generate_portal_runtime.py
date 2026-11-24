"""Extract the complete portal binding, verbatim except hardware includes, for host glue."""

from pathlib import Path

HERE = Path(__file__).parent
PORTAL = HERE.parent.parent / "components" / "portal"
parts = ['#pragma once\n#include "portal_stubs.h"\n#include "portal_core.h"\n']
parts.extend(
    "\n".join(
        line
        for line in (PORTAL / name).read_text().splitlines()
        if not line.startswith("#include ") and line != "#pragma once"
    )
    for name in ("portal.h", "portal.cpp")
)
(HERE / "build" / "portal_runtime.h").write_text("\n".join(parts) + "\n")
