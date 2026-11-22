"""Exercise the runner with fake CMake databases and fake clang-tidy output."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

import pytest
import yaml

from script import clang_tidy as tidy, lint_report

REPO = Path(__file__).resolve().parents[2]


@pytest.fixture
def tree(tmp_path, monkeypatch):
    monkeypatch.setattr(tidy, "ROOT", tmp_path)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    source = tmp_path / "components/demo/demo.cpp"
    source.parent.mkdir(parents=True)
    source.write_text("// café\nint BadName = 1;\n", encoding="utf-8")
    (source.parent / "__init__.py").write_text("# codegen\n", encoding="utf-8")
    (source.parent / "unused.h").write_text("#pragma once\n", encoding="utf-8")
    (tmp_path / "requirements_lint.txt").write_text(
        "clang-tidy==23.1.4\n", encoding="utf-8"
    )
    monkeypatch.setattr(tidy, "version", lambda package: "23.1.4")
    monkeypatch.setattr(tidy, "toolchain_includes", lambda *args: ["/sdk/include"])
    return tmp_path


def database(tree, name="one", *, arguments=True):
    build = tree / f"tests/build/demo/.esphome/build/{name}/build"
    build.mkdir(parents=True)
    copied = build.parent / "src/esphome/components/demo/demo.cpp"
    copied.parent.mkdir(parents=True)
    for filename in ("demo.cpp", "unused.h"):
        shutil.copyfile(tree / "components/demo" / filename, copied.parent / filename)
    generated = build.parent / "src/esphome/core/defines.h"
    generated.parent.mkdir(parents=True)
    generated.write_text("#define GENERATED_VALUE 42\n", encoding="utf-8")
    os.utime(tree / "components/demo/__init__.py", ns=(1_000_000_000, 1_000_000_000))
    os.utime(generated, ns=(2_000_000_000, 2_000_000_000))
    entry = {
        "directory": str(build),
        "file": str(copied),
    }
    tokens = [
        "/sdk/bin/riscv32-esp-elf-g++",
        "-DTEST=1",
        "-I../src",
        "-c",
        str(copied),
        "-o",
        "demo.o",
    ]
    entry["arguments" if arguments else "command"] = (
        tokens if arguments else shlex.join(tokens)
    )
    path = build / "compile_commands.json"
    path.write_text(json.dumps([entry]), encoding="utf-8")
    return path


def diagnostic(
    path, check="readability-identifier-naming", message="bad name", offset=9
):
    return {
        "DiagnosticName": check,
        "DiagnosticMessage": {
            "FilePath": str(path),
            "FileOffset": offset,
            "Message": message,
        },
        "Level": "Error",  # WarningsAsErrors elevates ordinary findings too.
    }


def test_path_mapping(tree):
    copied = database(tree).parent.parent / "src/esphome/components/demo/demo.cpp"
    assert tidy.repo_path(str(copied), tree) == "components/demo/demo.cpp"
    assert (
        tidy.repo_path("components/demo/demo.cpp", tree) == "components/demo/demo.cpp"
    )
    assert tidy.repo_path("/sdk/include/stdlib.h", tree) is None
    assert tidy.repo_path(str(copied).replace("demo.cpp", "absent.cpp"), tree) is None
    assert tidy.repo_path(str(copied).replace("/demo/", "/builtin/"), tree) is None
    link = tree / "include/esphome/components/demo"
    link.parent.mkdir(parents=True)
    link.symlink_to(tree / "components/demo", target_is_directory=True)
    assert tidy.repo_path(str(link / "unused.h"), tree) == "components/demo/unused.h"


@pytest.mark.parametrize("arguments", [True, False])
def test_parse_and_header_coverage_use_current_repo_sources(tree, arguments):
    db = database(tree, arguments=arguments)
    temporary = tree / "temporary"
    jobs, components = tidy.prepare_jobs(db, temporary, "tests/build/demo/test.yaml")
    assert components == ["demo"]
    assert jobs[0][0] == tree / "components/demo/demo.cpp"
    assert (
        jobs[1][0].read_text(encoding="utf-8")
        == f'#include "{tree}/components/demo/unused.h"\n'
    )
    assert jobs[0][1].index(str(temporary / "include")) < jobs[0][1].index(
        str(db.parent.parent / "src")
    )
    assert (
        tidy.repo_path(
            str(temporary / "include/esphome/components/demo/unused.h"), tree
        )
        == "components/demo/unused.h"
    )


def test_response_flags_launcher_and_pch(tree):
    (tree / "inner.rsp").write_text('-mlongcalls -DNAME=\\"value\\"', encoding="utf-8")
    (tree / "flags.rsp").write_text(
        "@inner.rsp -march=rv32imac_xesploop_xespv -std=gnu++2b", encoding="utf-8"
    )
    compiler, flags, includes = tidy.parse_entry(
        {
            "directory": str(tree),
            "file": "source.cpp",
            "command": 'ccache /sdk/bin/riscv32-esp-elf-g++ @flags.rsp -I "a path" -isystem sdk -include cmake_pch.hxx -MF deps -MT obj -MMD -o obj -c source.cpp',
        }
    )
    assert compiler == "/sdk/bin/riscv32-esp-elf-g++"
    assert flags == [
        "-mlongcalls",
        '-DNAME="value"',
        "-march=rv32imac_xesploop_xespv",
        "-std=gnu++2b",
    ]
    assert includes == [str(tree / "a path"), str(tree / "sdk")]
    assert tidy.parse_entry(
        {
            "directory": str(tree),
            "file": "source.cpp",
            "arguments": [compiler, "-include", "custom.h", "-c", "source.cpp"],
        }
    )[1] == ["-include", str(tree / "custom.h")]
    options = tidy.clang_options(
        compiler,
        flags + ["-Werror=all", "-mtune=esp32c6", "-fno-shrink-wrap"],
        includes,
        tree / "src",
        tree / "include",
    )
    assert "--target=riscv32-esp-elf" in options
    assert "-march=rv32imac" in options
    assert "-std=gnu++20" in options
    assert "-fno-cxx-modules" in options
    assert "-DCLANG_TIDY" in options
    assert not any(
        f in options
        for f in (
            "-mlongcalls",
            "-std=gnu++2b",
            "-Werror=all",
            "-mtune=esp32c6",
            "-fno-shrink-wrap",
        )
    )


def test_xtensa_target_on_arm_host(tree):
    options = tidy.clang_options(
        "/sdk/bin/xtensa-esp32-elf-g++",
        list(tidy.OMIT_FLAGS),
        [],
        tree / "src",
        tree / "include",
    )
    assert "--target=i386-unknown-linux-gnu" in options
    assert "-U__i386__" in options
    assert "-D__XTENSA_EL__" in options
    assert not tidy.OMIT_FLAGS.intersection(options)


def test_bad_response_file_fails(tree):
    (tree / "recursive.rsp").write_text("@recursive.rsp", encoding="utf-8")
    with pytest.raises(ValueError, match="Recursive"):
        tidy.expand_responses(["@recursive.rsp"], tree)
    with pytest.raises(FileNotFoundError):
        tidy.expand_responses(["@missing.rsp"], tree)


def test_multilib_probe_and_failure(tmp_path, monkeypatch):
    include = tmp_path / "include"
    (include / "rv32").mkdir(parents=True)
    commands = []

    def probe(command, **kwargs):
        commands.append(command)
        if command[-1] == "-print-multi-directory":
            return subprocess.CompletedProcess(command, 0, "rv32\n", "")
        return subprocess.CompletedProcess(
            command,
            0,
            "",
            f"#include <...> search starts here:\n {include}\nEnd of search list.\n",
        )

    monkeypatch.setattr(tidy.subprocess, "run", probe)
    tidy.toolchain_includes.cache_clear()
    assert tidy.toolchain_includes("/sdk/g++", ("-march=rv32imac",)) == [
        str(include / "rv32"),
        str(include),
    ]
    assert all("-march=rv32imac" in command for command in commands)
    monkeypatch.setattr(
        tidy.subprocess,
        "run",
        lambda command, **kwargs: subprocess.CompletedProcess(
            command, 1, "", "probe failed"
        ),
    )
    with pytest.raises(RuntimeError, match="probe failed"):
        tidy.toolchain_includes("/missing/g++", ())
    tidy.toolchain_includes.cache_clear()


def test_deduplication_offsets_aliases_counts_and_external_errors(tree):
    source = tree / "components/demo/demo.cpp"
    copied = database(tree).parent.parent / "src/esphome/components/demo/demo.cpp"
    # Byte offsets, including UTF-8 in a preceding line.
    first = diagnostic(source, "check-a,check-b")
    second = diagnostic(copied, "check-a,check-b")
    findings, errors = tidy.read_findings(
        [
            first,
            second,
            diagnostic("/sdk/file.h", "clang-diagnostic-error", "SDK broken"),
            diagnostic("/sdk/file.h"),
        ],
        tree,
    )
    assert len(findings) == 2
    assert all((f.line, f.column) == (2, 1) for f in findings)
    assert errors == ["/sdk/file.h: SDK broken"]
    checks, components = tidy.count_findings(findings, ["demo", "empty"])
    assert checks == {"check-a": 1, "check-b": 1}
    assert components == {"demo": 2, "empty": 0}


@pytest.mark.parametrize(
    "code,check,error",
    [
        (1, "readability-identifier-naming", False),
        (0, "clang-diagnostic-error", True),
        (1, "clang-diagnostic-error", True),
        (-11, "readability-identifier-naming", True),
        (2, "readability-identifier-naming", True),
        (1, None, True),
        (0, None, False),
    ],
)
def test_tidy_process_errors_are_never_advisory(tree, monkeypatch, code, check, error):
    fixes = tree / "fixes.yaml"

    def fake_tidy(command, **kwargs):
        assert f"--config-file={tree / '.clang-tidy'}" in command
        assert "--fix" not in command
        if check:
            fixes.write_text(
                yaml.safe_dump(
                    {
                        "Diagnostics": [
                            diagnostic(tree / "components/demo/demo.cpp", check)
                        ]
                    }
                ),
                encoding="utf-8",
            )
        return subprocess.CompletedProcess(command, code, "full stdout", "full stderr")

    monkeypatch.setattr(tidy.subprocess, "run", fake_tidy)
    if error:
        with pytest.raises(RuntimeError, match="full stdoutfull stderr"):
            tidy.run_tidy(
                "clang-tidy", tree / "components/demo/demo.cpp", [], tree, fixes
            )
    else:
        assert len(
            tidy.run_tidy(
                "clang-tidy", tree / "components/demo/demo.cpp", [], tree, fixes
            )
        ) == int(bool(check))


@pytest.mark.parametrize("summary", [False, True])
@pytest.mark.parametrize("report_only", [False, True])
@pytest.mark.parametrize("state", ["clean", "findings", "error"])
def test_cli_counts_exit_codes_and_summary(
    tree, monkeypatch, capsys, summary, report_only, state
):
    dbs = {name: database(tree, name) for name in ("one", "two")}
    monkeypatch.setattr(tidy, "locate_database", dbs.__getitem__)
    finding = tidy.Finding("components/demo/demo.cpp", 2, 1, "check-a", "details")

    def run(*args):
        if state == "error":
            raise RuntimeError("full tool error details")
        return {finding} if state == "findings" else set()

    monkeypatch.setattr(tidy, "run_tidy", run)
    summary_file = tree / "summary.md"
    summary_file.write_text("Existing\n", encoding="utf-8")
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary_file))
    args = ["one", "two", "-j", "2"]
    if summary:
        args.append("--summary")
    if report_only:
        args.append("--report-only")
    assert tidy.main(args) == (
        2 if state == "error" else int(state == "findings" and not report_only)
    )
    output = capsys.readouterr().out
    count = int(state == "findings")
    assert f"  TOTAL: {count}\n" in output
    assert ("[check-a] details" in output) == (state == "findings" and not summary)
    assert ("full tool error details" in output) == (state == "error")
    content = summary_file.read_text(encoding="utf-8")
    assert content.startswith("Existing\n")
    assert f"| demo | {count} |" in content
    assert ("INCOMPLETE" in content) == (state == "error")


@pytest.mark.parametrize("strict", [False, True])
def test_partial_builds_analyze_available_targets(tree, monkeypatch, capsys, strict):
    from esphome import config
    from esphome.core import CORE

    db = database(tree)

    def read_config(*args):
        CORE.build_path = str(
            db.parent.parent
            if CORE.config_path.name == "ready.yaml"
            else tree / "not-built"
        )
        return {}

    monkeypatch.setattr(config, "read_config", read_config)
    finding = tidy.Finding("components/demo/demo.cpp", 2, 1, "check-a", "details")
    calls = []

    def run(*args):
        calls.append(args)
        return {finding}

    monkeypatch.setattr(tidy, "run_tidy", run)
    args = ["missing.yaml", "ready.yaml", "--report-only", "--summary"]
    if strict:
        args.append("--strict")
    assert tidy.main(args) == (2 if strict else 0)
    assert len(calls) == 2
    output = capsys.readouterr().out
    assert "run: esphome compile missing.yaml" in output
    assert ("SKIP [clang-tidy]: missing.yaml:" in output) == (not strict)
    assert f"  tool errors: {int(strict)}\n" in output
    assert "  TOTAL: 1\n" in output
    assert "targets analyzed (1/2): ready.yaml" in output
    assert "INCOMPLETE" in output


@pytest.mark.parametrize("strict", [False, True])
@pytest.mark.parametrize("installed", [False, True])
def test_missing_or_wrong_version_is_a_tool_error(
    tree, monkeypatch, capsys, installed, strict
):
    def wrong_version(package):
        if not installed:
            raise tidy.PackageNotFoundError(package)
        return "22.0"

    monkeypatch.setattr(tidy, "version", wrong_version)
    assert tidy.main(["--report-only", *(["--strict"] if strict else [])]) == 2
    expected = "expected 23.1.4, found 22.0" if installed else "No package metadata"
    assert expected in capsys.readouterr().out


def test_empty_database_is_an_error(tree):
    db = database(tree)
    db.write_text("[]", encoding="utf-8")
    with pytest.raises(ValueError, match=r"no component C\+\+ translation units"):
        tidy.prepare_jobs(db, tree / "temp", "tests/build/demo/test.yaml")


def test_ci_targets_and_nonblocking_wiring(capsys):
    assert tidy.main(["--list-targets"]) == 0
    assert capsys.readouterr().out.splitlines() == list(tidy.DEFAULT_TARGETS)
    assert len(tidy.DEFAULT_TARGETS) == 7
    assert all((REPO / target).is_file() for target in tidy.DEFAULT_TARGETS)
    jobs = yaml.safe_load(
        (REPO / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    )["jobs"]
    assert "clang-tidy" not in jobs["required-checks"]["needs"]
    job = jobs["clang-tidy"]
    assert not job.get("continue-on-error", False)
    runs = "\n".join(step.get("run", "") for step in job["steps"])
    assert "script/clang_tidy.py --list-targets" in runs
    assert 'esphome compile "$config"' in runs
    assert "python script/clang_tidy.py --report-only --strict" in runs
    assert "~/.esphome-idf" in str(job)
    assert "clang-tidy" in lint_report.TOOLS


def test_lint_runner_skips_fresh_clone(tree, monkeypatch, capsys):
    monkeypatch.setattr(lint_report, "ROOT", REPO)
    monkeypatch.setattr(lint_report, "selected_files", lambda config: {})
    pins = dict(
        re.findall(
            r"^([\w-]+)==([\w.]+)$",
            (REPO / "requirements_lint.txt").read_text(encoding="utf-8"),
            re.MULTILINE,
        )
    )
    monkeypatch.setattr(lint_report, "version", pins.__getitem__)
    assert not tidy.has_local_builds()
    assert lint_report.main(["--only", "clang-tidy", "--summary"]) == 0
    assert "SKIPPED (no local compile_commands.json" in capsys.readouterr().out


@pytest.mark.parametrize("suffix", [".cpp", ".h", ".hpp", ".tcc"])
@pytest.mark.parametrize("strict", [False, True])
def test_new_component_file_requires_compile(tree, monkeypatch, capsys, suffix, strict):
    db = database(tree)
    new_file = tree / f"components/demo/nested/new{suffix}"
    new_file.parent.mkdir()
    new_file.write_text("// new source\n", encoding="utf-8")
    monkeypatch.setattr(tidy, "locate_database", lambda target: db)
    monkeypatch.setattr(
        tidy,
        "run_tidy",
        lambda *args: pytest.fail("stale build must be detected before clang-tidy"),
    )
    target = "tests/build/demo/test.yaml"
    args = [target, "--report-only", "--summary", *(["--strict"] if strict else [])]
    assert tidy.main(args) == (2 if strict else 0)
    output = capsys.readouterr().out
    assert (
        f"New component file missing from build: components/demo/nested/new{suffix}"
        in output
    )
    assert f"esphome compile {target}" in output
    assert ("SKIP [clang-tidy]:" in output) == (not strict)
    assert f"  tool errors: {int(strict)}\n" in output
    assert "targets analyzed (0/1): (none)" in output


@pytest.mark.parametrize("state", ["missing", "older", "same", "newer"])
@pytest.mark.parametrize("strict", [False, True])
def test_generated_config_freshness_guard(tree, monkeypatch, capsys, state, strict):
    db = database(tree)
    generated = db.parent.parent / "src/esphome/core/defines.h"
    if state == "missing":
        generated.unlink()
    else:
        timestamp = {
            "older": 500_000_000,
            "same": 1_000_000_000,
            "newer": 2_000_000_000,
        }[state]
        os.utime(generated, ns=(timestamp, timestamp))
    monkeypatch.setattr(tidy, "locate_database", lambda target: db)
    monkeypatch.setattr(tidy, "run_tidy", lambda *args: set())
    target = "tests/build/demo/test.yaml"
    stale = state in ("missing", "older")
    args = [target, "--report-only", "--summary", *(["--strict"] if strict else [])]
    assert tidy.main(args) == (2 if stale and strict else 0)
    output = capsys.readouterr().out
    if stale:
        assert "defines.h" in output
        assert f"esphome compile {target}" in output
        assert ("SKIP [clang-tidy]:" in output) == (not strict)
        assert f"  tool errors: {int(strict)}\n" in output
        assert "targets analyzed (0/1): (none)" in output
    if state == "older":
        assert "components/demo/__init__.py" in output


@pytest.mark.parametrize("summary", [False, True])
@pytest.mark.parametrize("state", ["missing", "stale", "new-file", "crash"])
def test_lint_report_partial_builds_keep_coverage_visible(
    tree, monkeypatch, capsys, summary, state
):
    ready = database(tree, "ready")
    unavailable = database(tree, "unavailable")
    if state == "missing":
        unavailable.unlink()
    elif state == "stale":
        generated = unavailable.parent.parent / "src/esphome/core/defines.h"
        os.utime(generated, ns=(500_000_000, 500_000_000))
    elif state == "new-file":
        (unavailable.parent.parent / "src/esphome/components/demo/unused.h").unlink()

    from esphome import config
    from esphome.core import CORE

    def read_config(*args):
        CORE.build_path = str(
            ready.parent.parent
            if CORE.config_path.name == "ready.yaml"
            else unavailable.parent.parent
        )
        return {}

    monkeypatch.setattr(config, "read_config", read_config)
    monkeypatch.setattr(tidy, "DEFAULT_TARGETS", ("unavailable.yaml", "ready.yaml"))
    monkeypatch.setattr(lint_report, "ROOT", REPO)
    monkeypatch.setattr(lint_report, "selected_files", lambda config: {})
    pins = dict(
        re.findall(
            r"^([\w-]+)==([\w.]+)$",
            (REPO / "requirements_lint.txt").read_text(encoding="utf-8"),
            re.MULTILINE,
        )
    )
    monkeypatch.setattr(lint_report, "version", pins.__getitem__)
    finding = tidy.Finding("components/demo/demo.cpp", 2, 1, "check-a", "details")

    def run(*args):
        if state == "crash":
            raise RuntimeError("clang-tidy crashed")
        return {finding}

    monkeypatch.setattr(tidy, "run_tidy", run)

    def invoke(command):
        assert command[1] == "script/clang_tidy.py"
        output = io.StringIO()
        with redirect_stdout(output):
            code = tidy.main(command[2:])
        return subprocess.CompletedProcess(command, code, output.getvalue(), "")

    monkeypatch.setattr(lint_report, "run", invoke)
    args = ["--only", "clang-tidy", "--report-only"]
    if summary:
        args.append("--summary")
    assert lint_report.main(args) == (2 if state == "crash" else 0)
    output = capsys.readouterr().out
    if state == "crash":
        assert "clang-tidy crashed" in output
    else:
        assert output.count("SKIP [clang-tidy]: unavailable.yaml:") == 1
        assert "esphome compile unavailable.yaml" in output
        assert "targets analyzed (1/2): ready.yaml" in output
        assert "INCOMPLETE" in output
        assert "  clang-tidy: 1\n" in output
        assert "ERROR" not in output


def test_dependency_codegen_also_requires_compile(tree):
    db = database(tree)
    dependency = tree / "components/dependency"
    dependency.mkdir()
    init = dependency / "__init__.py"
    init.write_text("# dependency codegen\n", encoding="utf-8")
    # A header-only dependency has no compile-command entry of its own.
    (db.parent.parent / "src/esphome/components/dependency").mkdir()
    with pytest.raises(RuntimeError, match="components/dependency/__init__.py"):
        tidy.prepare_jobs(db, tree / "temp", "tests/build/demo/test.yaml")


def test_repo_source_edits_are_passed_to_clang_tidy(tree, monkeypatch):
    db = database(tree)
    copied = db.parent.parent / "src/esphome/components/demo/demo.cpp"
    original = copied.read_bytes()
    source = tree / "components/demo/demo.cpp"
    source.write_text("int UpdatedName = 3;\n", encoding="utf-8")
    jobs, _ = tidy.prepare_jobs(db, tree / "temp", "tests/build/demo/test.yaml")

    def fake_tidy(command, **kwargs):
        input_path = Path(command[command.index("--") - 1])
        assert input_path == source
        assert input_path.read_text(encoding="utf-8") == "int UpdatedName = 3;\n"
        assert str(copied) not in command
        assert "-DTEST=1" in command
        return subprocess.CompletedProcess(command, 0, "", "")

    monkeypatch.setattr(tidy.subprocess, "run", fake_tidy)
    assert tidy.run_tidy("clang-tidy", *jobs[0], tree / "fixes.yaml") == set()
    assert copied.read_bytes() == original


def test_preprocessor_uses_repo_headers_even_from_generated_header(tree):
    clang = shutil.which("clang++")
    if clang is None:
        pytest.skip("clang++ is needed for the synthetic -E/-H include resolution test")
    db = database(tree)
    source_root = db.parent.parent / "src"
    source = tree / "components/demo/demo.cpp"
    source.write_text(
        '#include "generated_bridge.h"\nint cpp_edit = TEST;\n', encoding="utf-8"
    )
    # This bridge forces quote lookup relative to build/src, ahead of -I paths.
    (source_root / "generated_bridge.h").write_text(
        '#include "esphome/components/demo/unused.h"\n#include "esphome/core/defines.h"\nint generated_value = GENERATED_VALUE;\n',
        encoding="utf-8",
    )
    repo_header = tree / "components/demo/unused.h"
    repo_header.write_text("#pragma once\nint header_edit = 17;\n", encoding="utf-8")
    copied_header = source_root / "esphome/components/demo/unused.h"
    copied_header.write_text(
        '#error "stale build copy must never be read"\n', encoding="utf-8"
    )
    jobs, _ = tidy.prepare_jobs(db, tree / "temp", "tests/build/demo/test.yaml")
    for input_path, options, directory in jobs:
        # Exercise the real include/VFS options, with a host preprocessor and
        # no firmware compilation, SDK, or cross compiler required.
        includes = []
        iterator = iter(options)
        for option in iterator:
            if option in ("-I", "-isystem", "-ivfsoverlay"):
                includes.extend([option, next(iterator)])
        result = subprocess.run(
            [clang, "-E", "-P", "-H", "-DTEST=1", str(input_path), *includes],
            cwd=directory,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        assert "int header_edit = 17;" in result.stdout
        assert str(repo_header) in result.stderr
        assert str(copied_header) not in result.stderr
        if input_path == source:
            assert "int cpp_edit = 1;" in result.stdout
            assert "int generated_value = 42;" in result.stdout
