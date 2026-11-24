"""Compile the registered portal handler against the installed ESPHome origin policy.

Only transport, auth and logging are faked. Policy functions, middleware declarations,
the portal wrapper and its registration are extracted verbatim, as in the host RulePatch test.
"""

from pathlib import Path
import subprocess

import pytest

import esphome

from .common import COMPONENT, REPO


def _block(text: str, start: str) -> str:
    begin = text.index(start)
    brace = text.index("{", begin)
    depth, end = 1, brace + 1
    while depth:
        depth += {"{": 1, "}": -1}.get(text[end], 0)
        end += 1
    return text[begin:end]


@pytest.fixture(scope="module")
def origin_probe(tmp_path_factory):
    dest = tmp_path_factory.mktemp("portal-origin")
    upstream = Path(esphome.__file__).parent
    base = upstream / "components" / "web_server_base"
    cpp = (base / "web_server_base.cpp").read_text()
    hdr = (base / "web_server_base.h").read_text()
    portal = (COMPONENT / "portal.cpp").read_text()
    guard = ""
    if "class PortalOriginGuard" in portal:
        guard = _block(portal, "class PortalOriginGuard") + ";"
    setup = _block(portal, "void Portal::setup()")
    registration = setup[setup.index("this->base_->init();") : -1]
    generated = (
        (upstream / "core" / "string_ref.h")
        .read_text()
        .replace('#include "esphome/core/defines.h"', "")
        + "\n#define USE_ESP32 1\n"
        + "namespace esphome::web_server_base {\n"
        + (base / "origin.h")
        .read_text()
        .split("namespace esphome::web_server_base {")[1]
        + "namespace esphome::web_server_base {\n"
        + _block((base / "origin.cpp").read_text(), "bool is_origin_allowed(")
        + "\n#if defined(USE_WEBSERVER_ALLOWED_ORIGINS) || !defined(USE_WEBSERVER_ALLOW_ANY_ORIGIN)\n"
        + _block(
            cpp, "static bool __attribute__((noinline)) is_request_origin_allowed("
        )
        + "\n#endif\n"
        + "namespace internal {\n"
        + _block(hdr, "class MiddlewareHandler :")
        + ";\n"
        + _block(hdr, "class GuardMiddlewareHandler :")
        + ";\n"
        + "bool GuardMiddlewareHandler::check_auth_(AsyncWebServerRequest *) { return true; }\n"
        + "\n".join(
            _block(cpp, f"{ret} GuardMiddlewareHandler::{name}(")
            for ret, name in (
                ("bool", "check_origin_"),
                ("bool", "check_chunk_"),
                ("void", "handleRequest"),
                ("void", "handleBody"),
                ("void", "handleUpload"),
            )
        )
        + "\n}}\nnamespace esphome::portal {\n"
        + guard
        + "\nclass Portal : public AsyncWebHandler { public:\n"
        + "web_server_base::WebServerBase *base_;\nvoid setup() {"
        + registration
        + "}\n};\n}\n"
    )
    (dest / "portal_origin_extracted.h").write_text(generated)
    for policy in ("default", "allowlist", "any"):
        binary = dest / policy
        defines = {
            "default": [],
            "allowlist": ["-DUSE_WEBSERVER_ALLOWED_ORIGINS"],
            "any": ["-DUSE_WEBSERVER_ALLOW_ANY_ORIGIN"],
        }[policy]
        command = [
            "c++",
            "-std=c++17",
            "-Wall",
            "-Werror",
            *defines,
            "-I",
            str(dest),
            "-I",
            str(upstream.parent),
            str(REPO / "tests" / "host" / "portal_origin_probe.cpp"),
            "-o",
            str(binary),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        assert result.returncode == 0, result.stderr
    return dest


@pytest.mark.parametrize("policy", ["default", "allowlist", "any"])
def test_portal_origin_policy_before_dispatch_and_body(
    origin_probe, policy: str
) -> None:
    result = subprocess.run(
        [str(origin_probe / policy)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stdout + result.stderr
