"""Provider registry (docs/portal-spec.md section 4.6), PS-M1-04.

In-process cases register providers against the live registry (snapshotted and restored around
each test); the codegen cases load the test-only external component in
``fixtures/ext/portal_demo_providers`` the way a consuming project would.
"""

from __future__ import annotations

from pathlib import Path
import re

import pytest

from esphome import config_validation as cv
from esphome.components import sensor

from .common import (
    FIXTURES,
    NULL,
    generate,
    portal,
    portal_yaml,
    restore_registry,
    series_entry,
    setup_c6,
    snapshot_registry,
    tree_node,
    validate,
    value_entry,
)


@pytest.fixture(autouse=True)
def _core(set_core_config):
    setup_c6(set_core_config)


@pytest.fixture
def registry():
    saved = snapshot_registry()
    yield
    restore_registry(saved)


def _absent(extra=None, **args):
    return tree_node(
        absent="entities_unavailable",
        absent_when={"provider": "p_abs", "source": "vcc_measured_sensor", **args},
        **(extra or {}),
    )


def _register(**kw):
    from esphome.components.portal import register_absent_provider, register_series_provider

    register_absent_provider(
        "p_abs", args={"n": cv.int_range(min=0, max=3)}, source_type=sensor.Sensor, body="return {n} > 1;"
    )
    register_series_provider(
        "p_ser", args={"n": cv.int_range(min=0, max=5)}, source_type=sensor.Sensor, body="return false;"
    )


def test_registry_ships_empty() -> None:
    """PS-M1-04: the portal ships no providers (an unknown provider is rejected, naming it)."""
    with pytest.raises(cv.Invalid) as e:
        validate(portal(tree=[tree_node(absent="x", absent_when={"provider": "csc_sim_slot_silent", "source": "s"})]))
    assert "csc_sim_slot_silent" in str(e.value)


def test_registry_functions_exist_with_the_spec_signature() -> None:
    """PS-M1-04: names and keyword-only parameters of 4.6."""
    import inspect

    from esphome.components import portal as mod

    a = inspect.signature(mod.register_absent_provider)
    s = inspect.signature(mod.register_series_provider)
    for name in ("args", "source_type", "body"):
        assert name in a.parameters and a.parameters[name].kind is inspect.Parameter.KEYWORD_ONLY
        assert name in s.parameters and s.parameters[name].kind is inspect.Parameter.KEYWORD_ONLY
    for name in ("evidence", "for_why"):
        assert name in a.parameters
    assert next(iter(a.parameters)) == "name" and next(iter(s.parameters)) == "name"


def test_registered_absent_provider_validates(registry) -> None:
    """PS-M1-04 accept."""
    _register()
    node = validate(portal(tree=[_absent(n=2)]))["tree"][0]
    assert node["absent_when"]["provider"] == "p_abs" and node["absent_when"]["n"] == 2


def test_registered_series_provider_validates(registry) -> None:
    """PS-M1-04 accept."""
    _register()
    node = validate(portal(tree=[tree_node(series=series_entry(source="vcc_measured_sensor", provider="p_ser", n=1))]))
    assert node["tree"][0]["series"]["provider"] == "p_ser"


def test_unknown_provider_error_lists_registered_names(registry) -> None:
    """PS-M1-04 reject: the error lists the registered names."""
    _register()
    with pytest.raises(cv.Invalid) as e:
        validate(portal(tree=[tree_node(series=series_entry(source="s", provider="nope", n=1))]))
    msg = str(e.value)
    assert "nope" in msg and "p_ser" in msg


def test_absent_provider_missing_arg_is_rejected(registry) -> None:
    """PS-M1-04 reject."""
    _register()
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[_absent()]))


def test_absent_provider_extra_arg_is_rejected(registry) -> None:
    """PS-M1-04 reject."""
    _register()
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[_absent(n=1, m=2)]))


@pytest.mark.parametrize("n", [-1, 4])
def test_absent_provider_arg_out_of_range_is_rejected(registry, n: int) -> None:
    """PS-M1-04 reject: args use the registered validators."""
    _register()
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[_absent(n=n)]))


def test_series_provider_missing_and_extra_args_are_rejected(registry) -> None:
    """PS-M1-04 reject."""
    _register()
    base = dict(source="vcc_measured_sensor", provider="p_ser")
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[tree_node(series=series_entry(**base))]))
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[tree_node(series=series_entry(n=1, z=3, **base))]))


def test_series_args_without_provider_are_rejected(registry) -> None:
    """PS-M1-04 reject: provider, source and args are all-or-nothing."""
    _register()
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[tree_node(series=series_entry(n=1))]))


def test_absent_when_without_source_is_rejected(registry) -> None:
    """PS-M1-04 reject."""
    _register()
    node = tree_node(absent="entities_unavailable", absent_when={"provider": "p_abs", "n": 1})
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[node]))


def test_absent_when_lifts_values_and_children_exclusivity(registry) -> None:
    """PS-M1-04 / 4.1: with absent_when only role and unknown_when stay exclusive."""
    _register()
    node = _absent(
        n=1,
        extra={"values": [value_entry()], "children": [tree_node(segment="kid")], "series": series_entry()},
    )
    validate(portal(tree=[node]))


def test_absent_when_with_role_stays_exclusive(registry) -> None:
    """PS-M1-04 reject."""
    _register()
    node = _absent(n=1, extra={"role": "r", "roles_available": ["r"]})
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[node]))


def test_evidence_gated_code_needs_a_provider_with_matching_evidence(registry) -> None:
    """PS-M1-04 / 4.1: absent: never_announced is legal only with a provider declaring evidence for exactly that code."""
    from esphome.components.portal import register_absent_provider

    register_absent_provider(
        "p_ev",
        args={},
        source_type=sensor.Sensor,
        body="return false;",
        evidence='"announced":false',
        for_why=("never_announced",),
    )
    ok = tree_node(
        absent="never_announced", absent_when={"provider": "p_ev", "source": "vcc_measured_sensor"}
    )
    validate(portal(tree=[ok]))
    # another gated code with the same provider: refused
    bad = tree_node(absent="crc_fail", absent_when={"provider": "p_ev", "source": "vcc_measured_sensor"})
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[bad]))
    # a provider that declares no evidence at all: refused
    register = __import__("esphome.components.portal", fromlist=["x"]).register_absent_provider
    register("p_noev", args={}, source_type=sensor.Sensor, body="return false;")
    noev = tree_node(absent="never_announced", absent_when={"provider": "p_noev", "source": "vcc_measured_sensor"})
    with pytest.raises(cv.Invalid):
        validate(portal(tree=[noev]))


# --- codegen: one deduplicated, deterministically named thunk per (provider, args) -----

EXT = f"  - source:\n      type: local\n      path: {FIXTURES.as_posix()}/ext\n"


def _prov_tree():
    src = "vcc_measured_sensor"

    def a(seg, n):
        return tree_node(
            segment=seg, absent="entities_unavailable", absent_when={"provider": "demo_absent", "source": src, "n": n}
        )

    def s(seg, n):
        return tree_node(segment=seg, series=series_entry(source=src, provider="demo_series", n=n))

    return [
        a("a1", 2),
        a("a2", 2),
        a("a3", 3),
        tree_node(
            segment="ok",
            absent="never_announced",
            absent_when={"provider": "demo_absent", "source": src, "n": 1},
        ),
        tree_node(segment="p", absent="entities_unavailable", absent_when={"provider": "demo_absent_plain", "source": src}),
        s("s1", 1),
        s("s2", 1),
        s("s3", 4),
    ]


def test_codegen_emits_one_deduplicated_named_thunk_per_provider_and_args(tmp_path: Path) -> None:
    """PS-M1-04: portal_prov_<provider>_<argletter><value>..., once per distinct (provider, args)."""
    text = portal_yaml(portal(tree=_prov_tree()), extra="portal_demo_providers:\n", extra_components=EXT)
    gen = generate(tmp_path, text)
    assert gen.returncode == 0, gen.output
    cpp = gen.main_cpp
    names = {
        "portal_prov_demo_absent_n2": 1,
        "portal_prov_demo_absent_n3": 1,
        "portal_prov_demo_absent_n1": 1,
        "portal_prov_demo_series_n1": 1,
        "portal_prov_demo_series_n4": 1,
    }
    for name, count in names.items():
        defs = re.findall(rf"\b{name}\s*\([^;{{]*\)\s*{{", cpp)
        assert len(defs) == count, f"{name}: {len(defs)} definitions\n{gen.output}"
    assert len(re.findall(r"\bportal_prov_demo_absent_plain\b", cpp)) >= 1
    # the body template arguments were substituted
    assert "return 2 > 1;" in cpp and "return 3 > 1;" in cpp


def test_codegen_is_deterministic(tmp_path: Path) -> None:
    """PS-M1-04: thunk names never depend on a counter or iteration order."""
    text = portal_yaml(portal(tree=_prov_tree()), extra="portal_demo_providers:\n", extra_components=EXT)
    a = tmp_path / "a"
    b = tmp_path / "b"
    a.mkdir()
    b.mkdir()
    ga, gb = generate(a, text), generate(b, text)
    assert ga.returncode == 0 and gb.returncode == 0, ga.output + gb.output
    names = lambda g: sorted(set(re.findall(r"\bportal_prov_\w+", g.main_cpp)))  # noqa: E731
    assert names(ga) == names(gb)


def test_registry_leaves_no_provider_behind_without_the_ext_component(tmp_path: Path) -> None:
    """PS-M1-04: without the test ext component in the config the same tree is rejected (registry empty by default)."""
    text = portal_yaml(portal(tree=_prov_tree()))
    gen = generate(tmp_path, text)
    assert gen.returncode != 0
    assert "demo_absent" in gen.output
