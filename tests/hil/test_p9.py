"""P9 host tools: fictional evidence, fake builds, no board or socket access."""

from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import sys
from unittest.mock import Mock

import pytest

SCRIPTS = Path(__file__).resolve().parents[2] / "script/hil"


def load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


build = load("p9_build")
measure = load("p9_measure")
compare = load("p9_compare")


def test_fictional_log_parses_existing_instruments():
    text = (Path(__file__).parent / "fixtures/p9-fictional.log").read_text(
        encoding="utf-8"
    )
    result = measure.parse_log(text, 40, "green", ring=True)
    metrics = result["metrics"]
    assert metrics["ring.laps_per_s"] == 20
    assert metrics["heap.setup.free_bytes"] == 180000
    assert metrics["heap.load.largest_bytes"] == 115000
    assert metrics["heap.load.min_bytes"] == 145000
    assert metrics["loop.period_mean_us"] == 1000
    assert metrics["loop.max_period_us"] == 2200
    assert metrics["loop.max_blocking_operation_us"] == 61000
    assert metrics["loop.max_iteration_us"] == 2100
    assert metrics["task.loopTask.cpu_percent"] == 11.2
    assert metrics["cpu.busy_percent"] == 23.5
    assert metrics["slots.in_use"] == 2
    assert metrics["can0.0x150.period_mean_us"] == 10005
    assert metrics["lin.selftest.0x30.max_ms"] == 51
    assert result["verify"]["pass"]
    assert "cyclic.jitter_us" in result["missing"]
    assert "gateway.rx_isr_to_forward_us" in result["missing"]


def test_empty_capture_and_counter_reset_are_not_passes():
    empty = measure.parse_log("", 45, "green", ring=True)
    assert not empty["verify"]["pass"]
    assert "ring.laps_per_s" in empty["missing"]
    reset = measure.parse_log(
        "RING lap 100 closed\nRING lap 25 closed", 45, "green", ring=True
    )
    assert "ring.laps_per_s" not in reset["metrics"]


def test_runtime_totals_do_not_contaminate_load_windows():
    result = measure.parse_log(
        "Period stats (last 30000ms): 4 active components\n"
        "main_loop: iters=30000, active_max=1.25ms\n"
        "Total stats (since boot): 4 active components\n"
        "main_loop: iters=40000, active_max=2000.00ms\n",
        90,
        "orange",
    )
    assert result["metrics"]["loop.max_iteration_us"] == 1250


def test_errors_include_lifetime_and_all_ports():
    text = """Port 0 stats: bus_err 0, err_events 0, tx_fail 0, TEC 0, REC 0
Port 1 stats: bus_err 2, err_events 3, tx_fail 4, TEC 5, REC 6
Checksum Errors: 0 | 7
Send Failures: 1 | 2
TOKEN LOST
CAN bus-off
LIN error: fictional
records=20 dropped=8 tap_dropped=9
"""
    result = measure.parse_log(text, 45, "orange")
    assert result["metrics"]["errors.can1.TEC"] == 5
    assert result["metrics"]["errors.can0.TEC"] == 0
    assert result["metrics"]["errors.lin.checksum_errors"] == 7
    assert result["metrics"]["errors.sd.tap_dropped"] == 9
    assert result["metrics"]["errors.events"] == 3


@pytest.mark.parametrize(
    ("metric", "ref", "now", "expected"),
    [
        ("ring.laps_per_s", 20, 19.9, "FAIL"),
        ("ring.laps_per_s", 19, 19.39, "FAIL"),
        ("ring.laps_per_s", 19.4, 19.4, "PASS"),
        ("ring.laps", 800, 74, "FAIL"),
        ("errors.lin.checksum_errors", 0, 1, "FAIL"),
        ("errors.events", 1, 0, "FAIL"),
        ("errors.events", 0, 0, "PASS"),
        ("heap.setup.free_bytes", 100, 95, "PASS"),
        ("heap.setup.free_bytes", 100, 94.99, "REVIEW"),
        ("loop.max_period_us", 100, 106, "REVIEW"),
        ("slots.in_use", 0, 1, "REVIEW"),
        ("loop.max_iteration_us", None, 0, "REVIEW"),
        ("loop.period_mean_us", 0, float("nan"), "REVIEW"),
    ],
)
def test_comparison_limits(metric, ref, now, expected):
    assert compare.verdict(metric, ref, now)[0] == expected


def test_missing_or_mismatched_evidence_is_review():
    record = {
        "sha": "fictional",
        "yaml_sha256": "yaml",
        "tool_version": "test",
        "seconds": 45,
        "runs": {},
    }
    markdown, code = compare.compare(record, record)
    assert code == 2 and "REVIEW" in markdown
    record["runs"] = {"ring-green": {"metrics": {"errors.events": 0}, "missing": {}}}
    markdown, code = compare.compare(record, record)
    assert code == 2 and "ring.laps_per_s" in markdown
    assert "board_mac" in markdown


def test_mac_selection_ignores_names_and_port_order(monkeypatch):
    monkeypatch.setattr(measure, "KNOWN_MACS", {"02:00:00:00:00:01": "green"})
    boards = [
        {"name": "green", "mac": "02:00:00:00:00:99", "port": "fictional-a"},
        {"name": "wrong-name", "mac": "02:00:00:00:00:01", "port": "fictional-b"},
    ]
    assert measure.select_board("green", boards)["port"] == "fictional-b"
    with pytest.raises(ValueError, match="found 0"):
        measure.select_board("orange", boards)
    with pytest.raises(ValueError, match="found 2"):
        measure.select_board("green", [boards[1], boards[1]])


def test_capture_deasserts_lines_before_open(monkeypatch):
    # Exercise the shared verify.py capture used by P9, including attach order.
    events = []

    class Serial:
        def __setattr__(self, name, value):
            events.append((name, value))
            object.__setattr__(self, name, value)

        def open(self):
            assert self.dtr is False and self.rts is False
            events.append("open")

        def close(self):
            events.append("close")

    monkeypatch.setattr(sys.modules["verify"].serial, "Serial", Serial)
    out = {}
    measure.capture("fictional-port", 0, out, "role")
    assert events.index(("dtr", False)) < events.index("open")
    assert events.index(("rts", False)) < events.index("open")
    assert events[-1] == "close"


@pytest.mark.parametrize("app_name", ["firmware.bin", "firmware.ota.bin"])
def test_fake_ref_build_uses_identical_yaml_and_unique_directories(
    tmp_path, monkeypatch, app_name
):
    repo = tmp_path / "repo"
    source = repo / "tests/hil"
    source.mkdir(parents=True)
    (source / "p9-ring-green.yaml").write_text("esphome: {}\n", encoding="utf-8")
    (source / "p9-profile-green.yaml").write_text("esphome: {}\n", encoding="utf-8")
    (source / "secrets.yaml").write_text("private: never-copy\n", encoding="utf-8")
    secrets = tmp_path / "wifi.yaml"
    secrets.write_text("wifi_ssid: fictional\n", encoding="utf-8")
    monkeypatch.setattr(
        build,
        "ROLES",
        {
            "ring-green": ("green", "p9-ring-green.yaml"),
            "profile-green": ("green", "p9-profile-green.yaml"),
        },
    )
    trees, configs, dirs = [], [], []

    @contextmanager
    def fake_worktree(_repo, sha):
        tree = tmp_path / f"tree-{len(trees)}"
        trees.append((tree, sha))
        (tree / "tests/hil").mkdir(parents=True)
        yield tree

    def fake_run(command, *, cwd, log=None):
        if command[:2] == ["git", "rev-parse"]:
            return {
                "fake-ref^{commit}": "f" * 40,
                "HEAD": "a" * 40,
                build.REFERENCE: "b" * 40,
            }[command[-1]]
        if "version" in command:
            return "fictional-esphome"
        if "config" in command:
            configs.append(Path(command[-1]).read_bytes())
        if "compile" in command:
            import yaml

            wrapper = yaml.safe_load(
                Path(command[-1]).read_text(encoding="utf-8").replace("!include ", "")
            )
            directory = Path(wrapper["esphome"]["build_path"])
            dirs.append(directory)
            directory.mkdir(parents=True)
            (directory / app_name).write_bytes(b"fictional-app")
            (directory / "firmware.factory.bin").write_bytes(b"fictional-factory")
        return ""

    monkeypatch.setattr(build, "worktree", fake_worktree)
    monkeypatch.setattr(build, "run", fake_run)
    path = build.build("fake-ref", tmp_path / "images", repo=repo, secrets=secrets)
    result = json.loads(path.read_text(encoding="utf-8"))
    assert result["sha"] == "f" * 40
    assert len(configs) == 6 and len(set(configs)) == 1
    assert dirs[0].name == "ring-green"
    assert len(dirs) == len(set(dirs)) == 2
    assert not (path.parent / "yaml/secrets.yaml").exists()
    image = result["images"][0]
    assert image["sha256"] == build.sha256(Path(image["image"]))
    assert image["flash_sha256"] == build.sha256(Path(image["flash_image"]))
    assert {sha for _, sha in trees} == {"a" * 40, "b" * 40, "f" * 40}
    with pytest.raises(ValueError, match="empty"):
        build.build("fake-ref", path.parent, repo=repo, secrets=secrets)


def test_measure_never_flashes_without_flag(tmp_path, monkeypatch):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "ref": "fake",
                "sha": "fake-sha",
                "yaml_sha256": "yaml",
                "tool_version": "fictional",
                "images": [{"role": "ring-blue", "board": "blue", "sha256": "fake"}],
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(measure, "KNOWN_MACS", {"02:00:00:00:00:02": "blue"})
    monkeypatch.setattr(
        measure,
        "enumerate_boards",
        lambda: [{"mac": "02:00:00:00:00:02", "port": "fictional"}],
    )
    flash = Mock()
    monkeypatch.setattr(measure, "flash_image", flash)
    monkeypatch.setattr(
        measure,
        "capture",
        lambda _p, _s, out, key: out.update({key: "RING relay 25\nRING ack 25"}),
    )
    result = measure.measure(manifest, ["ring-blue"], 45)
    flash.assert_not_called()
    assert result["runs"]["ring-blue"]["verify"]["pass"]
    measure.measure(manifest, ["ring-blue"], 45, flash=True)
    flash.assert_called_once()


def test_tampered_image_is_rejected_before_board_access(tmp_path, monkeypatch):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"fictional")
    boards = Mock()
    monkeypatch.setattr(measure, "enumerate_boards", boards)
    with pytest.raises(ValueError, match="checksum mismatch"):
        measure.flash_image(
            {"role": "ring-green", "image": str(image), "sha256": "wrong"},
            tmp_path / "manifest.json",
        )
    boards.assert_not_called()


def test_whole_rig_is_identified_before_first_flash(tmp_path, monkeypatch):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "images": [
                    {"role": "ring-green", "board": "green"},
                    {"role": "ring-blue", "board": "blue"},
                    {"role": "ring-orange", "board": "orange"},
                ]
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(measure, "KNOWN_MACS", {"02:00:00:00:00:01": "green"})
    monkeypatch.setattr(
        measure,
        "enumerate_boards",
        lambda: [{"mac": "02:00:00:00:00:01", "port": "fictional"}],
    )
    flash = Mock()
    monkeypatch.setattr(measure, "flash_image", flash)
    with pytest.raises(ValueError, match="blue"):
        measure.measure(manifest, ["ring"], 45, flash=True)
    flash.assert_not_called()
