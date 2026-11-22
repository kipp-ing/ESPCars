#!/usr/bin/env python3
"""Run upstream linters without modifying the checkout; findings fail by default.

Install requirements_lint.txt into the same environment as requirements_test.txt.
Both check.sh and CI call this entry point. Mutating tools run on disposable
copies, independently, so one tool's fixes cannot hide another tool's findings.
Counts are diagnostics for ruff/yamllint/ci-custom, replacements for clang-format,
and affected files for formatters, pyupgrade and whitespace hooks.
Use --report-only to exit successfully on findings; tool errors still fail.
"""

import argparse
from collections import Counter
from contextlib import nullcontext, redirect_stdout
import difflib
from importlib.metadata import PackageNotFoundError, version
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import yaml

ROOT = Path(__file__).resolve().parents[1]
TOOLS = (
    "hook-config",
    "ruff",
    "ruff-format",
    "pyupgrade",
    "yamllint",
    "clang-format",
    "end-of-file-fixer",
    "trailing-whitespace",
    "no-commit-to-branch",
    "ci-custom",
    "clang-tidy",
)


def run(command, **kwargs):
    return subprocess.run(
        command, capture_output=True, text=True, check=False, cwd=ROOT, **kwargs
    )


def python_module(module, *args):
    return [sys.executable, "-m", module, *map(str, args)]


def selected_files(config):
    from identify.identify import tags_from_path

    result = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    )
    exclude = re.compile(config["exclude"])
    files = {}
    for name in sorted(set(result.decode().split("\0")) - {""}):
        path = ROOT / name
        if path.is_file() and not path.is_symlink() and not exclude.search(name):
            files[name] = tags_from_path(str(path))
    return files


def mutation_report(module, args, files, label):
    """Run an upstream fixer on copies and show each proposed diff."""
    count = 0
    with tempfile.TemporaryDirectory(prefix="espcars-lint-") as directory:
        copies = []
        for name in files:
            target = Path(directory) / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / name).read_bytes())
            copies.append(str(target))
        result = run(python_module(module, *args, *copies))
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stdout + result.stderr)
        # A missing module also exits 1; do not misreport it as a clean run.
        if "No module named" in result.stderr or "Traceback" in result.stderr:
            raise RuntimeError(result.stderr)
        for name in files:
            before = (ROOT / name).read_bytes()
            after = (Path(directory) / name).read_bytes()
            if before == after:
                continue
            count += 1
            print(f"{name}:1: [{label}] would modify file")
            print(
                "".join(
                    difflib.unified_diff(
                        before.decode().splitlines(keepends=True),
                        after.decode().splitlines(keepends=True),
                        fromfile=name,
                        tofile=name + " (proposed)",
                    )
                ),
                end="",
            )
        if result.returncode and not count:
            raise RuntimeError(result.stdout + result.stderr)
    return count


def report(tool, files, config):
    python_files = [name for name, tags in files.items() if "python" in tags]
    text_files = [name for name, tags in files.items() if "text" in tags]
    if tool == "hook-config":
        # Even validation creates a prek log; keep it out of shared user caches.
        with tempfile.TemporaryDirectory(prefix="espcars-prek-") as cache:
            result = run(
                python_module("prek", "validate-config", ".pre-commit-config.yaml"),
                env={**os.environ, "PREK_HOME": cache},
            )
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return 0
    if tool in ("ruff", "ruff-format"):
        action = ["check"] if tool == "ruff" else ["format", "--check"]
        result = run(
            python_module("ruff", *action, "--output-format=json", *python_files)
        )
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stderr)
        findings = json.loads(result.stdout)
        for finding in findings:
            name = Path(finding["filename"]).relative_to(ROOT)
            print(
                f"{name}:{finding['location']['row']}: [{finding['code']}] {finding['message']}"
            )
        print(
            "  by area:",
            dict(
                Counter(
                    Path(f["filename"]).relative_to(ROOT).parts[0] for f in findings
                )
            ),
        )
        return len(findings)
    if tool == "pyupgrade":
        return mutation_report("pyupgrade", ["--py312-plus"], python_files, tool)
    if tool == "yamllint":
        yaml_files = [
            name
            for name, tags in files.items()
            if "yaml" in tags and name not in (".clang-format", ".clang-tidy")
        ]
        result = run(python_module("yamllint", "--format", "parsable", *yaml_files))
        if result.returncode not in (0, 1):
            raise RuntimeError(result.stderr)
        print(result.stdout, end="")
        if result.stderr:
            raise RuntimeError(result.stderr)
        return len(result.stdout.splitlines())
    if tool == "clang-format":
        from xml.etree import ElementTree as ET

        count = 0
        for name, tags in files.items():
            if not tags.intersection({"c", "c++"}):
                continue
            result = run(
                [
                    str(Path(sys.executable).parent / "clang-format"),
                    "--output-replacements-xml",
                    name,
                ]
            )
            if result.returncode:
                raise RuntimeError(result.stderr)
            replacements = ET.fromstring(result.stdout).findall("replacement")
            if replacements:
                print(f"{name}:1: [clang-format] {len(replacements)} replacements")
                count += len(replacements)
        return count
    if tool in ("end-of-file-fixer", "trailing-whitespace"):
        module = {
            "end-of-file-fixer": "pre_commit_hooks.end_of_file_fixer",
            "trailing-whitespace": "pre_commit_hooks.trailing_whitespace_fixer",
        }[tool]
        return mutation_report(module, [], text_files, tool)
    if tool == "no-commit-to-branch":
        hook = next(
            hook
            for repo in config["repos"]
            for hook in repo["hooks"]
            if hook["id"] == tool
        )
        result = run(
            python_module("pre_commit_hooks.no_commit_to_branch", *hook["args"])
        )
        if result.stderr or result.returncode not in (0, 1):
            raise RuntimeError(result.stdout + result.stderr)
        if result.returncode:
            print(".git/HEAD:1: [no-commit-to-branch] protected branch")
        return result.returncode
    if tool == "clang-tidy":
        result = run([sys.executable, "script/clang_tidy.py", "--report-only"])
        print(result.stdout, end="")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return int(re.search(r"  TOTAL: (\d+)", result.stdout)[1])
    result = run([sys.executable, "script/ci_custom.py"])
    print(result.stdout, end="")
    if result.returncode not in (0, 1) or result.stderr:
        raise RuntimeError(result.stderr)
    return int(re.search(r"  TOTAL: (\d+)", result.stdout)[1])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=TOOLS, nargs="+")
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="Exit 0 on findings, but still fail on tool errors",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="Print only per-tool counts and full tool errors",
    )
    args = parser.parse_args(argv)
    mode = "report-only" if args.report_only else "blocking"
    config = yaml.safe_load(
        (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
    )
    files = selected_files(config)
    counts = {}
    # This file is the version authority for local runs and CI installation.
    pins = (ROOT / "requirements_lint.txt").read_text(encoding="utf-8")
    for package, expected in re.findall(r"^([\w-]+)==([\w.]+)$", pins, re.MULTILINE):
        try:
            actual = version(package)
            if actual != expected:
                print(f"ERROR [version-{package}]: expected {expected}, found {actual}")
                counts[f"version-{package}"] = None
            elif not args.summary:
                print(f"{package}: {actual} (pinned {expected})")
        except PackageNotFoundError as error:
            print(f"ERROR [version-{package}]: {error}")
            counts[f"version-{package}"] = None
    for tool in args.only or TOOLS:
        if tool == "clang-tidy":
            if __package__:
                from .clang_tidy import has_local_builds
            else:
                from clang_tidy import has_local_builds

            if not has_local_builds():
                counts[tool] = (
                    "SKIPPED (no local compile_commands.json; run esphome compile)"
                )
                print(f"SKIP [clang-tidy]: {counts[tool]}")
                continue
        if not args.summary:
            print(f"== {mode}: {tool} ==", flush=True)
        details = io.StringIO()
        try:
            with redirect_stdout(details) if args.summary else nullcontext():
                counts[tool] = report(tool, files, config)
        except (OSError, ValueError, RuntimeError) as error:
            if args.summary:
                print(details.getvalue(), end="")
            print(f"ERROR [{tool}]: {error}")
            counts[tool] = None
    print(f"Upstream lint summary ({mode}; units described in script/lint_report.py):")
    for tool, count in counts.items():
        print(f"  {tool}: {count if count is not None else 'ERROR (no count)'}")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a", encoding="utf-8") as output:
            output.write("\n### Upstream lint counts\n\n| Tool | Count |\n|---|---:|\n")
            for tool, count in counts.items():
                value = count if count is not None else "ERROR (no count)"
                output.write(f"| {tool} | {value} |\n")
            output.write("\nCount units are documented in `script/lint_report.py`.\n")
    if None in counts.values():
        return 2
    return (
        0
        if args.report_only
        else int(any(isinstance(count, int) and count > 0 for count in counts.values()))
    )


if __name__ == "__main__":
    sys.exit(main())
