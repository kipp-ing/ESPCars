"""Composition, target coverage and command dispatch regression tests."""

import hashlib
import json
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
    stamps = []
    monkeypatch.setattr(runner, "write_build_stamp", lambda *args: stamps.append(args))
    target = runner.list_targets()[0]

    def invoke(args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, code)

    monkeypatch.setattr(runner.subprocess, "run", invoke)
    options = ["--only-generate"] if command == "compile" else []
    assert runner.main([command, target, *options]) == code
    assert len(calls) == 1
    assert len(stamps) == int(command == "compile" and code == 0)
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


@pytest.mark.parametrize("only_generate", [False, True])
@pytest.mark.parametrize("code", [0, 7])
@pytest.mark.parametrize("existing", [False, True])
def test_compile_writes_stamp_only_on_success(
    tmp_path, monkeypatch, only_generate, code, existing
):
    target = "tests/components/demo/test.esp32-idf.yaml"
    component = tmp_path / "components/demo"
    component.mkdir(parents=True)
    source = component / "__init__.py"
    source.write_text("# codegen\n", encoding="utf-8")
    subprocess.run(["git", "init", "--quiet", str(tmp_path)], check=True)
    subprocess.run(["git", "add", "components"], cwd=tmp_path, check=True)
    directory = tmp_path / ".esphome/build/demo-esp32"
    (directory / "src/esphome/components/demo").mkdir(parents=True)
    stamp = directory / runner.BUILD_STAMP
    if existing:
        stamp.write_text("old stamp\n", encoding="utf-8")
    monkeypatch.setattr(runner, "ROOT", tmp_path)
    monkeypatch.setattr(runner, "compose", lambda target: tmp_path / "generated.yaml")
    invoke = subprocess.run

    def run(args, **kwargs):
        if args[1:3] == ["-m", "esphome"]:
            assert (args[-1] == "--only-generate") == only_generate
            # Stamping must happen after a successful subprocess.
            assert stamp.exists() == existing
            if existing:
                assert stamp.read_text(encoding="utf-8") == "old stamp\n"
            return subprocess.CompletedProcess(args, code)
        return invoke(args, **kwargs)

    monkeypatch.setattr(runner.subprocess, "run", run)
    args = ["compile", target, *(["--only-generate"] if only_generate else [])]
    assert runner.main(args) == code
    if code:
        assert stamp.exists() == existing
        if existing:
            assert stamp.read_text(encoding="utf-8") == "old stamp\n"
    else:
        assert json.loads(stamp.read_text(encoding="utf-8")) == {
            "target": target,
            "files": {
                "components/demo/__init__.py": hashlib.sha256(
                    source.read_bytes()
                ).hexdigest()
            },
        }


def test_stamp_hashes_all_tracked_active_component_sources(tmp_path):
    source_root = tmp_path / "build/src"
    for name in ("demo", "dependency"):
        (source_root / "esphome/components" / name).mkdir(parents=True)
        (tmp_path / "components" / name).mkdir(parents=True)
    expected = {}
    for suffix in runner.COMPONENT_SOURCE_EXTENSIONS | {".py"}:
        relative = "components/demo/nested/source" + suffix
        path = tmp_path / relative
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(b"source bytes")
        expected[relative] = hashlib.sha256(b"source bytes").hexdigest()
    dependency = "components/dependency/__init__.py"
    (tmp_path / dependency).write_bytes(b"dependency bytes")
    expected[dependency] = hashlib.sha256(b"dependency bytes").hexdigest()
    (tmp_path / "components/demo/README.md").write_text(
        "not a source", encoding="utf-8"
    )
    inactive = tmp_path / "components/inactive/unused.cpp"
    inactive.parent.mkdir()
    inactive.write_text("not used", encoding="utf-8")
    subprocess.run(["git", "init", "--quiet", str(tmp_path)], check=True)
    subprocess.run(["git", "add", "components"], cwd=tmp_path, check=True)
    (tmp_path / "components/demo/untracked.py").write_text(
        "not tracked", encoding="utf-8"
    )
    assert runner.component_hashes(source_root, root=tmp_path) == expected
