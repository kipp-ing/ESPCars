#!/usr/bin/env python3
"""Report upstream clang-tidy findings on compiled ESPCars components.

Requires requirements_test.txt + requirements_lint.txt and an `esphome compile`
for each target. Findings are unique (path, line, column, check, message) across
TUs/targets; a diagnostic naming multiple checks counts once for each check.
--summary hides individual findings, --report-only tolerates findings, never
compiler/tool errors. Exit codes: 0 clean/advisory, 1 findings, 2 tool errors.

Options ported from esphome/esphome script/clang-tidy at
03eebae7b6e4cc81058f36a00a7f8c0c98eba341 (ESP-IDF Xtensa/RISC-V only).
CMake entries replace upstream's idedata; no PlatformIO project is required.
Existing source/header edits are read from the working tree. New C++ files or
codegen edits newer than generated defines require another `esphome compile`.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from functools import cache
from importlib.metadata import PackageNotFoundError, version
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TARGETS = tuple(
    f"tests/build/{component}/test.{board}-idf.yaml"
    for component, board in (
        ("linbus", "esp32"),
        ("isotp", "esp32-c6"),
        ("portal", "esp32-c6"),
        ("ccp", "esp32-c6"),
        ("sd_logger", "esp32-c6"),
        ("uds", "esp32-c6"),
        ("can_gateway", "esp32-c6"),
    )
)
# GCC-only options excluded by upstream's clang_options (ESP-IDF subset).
OMIT_FLAGS = frozenset(
    (
        "-free",
        "-fipa-pta",
        "-fstrict-volatile-bitfields",
        "-mlongcalls",
        "-mtext-section-literals",
        "-mdisable-hardware-atomics",
        "-mfix-esp32-psram-cache-issue",
        "-mfix-esp32-psram-cache-strategy=memw",
        "-fno-tree-switch-conversion",
        "-freorder-blocks",
        "-fno-jump-tables",
        "-fno-shrink-wrap",
        "-mno-target-align",
    )
)
# ESPHome's SOURCE_FILE_EXTENSIONS, plus the C++ TU suffixes accepted below.
# Python and assets (e.g. portal/index.html) are not copied into the C++ build.
COMPONENT_SOURCE_EXTENSIONS = {
    ".cpp",
    ".cc",
    ".cxx",
    ".hpp",
    ".h",
    ".c",
    ".tcc",
    ".ino",
}


def locate_database(target):
    """Let ESPHome resolve substitutions, packages and custom build_path."""
    from esphome.config import read_config
    from esphome.core import CORE

    CORE.reset()
    CORE.config_path = ROOT / target
    if read_config({}) is None:
        raise RuntimeError(f"Invalid ESPHome config: {target}")
    database = Path(CORE.build_path) / "build/compile_commands.json"
    if not database.is_file():
        raise RuntimeError(f"Missing {database}; run: esphome compile {target}")
    return database


def has_local_builds():
    """Cheap fresh-clone check, without importing ESPHome or compiling anything."""
    return any(
        (ROOT / "tests/build").glob("*/.esphome/build/*/build/compile_commands.json")
    )


def expand_responses(tokens, directory, seen=()):
    expanded = []
    for token in tokens:
        if token.startswith("@"):
            path = (directory / token[1:]).resolve()
            if path in seen:
                raise ValueError(f"Recursive response file: {path}")
            expanded.extend(
                expand_responses(
                    shlex.split(path.read_text(encoding="utf-8")),
                    directory,
                    (*seen, path),
                )
            )
        else:
            expanded.append(token)
    return expanded


def parse_entry(entry):
    """Parse both compdb spellings; expand response files before filtering."""
    directory = Path(entry["directory"])
    tokens = expand_responses(
        entry["arguments"] if "arguments" in entry else shlex.split(entry["command"]),
        directory,
    )
    if tokens and Path(tokens[0]).name in (
        "ccache",
        "sccache",
        "distcc",
        "icecc",
        "buildcache",
    ):
        tokens = tokens[1:]
    if not tokens or not tokens[0].endswith("-g++"):
        raise ValueError(f"Expected an ESP-IDF GCC C++ compiler for {entry['file']}")
    compiler = str((directory / tokens[0]).resolve())
    flags, includes = [], []
    iterator = iter(tokens[1:])
    for token in iterator:
        if token in ("-c", "-o", "-MT", "-MF", "-MQ"):
            next(iterator)
        elif token in ("-MD", "-MMD", "-MP"):
            continue
        elif token == "-include" or token.startswith("-include"):
            header = (
                next(iterator)
                if token in ("-include", "-include-pch")
                else token[len("-include") :]
            )
            # GCC's generated PCH cannot be consumed by clang. Upstream also
            # analyzes without it. Keep unrelated forced includes intact.
            if token != "-include-pch" and Path(header).name != "cmake_pch.hxx":
                flags.extend(["-include", str((directory / header).resolve())])
        elif token.startswith(("-I", "-isystem", "-iquote")):
            prefix = next(
                p for p in ("-isystem", "-iquote", "-I") if token.startswith(p)
            )
            raw = token[len(prefix) :] or next(iterator)
            includes.append(str((directory / raw).resolve()))
        elif token == "-D":
            flags.append("-D" + next(iterator))
        elif not token.startswith("-"):
            # Source/output filenames may appear bare rather than after -c/-o.
            if (directory / token).resolve() != (directory / entry["file"]).resolve():
                raise ValueError(f"Unexpected compiler argument: {token}")
        else:
            flags.append(token)
    return compiler, flags, includes


@cache
def toolchain_includes(compiler, machine_flags):
    """Probe GCC's builtin includes and active multilib, as upstream does."""
    result = subprocess.run(
        [compiler, *machine_flags, "-E", "-x", "c++", "-", "-v"],
        input="",
        capture_output=True,
        text=True,
        check=False,
    )
    match = re.search(
        r"#include <\.\.\.> search starts here:\n(.*?)End of search list\.",
        result.stderr,
        re.DOTALL,
    )
    if result.returncode or not match:
        raise RuntimeError(f"Cannot probe {compiler}:\n{result.stderr}")
    directories = [line.strip() for line in match[1].splitlines() if line.strip()]
    multilib = subprocess.run(
        [compiler, *machine_flags, "-print-multi-directory"],
        capture_output=True,
        text=True,
        check=False,
    )
    if multilib.returncode:
        raise RuntimeError(f"Cannot probe multilib for {compiler}:\n{multilib.stderr}")
    result_dirs = []
    for directory in directories:
        extra = Path(directory) / multilib.stdout.strip()
        if multilib.stdout.strip() not in ("", ".") and extra.is_dir():
            result_dirs.append(str(extra))
        result_dirs.append(directory)
    return result_dirs


def clang_options(compiler, flags, includes, source_root, overlay):
    triplet = Path(compiler).name.removesuffix("-g++")
    if triplet.startswith("xtensa-"):
        options = [
            "--target=i386-unknown-linux-gnu",
            "-m32",
            "-U__i386__",
            "-U__x86_64__",
            "-D__XTENSA__",
            "-D__XTENSA_EL__",
            "-D_LIBC",
        ]
        # Explicit i386 target is needed on ARM hosts; upstream's -m32 alone
        # selects the right target only when the host is x86.
    elif triplet.startswith("riscv32-"):
        options = [f"--target={triplet}", "-Qunused-arguments"]
    else:
        raise ValueError(f"Unsupported ESP-IDF toolchain: {compiler}")
    options += [
        "-nostdinc++",
        "-nostdinc",
        "-DCLANG_TIDY",
        "-D_GLIBCXX_HAVE_TLS",
        "-Ddeprecated(x)=",
        "-D_PGMSPACE_H_",
        "-Dpgm_read_byte(s)=(*(const uint8_t *)(s))",
        "-Dpgm_read_byte_near(s)=(*(const uint8_t *)(s))",
        "-Dpgm_read_word(s)=(*(const uint16_t *)(s))",
        "-Dpgm_read_dword(s)=(*(const uint32_t *)(s))",
        "-Dpgm_read_ptr(s)=(*(const void *const *)(s))",
        "-DPROGMEM=",
        "-DPGM_P=const char *",
        "-DPSTR(s)=(s)",
        "-DPSTRN(s, n)=(s)",
    ]
    options.extend(
        re.sub(r"_xesp\w+", "", flag)
        if triplet.startswith("riscv") and flag.startswith("-march=")
        else flag
        for flag in flags
        if flag not in OMIT_FLAGS
        and not flag.startswith(("-Werror", "-std=", "-mtune=esp"))
    )
    options += ["-std=gnu++20", "-fno-cxx-modules"]
    machine_flags = tuple(flag for flag in flags if flag.startswith("-m"))
    for directory in toolchain_includes(compiler, machine_flags):
        options += ["-isystem", directory]
    # Repository components take precedence over ESPHome's generated copies.
    # All other SDK/library headers are system headers, matching upstream.
    options += ["-I", str(overlay), "-I", str(source_root)]
    for directory in includes:
        if Path(directory) not in (source_root, source_root / "esphome"):
            options += ["-isystem", directory]
    return options


def repo_path(path, directory, root=None):
    """Map copied build paths and temporary include symlinks to repo paths."""
    root = ROOT if root is None else root
    resolved = (directory / path).resolve()
    try:
        relative = resolved.relative_to(root / "components")
    except ValueError:
        marker = "/src/esphome/components/"
        if marker not in str(resolved):
            return None
        relative = Path(str(resolved).split(marker, 1)[1])
    name = Path("components") / relative
    return name.as_posix() if (root / name).is_file() else None


@dataclass(frozen=True, order=True)
class Finding:
    path: str
    line: int
    column: int
    check: str
    message: str


def read_findings(diagnostics, directory):
    findings, errors = set(), []
    for diagnostic in diagnostics:
        check = diagnostic["DiagnosticName"]
        message = diagnostic["DiagnosticMessage"]
        if check == "clang-diagnostic-error":
            errors.append(f"{message.get('FilePath', '')}: {message['Message']}")
            continue
        name = repo_path(message.get("FilePath", ""), directory)
        if name is None:
            continue
        data = (ROOT / name).read_bytes()
        offset = message["FileOffset"]
        line = data.count(b"\n", 0, offset) + 1
        column = offset - data.rfind(b"\n", 0, offset)
        # Diagnostics sometimes name aliases together; count each named check.
        for item in check.split(","):
            findings.add(Finding(name, line, column, item.strip(), message["Message"]))
    return findings, errors


def run_tidy(executable, source, options, directory, fixes):
    result = subprocess.run(
        [
            executable,
            "--quiet",
            f"--config-file={ROOT / '.clang-tidy'}",
            f"--export-fixes={fixes}",
            str(source),
            "--",
            *options,
        ],
        cwd=directory,
        capture_output=True,
        text=True,
        check=False,
    )
    document = (
        yaml.safe_load(fixes.read_text(encoding="utf-8")) if fixes.is_file() else {}
    )
    diagnostics = (document or {}).get("Diagnostics", [])
    findings, errors = read_findings(diagnostics, directory)
    if (
        errors
        or result.returncode not in (0, 1)
        or (result.returncode and not diagnostics)
    ):
        raise RuntimeError(
            f"{source}: clang-tidy exited {result.returncode}\n"
            + result.stdout
            + result.stderr
        )
    return findings


def check_build_freshness(source_root, components, target):
    """Reject missing build inputs and defines older than component codegen."""
    generated = source_root / "esphome/core/defines.h"
    problems = []
    if not generated.is_file():
        problems.append(f"Missing generated config: {generated}")
    for component in components:
        repo_component = ROOT / "components" / component
        copied_component = source_root / "esphome/components" / component
        for path in sorted(repo_component.rglob("*")):
            if path.is_file() and path.suffix in COMPONENT_SOURCE_EXTENSIONS:
                copied = copied_component / path.relative_to(repo_component)
                if not copied.is_file():
                    problems.append(
                        f"New component file missing from build: {path.relative_to(ROOT)}"
                    )
        codegen = repo_component / "__init__.py"
        if (
            generated.is_file()
            and codegen.is_file()
            and codegen.stat().st_mtime_ns > generated.stat().st_mtime_ns
        ):
            problems.append(
                f"Generated config {generated} is older than {codegen.relative_to(ROOT)}"
            )
    if problems:
        raise RuntimeError("\n".join([*problems, f"Run: esphome compile {target}"]))


def prepare_jobs(database, temporary, target):
    entries = json.loads(database.read_text(encoding="utf-8"))
    if not isinstance(entries, list):
        raise ValueError(f"{database}: expected a compile-command list")
    selected = []
    source_root = database.parent.parent / "src"
    for entry in entries:
        name = repo_path(entry["file"], Path(entry["directory"]))
        if name and Path(name).suffix in (".cpp", ".cc", ".cxx"):
            selected.append((entry, name))
    if not selected:
        raise ValueError(f"{database}: no component C++ translation units")
    components = sorted(
        {Path(name).parts[1] for _, name in selected}
        | {
            path.name
            for path in (source_root / "esphome/components").iterdir()
            if path.is_dir() and (ROOT / "components" / path.name).is_dir()
        }
    )
    check_build_freshness(source_root, components, target)
    overlay = temporary / "include"
    remappings = []
    for component in components:
        link = overlay / "esphome/components" / component
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(ROOT / "components" / component, target_is_directory=True)
        for path in sorted((ROOT / "components" / component).rglob("*")):
            if path.is_file() and path.suffix in COMPONENT_SOURCE_EXTENSIONS:
                copied = source_root / "esphome" / path.relative_to(ROOT)
                remappings.append(
                    {
                        "type": "file",
                        "name": str(copied),
                        "external-contents": str(path),
                    }
                )
    # -I alone cannot override a quoted include relative to a generated header
    # (notably src/esphome.h). Redirect those copied paths as well, without
    # modifying the build tree. Clang reports the external repository filenames.
    vfs = temporary / "components-vfs.json"
    vfs.write_text(
        json.dumps({"version": 0, "use-external-names": True, "roots": remappings}),
        encoding="utf-8",
    )
    jobs = []
    for entry, name in selected:
        compiler, flags, includes = parse_entry(entry)
        options = clang_options(compiler, flags, includes, source_root, overlay)
        options += ["-ivfsoverlay", str(vfs)]
        # The database identifies flags only. Never pass its copied input to
        # clang-tidy; existing component source edits need no codegen/compile.
        jobs.append((ROOT / name, options, Path(entry["directory"])))
    # One TU includes every header of every active repo component, even headers
    # unused by the generated application (upstream --all-headers equivalent).
    headers = sorted(
        path
        for component in components
        for path in (ROOT / "components" / component).rglob("*.h")
    )
    all_headers = temporary / "all_headers.cpp"
    all_headers.write_text(
        "".join(f'#include "{path}"\n' for path in headers), encoding="utf-8"
    )
    jobs.append((all_headers, jobs[0][1], jobs[0][2]))
    return jobs, components


def count_findings(findings, components):
    return Counter(f.check for f in findings), Counter(
        {c: sum(Path(f.path).parts[1] == c for f in findings) for c in components}
    )


def print_counts(findings, components, errors):
    checks, areas = count_findings(findings, components)
    print("clang-tidy counts (unique path, line, column, check, message):")
    for label, counts in (("check", checks), ("component", areas)):
        print(f"  by {label}:")
        for name, count in sorted(counts.items()):
            print(f"    {name}: {count}")
    print(f"  TOTAL: {len(findings)}")
    print(f"  tool errors: {len(errors)}")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a", encoding="utf-8") as output:
            output.write("\n### clang-tidy counts\n\n")
            if errors:
                output.write("**INCOMPLETE: tool errors; counts are partial.**\n\n")
            for label, counts in (("Check", checks), ("Component", areas)):
                output.write(f"| {label} | Count |\n|---|---:|\n")
                for name, count in sorted(counts.items()):
                    output.write(f"| {name} | {count} |\n")
                output.write("\n")
            output.write(
                f"Total: **{len(findings)}**; tool errors: **{len(errors)}**.\n"
            )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "targets",
        nargs="*",
        default=list(DEFAULT_TARGETS),
        help="tests/build target YAMLs (default: seven component targets)",
    )
    parser.add_argument("-j", "--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument(
        "--summary", action="store_true", help="Print counts and full tool errors only"
    )
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="Findings exit 0; tool/compiler errors still exit 2",
    )
    parser.add_argument(
        "--list-targets",
        action="store_true",
        help="Print default YAMLs for CI compilation and exit",
    )
    args = parser.parse_args(argv)
    if args.list_targets:
        print("\n".join(DEFAULT_TARGETS))
        return 0
    if args.jobs < 1:
        parser.error("-j must be positive")
    findings, components, errors = set(), set(), []
    try:
        pins = (ROOT / "requirements_lint.txt").read_text(encoding="utf-8")
        expected = re.search(r"^clang-tidy==([\w.]+)$", pins, re.MULTILINE)[1]
        actual = version("clang-tidy")
        if actual != expected:
            raise RuntimeError(f"clang-tidy: expected {expected}, found {actual}")
        executable = str(Path(sys.executable).parent / "clang-tidy")
        with tempfile.TemporaryDirectory(prefix="espcars-tidy-") as temporary:
            jobs = []
            for index, target in enumerate(args.targets):
                directory = Path(temporary) / str(index)
                directory.mkdir()
                target_jobs, active = prepare_jobs(
                    locate_database(target), directory, target
                )
                components.update(active)
                jobs.extend(target_jobs)
            if not args.summary:
                print(
                    f"clang-tidy {actual}: {len(jobs)} TUs (including {len(args.targets)} header TUs), {args.jobs} workers",
                    flush=True,
                )
            with ThreadPoolExecutor(max_workers=args.jobs) as executor:
                futures = [
                    executor.submit(
                        run_tidy,
                        executable,
                        source,
                        options,
                        directory,
                        Path(temporary) / f"fixes-{index}.yaml",
                    )
                    for index, (source, options, directory) in enumerate(jobs)
                ]
                for future in as_completed(futures):
                    try:
                        findings.update(future.result())
                    except (
                        OSError,
                        ValueError,
                        RuntimeError,
                        KeyError,
                        TypeError,
                        yaml.YAMLError,
                    ) as error:
                        errors.append(str(error))
    except (
        OSError,
        PackageNotFoundError,
        ImportError,
        ValueError,
        RuntimeError,
        KeyError,
        TypeError,
        StopIteration,
        yaml.YAMLError,
    ) as error:
        errors.append(str(error))
    if not args.summary:
        for finding in sorted(findings):
            print(f"{finding.path}:{finding.line}: [{finding.check}] {finding.message}")
    for error in errors:
        print(f"ERROR [clang-tidy]: {error}")
    print_counts(findings, components, errors)
    return 2 if errors else int(bool(findings) and not args.report_only)


if __name__ == "__main__":
    sys.exit(main())
