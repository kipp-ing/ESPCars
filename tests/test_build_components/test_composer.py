"""Composition, target coverage and command dispatch regression tests."""

from pathlib import Path
import subprocess

import pytest
import yaml

from script import clang_tidy as tidy, test_build_components as runner

ROOT = Path(__file__).resolve().parents[2]


def test_ci_coverage_and_unique_builds():
    jobs = yaml.safe_load(
        (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    )["jobs"]
    matrix = jobs["compile"]["strategy"]["matrix"]["config"]
    compiled = runner.list_targets()
    all_targets = runner.list_targets(include_validate=True)
    assert len(matrix) == 17
    assert len(compiled) == 18
    assert len(all_targets) == 21
    assert set(compiled) == set(matrix) | set(tidy.DEFAULT_TARGETS)
    assert len({runner.node_name(target) for target in all_targets}) == len(all_targets)
    assert len({runner.build_path(target) for target in all_targets}) == len(
        all_targets
    )
    assert all(len(runner.node_name(target)) <= 31 for target in all_targets)
    assert {Path(target).parent.name for target in tidy.DEFAULT_TARGETS} == {
        "can_gateway",
        "ccp",
        "isotp",
        "linbus",
        "portal",
        "sd_logger",
        "uds",
    }


def test_legacy_mapping_is_bijective():
    mapping = ROOT / "tests/test_build_components/legacy_map.txt"
    if not mapping.is_file():
        pytest.skip("Temporary mapping removed after P6 codegen verification")
    lines = mapping.read_text(encoding="utf-8").splitlines()
    assert lines[0].startswith("# Temporary")
    pairs = [line.split("\t") for line in lines[1:] if line]
    assert len(pairs) == 21
    assert len({old for old, _ in pairs}) == 21
    assert {new for _, new in pairs} == set(runner.list_targets(include_validate=True))


@pytest.mark.parametrize("target", runner.list_targets(include_validate=True))
def test_composition_resolves_platform_and_build(target):
    from esphome.config import read_config
    from esphome.core import CORE

    output = runner.compose(target)
    content = output.read_text(encoding="utf-8")
    assert runner.compose(target).read_text(encoding="utf-8") == content
    CORE.reset()
    CORE.config_path = output
    config = read_config({})
    assert config is not None
    assert Path(CORE.build_path) == runner.build_path(target)
    assert config["esphome"]["name"] == runner.node_name(target)
    assert config["esp32"]["framework"]["type"] == "esp-idf"
    assert str(config["logger"]["level"]) == "DEBUG"
    assert not config["esphome"].get("friendly_name")


def test_locate_database_uses_exact_composed_target(monkeypatch, tmp_path):
    from esphome.config import read_config
    from esphome.core import CORE

    first, second = runner.list_targets()[:2]
    # Isolate build output while retaining the real composition and validation.
    monkeypatch.setattr(
        runner,
        "build_path",
        lambda target, **kwargs: tmp_path / runner.node_name(target),
    )
    for target in (first, second):
        CORE.reset()
        CORE.config_path = runner.compose(target)
        assert read_config({}) is not None
    expected = runner.build_path(second) / "build/compile_commands.json"
    expected.parent.mkdir(parents=True)
    expected.write_text("[]", encoding="utf-8")
    assert tidy.locate_database(second) == expected
    with pytest.raises(tidy.BuildUnavailable, match="test_build_components.py compile"):
        tidy.locate_database(first)


@pytest.mark.parametrize("command", ["config", "compile"])
@pytest.mark.parametrize("code", [0, 7])
def test_dispatch_and_failure_propagation(monkeypatch, command, code):
    calls = []
    target = runner.list_targets()[0]

    def invoke(args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, code)

    monkeypatch.setattr(runner.subprocess, "run", invoke)
    options = ["--only-generate"] if command == "compile" else []
    assert runner.main([command, target, *options]) == code
    assert len(calls) == 1
    assert calls[0][1:4] == ["-m", "esphome", command]
    assert Path(calls[0][4]) == runner.compose(target)
    assert (calls[0][-1] == "--only-generate") == (command == "compile")


@pytest.mark.parametrize("extra", [[], ["--only-generate"]])
def test_validate_fixtures_cannot_compile(extra):
    target = next(
        target
        for target in runner.list_targets(include_validate=True)
        if "/validate" in target
    )
    with pytest.raises(SystemExit) as error:
        runner.main(["compile", target, *extra])
    assert error.value.code == 2


def test_list_targets_modes(capsys):
    assert runner.main(["--list-targets"]) == 0
    assert capsys.readouterr().out.splitlines() == runner.list_targets()
    assert runner.main(["config", "--list-targets"]) == 0
    assert capsys.readouterr().out.splitlines() == runner.list_targets(
        include_validate=True
    )


@pytest.mark.parametrize(
    "target",
    [
        "../test.esp32-idf.yaml",
        "tests/components/demo/test.host.yaml",
        "tests/components/../test.esp32-idf.yaml",
    ],
)
def test_invalid_targets_fail(target):
    with pytest.raises(SystemExit) as error:
        runner.main(["config", target])
    assert error.value.code == 2


def test_long_names_are_distinct_and_valid():
    first = (
        "tests/components/long_component_name/test-very-long-variant.esp32-c6-idf.yaml"
    )
    second = first.replace("variant", "other")
    assert len(runner.node_name(first)) == 31
    assert runner.node_name(first) != runner.node_name(second)
