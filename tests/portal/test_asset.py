"""Static checks on the component sources and the built-in page.

PS-M0-01 (spec neutral), PS-M1-01 (layout + core includes), PS-M1-11 (page size/neutrality/KINDS),
PS-M2-01 (no own httpd), PS-M3-08 (page renders every control, sends JSON via fetch), PS-M3-09
(grep-level part: no entity call from the handler), PS-G-02 (nothing private in our own files).
"""

from __future__ import annotations

import gzip
from pathlib import Path
import re
import shutil
import subprocess

import pytest

from .common import COMPONENT, REPO

SPEC = REPO / "docs" / "portal-spec.md"
PAGE = COMPONENT / "index.html"
NEUTRAL_KINDS = ["vehicle", "component", "bus", "device", "ecu", "charger", "battery", "logger", "gateway",
                 "network", "sensor", "actuator", "module"]
def _repo_patterns() -> list[str]:
    """The repo-wide private-data patterns, imported (never restated) from the guard script."""
    import importlib.util

    spec = importlib.util.spec_from_file_location("check_no_private_data", REPO / "script" / "check_no_private_data.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return [p.pattern for p, _reason in mod.PATTERNS]


# Extra markers for the portal files: bench board names, LAN addresses, absolute home paths, bench MACs.
_EXTRA = [r"\bmr-(blue|orange|yellow|purple)\b", r"192\.168\.\d+\.\d+(?<!192\.168\.4\.1)", r"/Users/", r"AC:EB:E6", r"20:6E:F1"]
PRIVATE = _repo_patterns() + _EXTRA


def _all_component_files():
    return [p for p in COMPONENT.rglob("*") if p.is_file() and "__pycache__" not in p.parts]


def test_component_layout_exists() -> None:
    """PS-M1-01: the five files of section 3."""
    for name in ("__init__.py", "portal.h", "portal.cpp", "portal_core.h", "index.html"):
        assert (COMPONENT / name).is_file(), name


def test_portal_core_includes_only_the_freestanding_headers() -> None:
    """PS-M1-01: portal_core.h includes only <cstddef>, <cstdint>, <cstring>."""
    text = (COMPONENT / "portal_core.h").read_text()
    includes = re.findall(r"^\s*#\s*include\s*([<\"][^>\"]+[>\"])", text, re.M)
    assert sorted(set(includes)) == ["<cstddef>", "<cstdint>", "<cstring>"], includes


def test_portal_core_allocates_nothing() -> None:
    """PS-M1-01 / section 3: no allocation in the core."""
    text = re.sub(r"//.*", "", (COMPONENT / "portal_core.h").read_text())
    for banned in (r"\bnew\b", r"\bmalloc\b", r"\bcalloc\b", r"\brealloc\b", r"std::string", r"std::vector", r"std::map"):
        assert not re.search(banned, text), banned


def test_no_own_httpd_in_the_component() -> None:
    """PS-M2-01: the portal registers on web_server_base; it never starts an httpd."""
    for f in _all_component_files():
        if f.suffix in (".h", ".cpp", ".py"):
            code = re.sub(r"//.*|#.*" if f.suffix == ".py" else r"//.*", "", f.read_text())
            assert "httpd_start" not in code, f.name


def test_component_registers_via_web_server_base() -> None:
    """PS-M2-01: init() + add_handler_without_auth on web_server_base."""
    src = (COMPONENT / "portal.cpp").read_text() + (COMPONENT / "portal.h").read_text()
    assert "add_handler" in src, "no handler registration found"
    assert "web_server_base" in src.lower()


def test_entity_calls_are_not_made_from_the_request_handler() -> None:
    """PS-M3-09 (grep level): turn_on/turn_off/press/make_call only appear in loop()-side code.

    Heuristic: inside handleRequest/handleBody bodies none of the entity verbs may appear.
    """
    cpp = (COMPONENT / "portal.cpp").read_text()
    for fn in ("handleRequest", "handleBody", "canHandle"):
        for m in re.finditer(rf"\b{fn}\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?\{{", cpp):
            depth, i = 1, m.end()
            while depth and i < len(cpp):
                depth += {"{": 1, "}": -1}.get(cpp[i], 0)
                i += 1
            body = cpp[m.end(): i]
            for verb in (".turn_on(", "->turn_on(", ".turn_off(", "->turn_off(", ".press(", "->press(", "make_call("):
                assert verb not in body, f"{verb} inside {fn}"


# --- built-in page -----------------------------------------------------------------------


def test_page_fits_64_kib_gzipped() -> None:
    """PS-M1-11: <= 64 KiB as embedded (gzip -9, mtime 0)."""
    packed = gzip.compress(PAGE.read_bytes(), compresslevel=9, mtime=0)
    assert len(packed) <= 64 * 1024, len(packed)


def test_page_has_no_external_resource() -> None:
    """PS-M1-11: no http(s):// resource reference (src/href/url/import/fetch)."""
    text = PAGE.read_text()
    assert not re.search(r"""(?:src|href|action)\s*=\s*["']?\s*(?:https?:)?//""", text)
    assert not re.search(r"""url\(\s*["']?\s*(?:https?:)?//""", text)
    assert not re.search(r"""@import\s+["']?(?:https?:)?//""", text)
    assert not re.search(r"""(?:fetch|import|XMLHttpRequest)[^;\n]*["']https?://""", text)
    assert "http://" not in text and "https://" not in text, "no absolute URL at all in the page"


def test_page_has_no_private_marker() -> None:
    """PS-M1-11 / PS-G-02: no vehicle, board, MAC, address or private marker."""
    text = PAGE.read_text()
    for pattern in PRIVATE:
        m = re.search(pattern, text, re.I)
        assert not m, f"{pattern!r} found: {m.group(0)!r}"


def test_page_kinds_contain_the_neutral_nouns() -> None:
    """PS-M1-11: KINDS includes the section 7 nouns."""
    text = PAGE.read_text()
    m = re.search(r"\bKINDS\b\s*=\s*(\{.*?\}|\[.*?\])\s*;", text, re.S)
    assert m, "KINDS definition not found"
    for noun in NEUTRAL_KINDS:
        assert re.search(rf"""["']?\b{noun}\b["']?\s*[:,\]\}}]""", m.group(1)), noun


def test_page_names_unknown_kinds_generically() -> None:
    """PS-M1-11 / 7: an unknown kind renders generically and is named (behavioural hint in the source)."""
    text = PAGE.read_text()
    assert re.search(r"KINDS\s*\[", text) or re.search(r"KINDS\.", text) or re.search(r"in\s+KINDS", text)


def test_page_renders_each_control() -> None:
    """PS-M3-08: toggle, button, dropdown, number input + set button with min/max/step, result chip."""
    t = PAGE.read_text()
    assert re.search(r"""["']switch["']""", t) and re.search(r"""["']button["']""", t)
    assert re.search(r"""["']select["']""", t) and re.search(r"""["']number["']""", t)
    assert re.search(r"""type\s*=\s*["']?checkbox|role\s*=\s*["']switch|createElement\(\s*["']select|\.type\s*=\s*["']checkbox""", t), "toggle"
    assert re.search(r"""createElement\(\s*["']select["']|<select""", t), "dropdown"
    assert re.search(r"""type\s*=\s*["']?number|\.type\s*=\s*["']number""", t), "number input"
    for attr in ("min", "max", "step"):
        assert re.search(rf"""\.{attr}\b|\b{attr}\s*=""", t), attr
    assert re.search(r"result", t) and re.search(r"age_ms", t), "result chip"
    assert re.search(r"entity_unavailable|\.why\b", t), "disabled actions show their reason"
    assert re.search(r"unknown_control|unrecognis|unrecogniz", t, re.I), "unknown control shown disabled"


def test_page_sends_json_via_fetch_post() -> None:
    """PS-M3-08: requests use fetch with JSON bodies; a 401 leaves login to the browser."""
    t = PAGE.read_text()
    assert "fetch(" in t
    assert re.search(r"""method\s*:\s*["']POST["']|\.method""", t)
    assert "application/json" in t
    assert "JSON.stringify" in t
    assert not re.search(r"""Authorization|btoa\(""", t), "the page must not handle credentials itself"


def test_page_script_parses_with_node() -> None:
    """PS-M3-08 (optional smoke): the inline script is valid JavaScript; skipped without node."""
    node = shutil.which("node")
    if not node:
        pytest.skip("node not installed")
    text = PAGE.read_text()
    scripts = re.findall(r"<script[^>]*>(.*?)</script>", text, re.S)
    assert scripts
    for i, js in enumerate(scripts):
        tmp = Path(__file__).parent / f".tmp_page_{i}.js"
        tmp.write_text(js)
        try:
            r = subprocess.run([node, "--check", str(tmp)], capture_output=True, text=True)
        finally:
            tmp.unlink(missing_ok=True)
        assert r.returncode == 0, r.stderr


# --- spec and privacy ----------------------------------------------------------------------


def test_spec_exists_and_is_neutral() -> None:
    """PS-M0-01: the spec file exists and carries no private marker (the repo-wide guard also runs it)."""
    text = SPEC.read_text()
    for pattern in PRIVATE:
        assert not re.search(pattern, text, re.I), pattern
    r = subprocess.run(["python3", str(REPO / "script" / "check_no_private_data.py")], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


def test_our_own_files_carry_nothing_private() -> None:
    """PS-G-02: tests/portal, tests/build/portal, script/portal_schema.py, tests/host/*portal*."""
    roots = [Path(__file__).parent, REPO / "tests" / "build" / "portal"]
    files = [p for r in roots if r.exists() for p in r.rglob("*") if p.is_file() and ".esphome" not in p.parts and "__pycache__" not in p.parts and p.suffix in
             (".py", ".yaml", ".json", ".md", ".html", ".cpp", ".h")]
    files += [REPO / "script" / "portal_schema.py"] + list((REPO / "tests" / "host").glob("*portal*"))
    me = Path(__file__).resolve()
    for f in files:
        if f.resolve() == me:
            continue
        text = f.read_text()
        for pattern in PRIVATE:
            m = re.search(pattern, text, re.I)
            assert not m, f"{f}: {pattern!r} -> {m.group(0)!r}"
        for mac in re.findall(r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])", text):
            assert mac.upper().startswith("02:00:00:00:00:"), (f, mac)


def test_every_portal_build_yaml_is_in_the_ci_compile_matrix() -> None:
    """PS-G-03."""
    ci = (REPO / ".github" / "workflows" / "ci.yml").read_text()
    yamls = sorted((REPO / "tests" / "build" / "portal").glob("test*.yaml"))
    assert len(yamls) >= 3
    for y in yamls:
        rel = y.relative_to(REPO).as_posix()
        assert f"- {rel}" in ci, f"{rel} missing from the compile matrix"


def test_build_yamls_transmit_on_no_bus() -> None:
    """PS-M4-01 / TX safety: the build configs contain no bus component at all."""
    for y in (REPO / "tests" / "build" / "portal").glob("*.yaml"):
        text = y.read_text()
        for banned in ("can_gateway:", "isotp:", "uds:", "linbus:", "ccp:", "canbus:", "uart:", "modbus:", "lin:"):
            assert not re.search(rf"^{banned}", text, re.M), (y.name, banned)


def test_page_shows_envelope_links_and_never_asks_for_get_credentials() -> None:
    """PS-M3-08 (v1.1): `links` entries are rendered in the header; no credential handling for GET."""
    t = PAGE.read_text()
    assert re.search(r"\blinks\b", t) and re.search(r"\.href|href\s*=", t)
    assert not re.search(r"""WWW-Authenticate|Authorization|prompt\(""", t)
