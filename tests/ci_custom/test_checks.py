"""Every ported upstream check has a positive and negative example."""

from pathlib import Path
import re

import pytest

from script import ci_custom

CPP = "components/demo/demo.cpp"
HEADER = "components/demo/demo.h"
PYTHON = "components/demo/__init__.py"
CASES = [
    ("ino", "components/demo/demo.ino", "void setup() {}\n", CPP, "void setup() {}\n"),
    ("ext-check", "components/demo/demo.unknown", "hello\n", CPP, "hello\n"),
    ("tabs", CPP, "\tint x;\n", CPP, "  int x;\n"),
    ("newline", CPP, "int x;\r\n", CPP, "int x;\n"),
    ("end-newline", CPP, "int x;", CPP, "int x;\n"),
    (
        "no-defines",
        HEADER,
        "#define COUNT 5\n",
        HEADER,
        "static constexpr int COUNT = 5;\n",
    ),
    ("no-long-delays", CPP, "  delay(50);\n", CPP, "  delay(49);\n"),
    (
        "esp-log-needs-braces",
        CPP,
        'if (ok) ESP_LOGD(TAG, "ok");\n',
        CPP,
        'if (ok) { ESP_LOGD(TAG, "ok"); }\n',
    ),
    ("conf-matches", PYTHON, 'CONF_FOO = "bar"\n', PYTHON, 'CONF_FOO = "foo"\n'),
    (
        "constant-is-plain-string",
        PYTHON,
        'CONF_FOO = "f" + "oo"\n',
        PYTHON,
        'CONF_FOO = "foo"\n',
    ),
    (
        "no-raw-pin-access",
        CPP,
        " digitalWrite(pin->get_pin(), true);\n",
        CPP,
        " pin->digital_write(true);\n",
    ),
    (
        "no-arduino-framework-functions",
        CPP,
        " digitalWrite(1, true);\n",
        CPP,
        " pin->digital_write(true);\n",
    ),
    (
        "no-removed-in-idf-conversions",
        CPP,
        "#ifdef ARDUINO_ARCH_ESP32\n",
        CPP,
        "#ifdef USE_ESP32\n",
    ),
    ("no-byte-datatype", CPP, " byte x = 0;\n", CPP, " uint8_t x = 0;\n"),
    ("no-std-string-view", CPP, "std::string_view name;\n", CPP, "StringRef name;\n"),
    ("component-domain", PYTHON, 'DOMAIN = "other"\n', PYTHON, 'DOMAIN = "demo"\n'),
    (
        "relative-cpp-import",
        CPP,
        '#include "esphome/components/demo/demo.h"\n',
        CPP,
        '#include "demo.h"\n',
    ),
    (
        "relative-py-import",
        PYTHON,
        "from esphome.components.demo import helper\n",
        PYTHON,
        "from . import helper\n",
    ),
    ("namespace", CPP, "namespace other {}\n", CPP, "namespace esphome::demo {}\n"),
    (
        "esphome-h",
        HEADER,
        '#include "esphome.h"\n',
        HEADER,
        '#include "esphome/core/component.h"\n',
    ),
    ("pragma-once", HEADER, "int x;\n", HEADER, "#pragma once\n"),
    (
        "inclusive-language",
        PYTHON,
        'CONF_SLAVE = "slave"\n',
        PYTHON,
        'CONF_DEVICE = "device"\n',
    ),
    ("trailing-whitespace", CPP, "int x; \n", CPP, "int x;\n"),
    (
        "no-heap-allocating-helpers",
        CPP,
        " auto s = format_hex(data);\n",
        CPP,
        " format_hex_to(buf, data);\n",
    ),
    (
        "no-sprintf",
        CPP,
        ' sprintf(buf, "%d", x);\n',
        CPP,
        ' snprintf(buf, sizeof(buf), "%d", x);\n',
    ),
    (
        "no-std-to-string",
        CPP,
        " auto s = std::to_string(x);\n",
        CPP,
        " uint32_to_str(buf, x);\n",
    ),
    (
        "no-scanf",
        CPP,
        ' sscanf(buf, "%d", &x);\n',
        CPP,
        " auto x = parse_number<int>(buf);\n",
    ),
    (
        "no-std-bind",
        CPP,
        " auto f = std::bind(&Demo::run, this);\n",
        CPP,
        " auto f = [this]() { this->run(); };\n",
    ),
    (
        "no-std-nothrow",
        CPP,
        " auto p = new (std::nothrow) int;\n",
        CPP,
        " auto p = RAMAllocator<int>().make_unique();\n",
    ),
    (
        "log-tag-macro",
        CPP,
        'static const char *const TAG = "demo";\n',
        CPP,
        'ESPHOME_LOG_TAG(TAG, "demo");\n',
    ),
    (
        "log-tag-as-string",
        CPP,
        " set_timeout(TAG, 10, cb);\n",
        CPP,
        ' set_timeout("timer", 10, cb);\n',
    ),
    (
        "log-multiline-continuation",
        CPP,
        'ESP_LOGD(TAG, "first\\nsecond");\n',
        CPP,
        'ESP_LOGD(TAG, "first\\n second");\n',
    ),
    (
        "log-no-bare-literal-ternary",
        CPP,
        'ESP_LOGD(TAG, "%s", ok ? "yes" : "no");\n',
        CPP,
        'ESP_LOGD(TAG, "%s", ok ? LOG_STR_LITERAL("yes") : LOG_STR_LITERAL("no"));\n',
    ),
    (
        "log-direct-call-format-in-flash",
        CPP,
        'esp_log_printf_(1, TAG, 2, "hello");\n',
        CPP,
        'esp_log_printf_(1, TAG, 2, ESPHOME_LOG_FORMAT("hello"));\n',
    ),
    (
        "log-in-header",
        HEADER,
        'ESP_LOGD(TAG, "hello");\n',
        CPP,
        'ESP_LOGD(TAG, "hello");\n',
    ),
]


@pytest.mark.parametrize(
    "check,bad_path,bad,good_path,good", CASES, ids=[c[0] for c in CASES]
)
def test_upstream_checks(check, bad_path, bad, good_path, good):
    findings = ci_custom.check_content(Path(bad_path), bad)
    assert any(f.check == check for f in findings)
    assert not any(
        f.check == check for f in ci_custom.check_content(Path(good_path), good)
    )


def test_executable_bit():
    assert any(
        f.check == "executable-bit"
        for f in ci_custom.check_content(Path(CPP), "", 100755)
    )
    assert not any(
        f.check == "executable-bit"
        for f in ci_custom.check_content(Path(CPP), "", 100644)
    )


def test_all_ported_checks_have_examples():
    assert set(ci_custom.CHECK_IDS) == {c[0] for c in CASES} | {"executable-bit"}


@pytest.mark.parametrize(
    "namespace", ["namespace demo {}", "namespace esphome::demo {}"]
)
def test_clean_cpp(namespace):
    assert not ci_custom.check_content(Path(HEADER), f"#pragma once\n{namespace}\n")


def test_clean_python():
    assert not ci_custom.check_content(
        Path(PYTHON), 'DOMAIN = "demo"\nCONF_FOO = "foo"\n'
    )


@pytest.mark.parametrize(
    "text",
    [
        "// new (std::nothrow) int;\n",
        'const char *s = R"raw(new (std::nothrow) int)raw";\n',
        '// if (ok) ESP_LOGD(TAG, "example");\n',
    ],
)
def test_masked_checks_ignore_comments_and_literals(text):
    checks = {f.check for f in ci_custom.check_content(Path(CPP), text)}
    assert not checks.intersection({"no-std-nothrow", "esp-log-needs-braces"})


def test_cli_findings_and_clean_exit(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    path = Path(CPP)
    path.parent.mkdir(parents=True)
    path.write_text("namespace demo {}\n  delay(50);\n", encoding="utf-8")
    monkeypatch.setattr(ci_custom.subprocess, "check_output", lambda *a, **kw: "")
    assert ci_custom.main([str(path)]) == 1
    output = capsys.readouterr().out
    assert re.search(r"components/demo/demo.cpp:2: \[no-long-delays\]", output)
    assert "  no-long-delays: 1\n" in output
    assert "  TOTAL: 1\n" in output
    path.write_text("namespace demo {}\n", encoding="utf-8")
    assert ci_custom.main([str(path)]) == 0


def test_non_component_paths_are_ignored():
    assert not ci_custom.check_content(Path("tests/host/test_demo.cpp"), "delay(999);")


def test_platform_package_does_not_need_domain():
    assert not ci_custom.check_content(Path("components/demo/sensor/__init__.py"), "")


def test_crlf_preserved_by_cli(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    path = Path(CPP)
    path.parent.mkdir(parents=True)
    path.write_bytes(b"namespace demo {}\r\n")
    monkeypatch.setattr(ci_custom.subprocess, "check_output", lambda *a, **kw: "")
    assert ci_custom.main([str(path)]) == 1
    assert "[newline]" in capsys.readouterr().out


def test_invalid_utf8_is_reported(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    path = Path(CPP)
    path.parent.mkdir(parents=True)
    path.write_bytes(b"namespace demo {}\n\xff")
    monkeypatch.setattr(ci_custom.subprocess, "check_output", lambda *a, **kw: "")
    assert ci_custom.main([str(path)]) == 1
    assert "[utf8] File is not UTF-8" in capsys.readouterr().out
    path.write_text("namespace demo {}\n", encoding="utf-8")
    assert ci_custom.main([str(path)]) == 0
