"""script/sdlog_publish.py — the pipeline's state in Home Assistant (docs/sdlog-publish-spec.md).

Against a FakeSink-backed source and a fake MQTT client: no broker, no database.
"""

from __future__ import annotations

import ast
import importlib.util
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import pytest

pytest.importorskip("cantools")

_SCRIPT = Path(__file__).parent.parent.parent / "script"
MINI_DBC = Path(__file__).parent / "fixtures" / "mini.dbc"


def _load(name: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, _SCRIPT / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


dec = _load("sdlog_decode")
pub = _load("sdlog_publish")

# Same fictional chunk as test_decode.py: two #utc anchors, so every frame after the
# first has a wall time. Newest utc is 1999821 µs (the 0x7FF frame).
CHUNK = [
    "#sdlog,2,7,@0,2026.7.0",
    "#origin,mr-orange",
    "#src,C,EV,1,can_gateway,500000",
    "C1,@1000,100,E803F60300000000",
    "#utc,1010,F4240",
    "C1,20,200,0FA0FF9C00000000",
    "C1,7,100,D007F60300000000",
    "C1s,8,300,00D007",
    "#utc,1100,1E8480",
    "C1,9,300,01B80B",
    "C1,A,7FF,",
    "#close,@2000,rotate",
]
NOW = datetime(1970, 1, 3, tzinfo=timezone.utc)  # gaps window: everything in 1970-01-01 counts


class FakeClient:
    def __init__(self) -> None:
        self.published: list[tuple[str, str, bool]] = []
        self.subscribed: list[str] = []

    def publish(self, topic, payload, qos=0, retain=False):
        self.published.append((topic, payload, retain))

    def subscribe(self, topic, qos=0):
        self.subscribed.append(topic)


@pytest.fixture
def env(tmp_path: Path):
    archive = tmp_path / "archive"
    (archive / "mr-orange").mkdir(parents=True)
    (archive / "mr-orange" / "L0000007.LOG").write_bytes(("\n".join(CHUNK) + "\n").encode("latin-1"))
    dbc = tmp_path / "mini.dbc"
    dbc.write_bytes(MINI_DBC.read_bytes())
    decode_toml = tmp_path / "decode.toml"
    decode_toml.write_text(
        f'archive = "{archive}"\nsink = "fake"\n\n[[device]]\nname = "mr-orange"\nvehicle = "demo-car"\n'
        f'[device.dbc]\nEV = "{dbc}"\n'
    )
    fleet = tmp_path / "fleet"
    fleet.mkdir()
    password = tmp_path / "mqtt.pass"
    password.write_text("secret\n")
    publish_toml = tmp_path / "publish.toml"
    publish_toml.write_text(
        f'node = "sdlog"\ndecode_config = "{decode_toml}"\nfleet_state = "{fleet}"\n'
        f'archive_min_free_mb = 1\ntrigger = "{tmp_path}/run/trigger"\n\n'
        f'[mqtt]\nhost = "192.0.2.42"\nuser = "sdlog"\npassword_file = "{password}"\n\n'
        f'[[vehicle]]\nname = "demo-car"\n'
        f'[[vehicle.sensor]]\nkey = "speed"\nname = "Speed"\nsignal = "DEMO_STATUS.Speed"\nunit = "km/h"\n'
        f'[[vehicle.sensor]]\nkey = "cell_b"\nname = "Cell B"\nsignal = "DEMO_MUX.CellB"\nunit = "V"\ndevice_class = "voltage"\n'
        f'[[vehicle.sensor]]\nkey = "nothing"\nname = "Nothing"\nsignal = "DEMO_EXT.Counter"\n'
    )
    sink = dec.FakeSink()
    assert dec.main(["run", "--config", str(decode_toml)], sink=sink) == 0
    config = pub.load_config(publish_toml)
    return config, sink, fleet, tmp_path


def beat(fleet: Path, device: str, **fields) -> None:
    (fleet / f"{device}.json").write_text(json.dumps({"device": device, **fields}))


# ------------------------------------------------------------------ P1 config


@pytest.mark.parametrize(
    "edit, why",
    [
        (lambda t: t + '\nbogus = 1\n', "unknown key"),
        (lambda t: t.replace('password_file = ', 'password = "x"\npassword_file = '), "never inline"),
        (lambda t: t.replace('mqtt.pass"', 'nope.pass"'), "password_file not found"),
        (lambda t: t.replace('name = "demo-car"\n[[vehicle.sensor]]', 'name = "other-car"\n[[vehicle.sensor]]', 1), "not in the decode config"),
        (lambda t: t.replace('key = "cell_b"', 'key = "speed"'), "duplicate sensor key"),
    ],
)
def test_config_rejects(env, capsys, edit, why) -> None:
    _, _, _, tmp = env
    path = tmp / "publish.toml"
    path.write_text(edit(path.read_text()))
    assert pub.main(["once", "--config", str(path)]) == 2
    assert why in capsys.readouterr().err


# ------------------------------------------------------------------ P2/P3 collect


def test_chain_state_per_device(env) -> None:
    config, sink, fleet, _ = env
    beat(fleet, "mr-orange", last_success="2026-10-11T12:05:28.5+00:00", last_result="offline", verdict="ok")
    state = pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    d = state["devices"]["mr-orange"]
    assert d["last_pull"] == "2026-10-11T12:05:28+00:00"
    assert d["result"] == "offline"
    assert (d["ok"], d["defect"], d["pending"]) == (1, 0, 0)
    assert d["last_data"] == "1970-01-01T00:00:01+00:00"  # 1999821 µs
    assert d["gaps_7d"] == 0
    assert state["problem"] == "OFF"


def test_a_fresh_device_is_null_not_error(env) -> None:
    config, _, _, _ = env
    state = pub.collect(config, pub.FakeSinkDb(config.decode, dec.FakeSink()), now=NOW)
    d = state["devices"]["mr-orange"]
    assert d["last_pull"] is None and d["result"] is None and d["last_data"] is None
    assert d["pending"] == 1 and state["problem"] == "OFF"


def test_latest_value_by_utc(env) -> None:
    config, sink, _, _ = env
    state = pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    v = state["vehicles"]["demo-car"]
    # Two Speed frames: 10.0 before the first #utc (no wall time, ignored), 20.0 after it.
    assert v["speed"]["value"] == pytest.approx(20.0)
    assert v["cell_b"]["value"] == pytest.approx(3.0)


def test_missing_signal_is_null(env) -> None:
    config, sink, _, _ = env
    state = pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    assert state["vehicles"]["demo-car"]["nothing"] == {"value": None, "time": None}


# ------------------------------------------------------------------ P4/P5 problem


@pytest.mark.parametrize(
    "case, reasons",
    [
        ("healthy", []),
        ("away", []),
        ("collect_error", ["collect:mr-orange"]),
        ("defect", ["defect:mr-orange"]),
        ("space", ["space"]),
        ("gap", []),
    ],
)
def test_problem_table(env, case, reasons) -> None:
    config, sink, fleet, _ = env
    beat(fleet, "mr-orange", last_result="ok", verdict="ok")
    if case == "away":
        beat(fleet, "mr-orange", last_result="offline", verdict="ok", last_success="2026-01-01T00:00:00+00:00")
    if case == "collect_error":
        beat(fleet, "mr-orange", last_result="error", verdict="error")
    if case == "defect":
        sink.tables["chunks"][0]["result"] = "defect"
    if case == "space":
        config = pub.Config(**{**config.__dict__, "archive_min_free_mb": 1e15})
    if case == "gap":
        sink.tables["chunks"][0]["gaps"] = 2
    state = pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    assert state["reasons"] == reasons
    assert state["problem"] == ("ON" if reasons else "OFF")
    if case == "gap":
        assert state["devices"]["mr-orange"]["gaps_7d"] == 2


def test_clickhouse_down_still_publishes_the_rest(env) -> None:
    config, sink, fleet, _ = env
    beat(fleet, "mr-orange", last_result="ok", verdict="ok", last_success="2026-10-11T12:00:00+00:00")
    sink.unavailable = True
    state = pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    assert state["reasons"] == ["clickhouse"]
    d = state["devices"]["mr-orange"]
    assert d["last_pull"] == "2026-10-11T12:00:00+00:00" and d["ok"] is None
    assert state["vehicles"]["demo-car"]["speed"]["value"] is None
    assert state["archive_free_gb"] > 0


# ------------------------------------------------------------------ P6–P9 MQTT


def test_discovery_payloads(env) -> None:
    config, _, _, _ = env
    entities = dict(pub.discovery(config))
    assert "homeassistant/binary_sensor/sdlog/sdlog_problem/config" in entities
    assert "homeassistant/sensor/sdlog/sdlog_mr_orange_last_pull/config" in entities
    assert "homeassistant/sensor/sdlog/sdlog_demo_car_cell_b/config" in entities
    assert "homeassistant/button/sdlog/sdlog_collect_now/config" in entities
    for topic, body in entities.items():
        assert body["unique_id"] and body["availability_topic"] == "sdlog/status"
        if "/button/" in topic:
            assert "expire_after" not in body and "state_topic" not in body
            assert body["command_topic"] == "sdlog/cmd/collect"
        else:
            assert body["expire_after"] == 1800 and body["state_topic"] == "sdlog/state"
    cell = entities["homeassistant/sensor/sdlog/sdlog_demo_car_cell_b/config"]
    assert cell["unit_of_measurement"] == "V" and cell["device_class"] == "voltage"
    assert cell["value_template"] == "{{ value_json.vehicles['demo-car'].cell_b.value }}"


def test_last_will_and_online(env) -> None:
    config, sink, _, _ = env
    client = FakeClient()
    pub.Publisher(config, client, pub.FakeSinkDb(config.decode, sink)).on_connect()
    assert ("sdlog/status", "online", True) in client.published
    assert all(retain for _, _, retain in client.published)
    assert client.subscribed == ["sdlog/cmd/collect"]


def test_state_is_published_retained(env) -> None:
    config, sink, _, _ = env
    client = FakeClient()
    pub.Publisher(config, client, pub.FakeSinkDb(config.decode, sink)).publish_state()
    ((topic, payload, retain),) = client.published
    assert topic == "sdlog/state" and retain
    assert json.loads(payload)["devices"]["mr-orange"]["ok"] == 1


def test_button_touches_the_trigger_only(env) -> None:
    config, sink, _, tmp = env
    client = FakeClient()
    publisher = pub.Publisher(config, client, pub.FakeSinkDb(config.decode, sink))
    publisher.on_message("sdlog/cmd/other", b"PRESS")
    assert not config.trigger.exists()
    publisher.on_message("sdlog/cmd/collect", b"PRESS")
    assert config.trigger.exists()
    assert client.published == []


# ------------------------------------------------------------------ P10–P12


def test_collect_writes_nothing(env) -> None:
    config, sink, fleet, tmp = env
    beat(fleet, "mr-orange", last_result="ok", verdict="ok")

    def snapshot():
        return {p: (p.stat().st_mtime_ns, p.read_bytes()) for p in tmp.rglob("*") if p.is_file()}

    before, tables = snapshot(), {t: list(r) for t, r in sink.tables.items()}
    pub.collect(config, pub.FakeSinkDb(config.decode, sink), now=NOW)
    assert snapshot() == before and sink.tables == tables


def test_once_needs_no_mqtt() -> None:
    tree = ast.parse((_SCRIPT / "sdlog_publish.py").read_text())
    top_level = [n for n in tree.body if isinstance(n, (ast.Import, ast.ImportFrom))]
    names = {a.name for n in top_level for a in getattr(n, "names", [])} | {getattr(n, "module", None) for n in top_level}
    assert not any(name and name.startswith("paho") for name in names)


def test_the_example_config_loads(tmp_path: Path) -> None:
    deploy = _SCRIPT.parent / "deploy"
    decode = (deploy / "sdlog-decode" / "decode.example.toml").read_text()
    decode = decode.replace("/path/to/consuming-project/vehicles/demo/dbc/ev.dbc", str(MINI_DBC))
    decode = decode.replace(
        "/path/to/consuming-project/vehicles/demo/uds/mini.dcat", str(_SCRIPT.parent / "tests" / "uds" / "fixtures" / "mini.dcat")
    )
    decode = decode.replace(
        "/path/to/consuming-project/vehicles/demo/uds/mini.names.toml", str(deploy / "sdlog-decode" / "names.example.toml")
    )
    (tmp_path / "decode.toml").write_text(decode)
    (tmp_path / "mqtt.pass").write_text("x")
    text = (deploy / "sdlog-publish" / "publish.example.toml").read_text()
    text = text.replace("/etc/sdlog-decode/decode.toml", str(tmp_path / "decode.toml"))
    text = text.replace("/etc/sdlog-publish/mqtt.pass", str(tmp_path / "mqtt.pass"))
    (tmp_path / "publish.toml").write_text(text)
    config = pub.load_config(tmp_path / "publish.toml")
    assert config.vehicles[0].name == "demo-car"
