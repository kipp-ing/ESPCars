"""Shared plumbing for the uds catalog test suite.

The catalog library is loaded **by file location** rather than as
`esphome.components.uds.catalog`, and stays that way now that the ESPHome
component exists beside it: importing the package pulls in `__init__.py`, which
imports `esphome.components.isotp` and therefore needs the whole ESPHome
component loader to be initialised. The catalog tests are about bytes, not about
codegen, and should not need a configured core to run.

The codegen suites (`test_schema.py`, `test_bindings.py`, `test_gates.py`) do the
opposite: they import `esphome.components.uds` through the path hook in
`tests/conftest.py`, after setting up a core.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURES = Path(__file__).resolve().parent / "fixtures"
CATALOGS = REPO_ROOT / "catalogs"

# Real, bench-specific diagnostic data (a compiled factory catalog, and tables
# reverse-engineered off real hardware) never goes in this repo — see
# docs/CONVENTIONS.md. It lives in the consuming project's own repository,
# never here; `private/` below is only an optional local drop-in, absent on a
# fresh clone and in CI, so tests touching it must skip, not fail. The checked-in `mini` fixture is the artifact of record for
# everything that runs everywhere.
PRIVATE = REPO_ROOT / "private"
PRIVATE_CATALOGS = PRIVATE / "catalogs"
PRIVATE_FIXTURES = PRIVATE / "fixtures"
BMS_JSON = PRIVATE / "external" / "BMS.json"
KWP_BMS_MD = PRIVATE / "external" / "kwp-bms.md"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses resolves types via sys.modules
    spec.loader.exec_module(module)
    return module


def load_catalog_module():
    """The shared reader/writer, components/uds/catalog.py."""
    name = "uds_catalog_lib"
    if name in sys.modules:
        return sys.modules[name]
    return _load(name, REPO_ROOT / "components" / "uds" / "catalog.py")


def load_cli_module():
    """The CLI, script/uds_catalog.py (it self-loads the shared module)."""
    name = "uds_catalog_cli"
    if name in sys.modules:
        return sys.modules[name]
    return _load(name, REPO_ROOT / "script" / "uds_catalog.py")


def tiny_catalog(catalog):
    """A hand-built four-group catalog exercising every record type.

    Field names are unique within each group (§5.1a), the way a compiled
    catalog must be — but `PRES_Volt_2Byte` deliberately appears in *two*
    groups, because cross-group ambiguity is real in a factory database (an
    identity field can legitimately live under two different DIDs) and
    `service:` is what disambiguates it. The cell array covers
    `repeat_count`/`repeat_stride`.
    """
    volt = (
        catalog.Scale(0, 65534, 0.001, 1.5),
        catalog.Scale(65535, 65535, 0.0, 0.0, "Signal not available", True),
    )
    state = (
        catalog.Scale(0, 0, 0.0, 0.0, "open"),
        catalog.Scale(2, 2, 0.0, 0.0, "closed"),
        catalog.Scale(255, 255, 0.0, 0.0, "SNA", True),
    )
    g1 = catalog.Group(
        name="DT_Volt",
        request=bytes([0x22, 0x02, 0x07]),
        fields=(
            catalog.Field("PRES_Volt_2Byte", 24, 16, unit="V", scales=volt),
            catalog.Field("DT_Volt_Minimum", 40, 16, unit="V", scales=volt),
            catalog.Field("PRES_Volt_Avg_2Byte", 56, 16, unit="V", scales=volt),
        ),
        aliases=("DT_Volt_Maximum", "DT_Volt_Minimum", "DT_Volt_Averaged"),
    )
    g2 = catalog.Group(
        name="DT_State",
        request=bytes([0x22, 0xD0, 0x00]),
        fields=(catalog.Field("PRES_State", 24, 8, scales=state),),
    )
    g3 = catalog.Group(
        name="RT_Job_Start",
        request=bytes([0x31, 0x01, 0x03, 0x00]),
        fields=(catalog.Field("PRES_Result", 32, 8),),
        needs_session=True,
        needs_security=True,
        security_level=3,
    )
    g4 = catalog.Group(
        name="DT_Cells",
        request=bytes([0x22, 0x02, 0x08]),
        fields=(
            catalog.Field(
                "PRES_Cell_2Byte", 24, 16, unit="V", scales=volt,
                repeat_count=4, repeat_stride=16,
            ),
            catalog.Field("PRES_Volt_2Byte", 88, 16, unit="V", scales=volt),
        ),
    )
    ecu = catalog.Ecu(
        name="BMS",
        request_id=0x7E7,
        response_id=0x7EF,
        block_size=8,
        st_min_raw=20,
        p2_ms=150,
        p2_ext_ms=2000,
        groups=(g1, g2, g3, g4),
    )
    return catalog.Catalog(ecus=(ecu,))
