"""Guard the report-only wiring, shared pins and byte-exact exclusions."""

from pathlib import Path
import re
import subprocess

import pytest
import yaml

from script import lint_report

ROOT = Path(__file__).resolve().parents[2]


def test_hook_revisions_match_shared_pins():
    pins = dict(
        re.findall(
            r"^([\w-]+)==([\w.]+)$",
            (ROOT / "requirements_lint.txt").read_text(encoding="utf-8"),
            re.MULTILINE,
        )
    )
    config = yaml.safe_load(
        (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
    )
    packages = ["ruff", "pre-commit-hooks", "pyupgrade", "yamllint", "clang-format"]
    for repo, package in zip(config["repos"], packages, strict=False):
        assert repo["rev"] == "v" + pins[package]
    hooks = {hook["id"]: hook for repo in config["repos"] for hook in repo["hooks"]}
    assert hooks["ruff"]["args"] == ["--fix"]
    assert hooks["pyupgrade"]["args"] == ["--py312-plus"]


@pytest.mark.parametrize(
    "name",
    [
        "tests/uds/fixtures/mini.dcat",
        "tests/uds/fixtures/mini.manifest.json",
        "tests/sd_logger/fixtures/mini.dbc",
        "tests/portal/fixtures/envelopes/basic.json",
        "tests/portal/page/healthy.json",
        "components/portal/index.html",
        "tests/build/portal/custom_page.html",
    ],
)
def test_byte_exact_files_excluded(name):
    config = yaml.safe_load(
        (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
    )
    assert re.search(config["exclude"], name)
    assert not re.search(
        config["exclude"], "tests/portal/fixtures/ext/portal_demo_providers/__init__.py"
    )


def test_ci_job_stays_advisory():
    workflow = yaml.safe_load(
        (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    )
    jobs = workflow["jobs"]
    assert not jobs["upstream-lint"].get("continue-on-error", False)
    assert "upstream-lint" not in jobs["required-checks"]["needs"]
    assert any(
        step.get("run") == "python script/lint_report.py --report-only"
        for step in jobs["upstream-lint"]["steps"]
    )
    check_script = (ROOT / "script/check.sh").read_text(encoding="utf-8")
    assert '"$PY" script/lint_report.py --report-only --summary\n' in check_script
    assert (
        'echo "For details, run: .venv/bin/python script/lint_report.py"'
        in check_script
    )


def test_mutating_tools_only_receive_temporary_copies(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(lint_report, "ROOT", tmp_path)
    source = tmp_path / "example.py"
    original = b"value = old_syntax\n"
    source.write_bytes(original)

    def fake_fixer(command):
        copy = Path(command[-1])
        assert copy != source
        copy.write_bytes(b"value = new_syntax\n")
        return subprocess.CompletedProcess(command, 1, "", "")

    monkeypatch.setattr(lint_report, "run", fake_fixer)
    assert (
        lint_report.mutation_report("pyupgrade", [], ["example.py"], "pyupgrade") == 1
    )
    assert source.read_bytes() == original
    assert "[pyupgrade] would modify file" in capsys.readouterr().out


def test_missing_tool_is_an_error(tmp_path, monkeypatch):
    monkeypatch.setattr(lint_report, "ROOT", tmp_path)
    (tmp_path / "example.py").write_text("", encoding="utf-8")
    monkeypatch.setattr(
        lint_report,
        "run",
        lambda command: subprocess.CompletedProcess(
            command, 1, "", "No module named pyupgrade"
        ),
    )
    with pytest.raises(RuntimeError, match="No module named"):
        lint_report.mutation_report("pyupgrade", [], ["example.py"], "pyupgrade")


@pytest.fixture
def report_environment(monkeypatch):
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    monkeypatch.setattr(lint_report, "selected_files", lambda config: {})
    monkeypatch.setattr(
        lint_report,
        "version",
        lambda package: {
            "ruff": "0.17.5",
            "pyupgrade": "3.22.0",
            "yamllint": "1.38.0",
            "prek": "0.5.11",
            "pre-commit-hooks": "5.0.0",
            "identify": "2.6.20",
            "clang-format": "13.0.1",
        }[package],
    )


def test_report_continues_after_tool_error(report_environment, monkeypatch, capsys):
    def fake_report(tool, files, config):
        if tool == "ruff":
            raise RuntimeError("unavailable")
        return 3

    monkeypatch.setattr(lint_report, "report", fake_report)
    assert lint_report.main(["--only", "ruff", "pyupgrade"]) == 2
    output = capsys.readouterr().out
    assert "ruff: ERROR (no count)" in output
    assert "pyupgrade: 3" in output


@pytest.mark.parametrize("report_only", [False, True])
@pytest.mark.parametrize("summary", [False, True])
@pytest.mark.parametrize("findings", [0, 3, None])
def test_report_only_exit_codes(
    report_environment, monkeypatch, report_only, summary, findings
):
    def fake_report(tool, files, config):
        if findings is None:
            raise RuntimeError("tool crashed")
        return findings

    monkeypatch.setattr(lint_report, "report", fake_report)
    args = ["--only", "ruff"]
    if report_only:
        args.append("--report-only")
    if summary:
        args.append("--summary")
    expected = 2 if findings is None else int(bool(findings) and not report_only)
    assert lint_report.main(args) == expected


@pytest.mark.parametrize("summary", [False, True])
def test_summary_hides_details_only(report_environment, monkeypatch, capsys, summary):
    def fake_report(tool, files, config):
        print("example.py:1: [ruff] finding details")
        return 3

    monkeypatch.setattr(lint_report, "report", fake_report)
    args = ["--report-only", "--only", "ruff"]
    if summary:
        args.append("--summary")
    assert lint_report.main(args) == 0
    output = capsys.readouterr().out
    assert "  ruff: 3\n" in output
    assert ("finding details" in output) is not summary
    if summary:
        assert output.splitlines() == [
            "P1a lint summary (report-only; units described in script/lint_report.py):",
            "  ruff: 3",
        ]


def test_summary_preserves_full_errors(report_environment, monkeypatch, capsys):
    def fake_report(tool, files, config):
        print("tool stdout before crash")
        raise RuntimeError("tool crashed\nfull error details\nlast error line")

    monkeypatch.setattr(lint_report, "report", fake_report)
    assert lint_report.main(["--report-only", "--summary", "--only", "ruff"]) == 2
    output = capsys.readouterr().out
    assert "tool stdout before crash\n" in output
    assert "ERROR [ruff]: tool crashed\nfull error details\nlast error line\n" in output
    assert "  ruff: ERROR (no count)\n" in output


def test_summary_preserves_version_errors(report_environment, monkeypatch, capsys):
    monkeypatch.setattr(lint_report, "version", lambda package: "wrong-version")
    monkeypatch.setattr(lint_report, "report", lambda *args: 0)
    assert lint_report.main(["--report-only", "--summary", "--only", "ruff"]) == 2
    assert (
        "ERROR [version-ruff]: expected 0.17.5, found wrong-version\n"
        in capsys.readouterr().out
    )


@pytest.mark.parametrize("summary_only", [False, True])
@pytest.mark.parametrize("tool_error", [False, True])
def test_github_summary_appends_counts(
    report_environment, tmp_path, monkeypatch, tool_error, summary_only
):
    summary = tmp_path / "summary.md"
    summary.write_text("Existing summary\n", encoding="utf-8")
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))

    def fake_report(tool, files, config):
        if tool == "ruff" and tool_error:
            raise RuntimeError("No module named ruff")
        return 3 if tool == "ruff" else 0

    monkeypatch.setattr(lint_report, "report", fake_report)
    args = ["--report-only", "--only", "ruff", "pyupgrade"]
    if summary_only:
        args.append("--summary")
    assert lint_report.main(args) == (2 if tool_error else 0)
    content = summary.read_text(encoding="utf-8")
    assert content.startswith("Existing summary\n")
    assert "| Tool | Count |\n|---|---:|\n" in content
    assert f"| ruff | {'ERROR (no count)' if tool_error else 3} |\n" in content
    assert "| pyupgrade | 0 |\n" in content


def test_check_shell_syntax():
    subprocess.run(["bash", "-n", str(ROOT / "script/check.sh")], check=True)
