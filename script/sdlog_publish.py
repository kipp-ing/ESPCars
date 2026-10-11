#!/usr/bin/env python3
"""Publish the sd_logger pipeline's state to Home Assistant (docs/sdlog-publish-spec.md).

A long-running service. Every ``interval_s`` it collects

    the chain     per device: last pull and its result (fleet heartbeat), chunks
                  ok / defect / pending, the newest data, #gaps of the last 7 days
    the values    per vehicle: the last known value of each configured signal
    the problem   on iff ClickHouse is down, the archive is short of space, a pull
                  ended in error, or a defect chunk is on record — never because a
                  car is away

and publishes it as one retained JSON, with MQTT discovery so Home Assistant
builds the entities itself. A button asks for a collection pass now: it creates
the trigger file the fleet's ``.path`` unit watches, and nothing else.

Usage:
    script/sdlog_publish.py run  --config publish.toml   # the service
    script/sdlog_publish.py once --config publish.toml   # print the state once, no MQTT

Only this tool needs ``paho-mqtt`` (https://pypi.org/project/paho-mqtt/), and only for ``run``.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import shutil
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    sys.exit("sdlog_publish.py needs Python >= 3.11 (tomllib)")

EX_CONFIG = 2
GAP_WINDOW = timedelta(days=7)


def _load(name: str):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, Path(__file__).absolute().parent / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


dec = _load("sdlog_decode")

_TOP_KEYS = {
    "node", "decode_config", "fleet_state", "archive_min_free_mb", "trigger", "interval_s",
    "expire_after_s", "discovery_prefix", "mqtt", "vehicle",
}
_MQTT_KEYS = {"host", "port", "user", "password_file"}
_VEHICLE_KEYS = {"name", "sensor"}
_SENSOR_KEYS = {"key", "name", "signal", "unit", "device_class"}
_KEY = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Sensor:
    key: str
    name: str
    signal: str
    unit: str | None = None
    device_class: str | None = None


@dataclass(frozen=True)
class Vehicle:
    name: str
    sensors: tuple[Sensor, ...]


@dataclass(frozen=True)
class Config:
    node: str
    decode: object  # sdlog_decode.Config
    fleet_state: Path
    archive_min_free_mb: float
    trigger: Path
    interval_s: float
    expire_after_s: int
    discovery_prefix: str
    mqtt_host: str
    mqtt_port: int
    mqtt_user: str | None
    mqtt_password_file: Path | None
    vehicles: tuple[Vehicle, ...]


def load_config(path: Path) -> Config:
    try:
        raw = tomllib.loads(path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as err:
        raise ConfigError(f"{path}: {err}") from None
    unknown = set(raw) - _TOP_KEYS
    if unknown:
        raise ConfigError(f"unknown key(s): {', '.join(sorted(unknown))}")
    node = raw.get("node", "sdlog")
    if not isinstance(node, str) or not _KEY.match(node):
        raise ConfigError(f"node must be a lowercase identifier, not {node!r}")
    if "decode_config" not in raw:
        raise ConfigError("decode_config is required")
    try:
        decode = dec.load_config(Path(raw["decode_config"]))
    except dec.ConfigError as err:
        raise ConfigError(f"decode_config: {err}") from None
    mqtt = raw.get("mqtt", {})
    if "password" in mqtt:
        raise ConfigError("mqtt: password must come from password_file, never inline")
    unknown = set(mqtt) - _MQTT_KEYS
    if unknown:
        raise ConfigError(f"mqtt: unknown key(s): {', '.join(sorted(unknown))}")
    if "host" not in mqtt:
        raise ConfigError("mqtt.host is required")
    password_file = Path(mqtt["password_file"]) if "password_file" in mqtt else None
    if password_file is not None and not password_file.is_file():
        raise ConfigError(f"mqtt.password_file not found: {password_file}")
    known_vehicles = {d.vehicle for d in decode.devices}
    vehicles = []
    for entry in raw.get("vehicle", []):
        unknown = set(entry) - _VEHICLE_KEYS
        if unknown:
            raise ConfigError(f"vehicle: unknown key(s): {', '.join(sorted(unknown))}")
        name = entry.get("name")
        if name not in known_vehicles:
            raise ConfigError(f"vehicle {name!r} is not in the decode config")
        sensors, keys = [], set()
        for s in entry.get("sensor", []):
            unknown = set(s) - _SENSOR_KEYS
            if unknown:
                raise ConfigError(f"vehicle {name!r} sensor: unknown key(s): {', '.join(sorted(unknown))}")
            key = s.get("key")
            if not isinstance(key, str) or not _KEY.match(key):
                raise ConfigError(f"vehicle {name!r}: sensor key {key!r} is not a lowercase identifier")
            if key in keys:
                raise ConfigError(f"vehicle {name!r}: duplicate sensor key {key!r}")
            keys.add(key)
            if not s.get("signal") or not s.get("name"):
                raise ConfigError(f"vehicle {name!r} sensor {key!r}: name and signal are required")
            sensors.append(Sensor(key, s["name"], s["signal"], s.get("unit"), s.get("device_class")))
        vehicles.append(Vehicle(name, tuple(sensors)))
    return Config(
        node=node,
        decode=decode,
        fleet_state=Path(raw.get("fleet_state", "/var/lib/sdlog-fleet")),
        archive_min_free_mb=float(raw.get("archive_min_free_mb", 1024)),
        trigger=Path(raw.get("trigger", "/run/sdlog-fleet/trigger")),
        interval_s=float(raw.get("interval_s", 60)),
        expire_after_s=int(raw.get("expire_after_s", 1800)),
        discovery_prefix=raw.get("discovery_prefix", "homeassistant"),
        mqtt_host=mqtt["host"],
        mqtt_port=int(mqtt.get("port", 1883)),
        mqtt_user=mqtt.get("user"),
        mqtt_password_file=password_file,
        vehicles=tuple(vehicles),
    )


# ------------------------------------------------------------------------ sources


class ClickHouseDb:
    """The decode tables, read-only."""

    def __init__(self, decode_config) -> None:
        self.cfg = decode_config
        self.sink = dec.ClickHouseSink(decode_config.clickhouse_url, decode_config.database, timeout_s=30.0)
        self.db = decode_config.database

    def ping(self) -> None:
        self.sink.query("SELECT 1")

    def device_status(self, device) -> tuple[int, int, int]:
        return dec.device_status(self.cfg, self.sink, device)

    def last_utc(self, device: str) -> int | None:
        text = self.sink.query(
            f"SELECT toUnixTimestamp64Micro(max(utc)) FROM {self.db}.frames WHERE device = {{d:String}} AND utc IS NOT NULL",
            {"d": device},
        ).strip()
        return None if text in ("", "\\N", "0") else int(text)

    def gaps_since(self, device: str, since_us: int) -> int:
        text = self.sink.query(
            f"SELECT sum(gaps) FROM (SELECT argMax(gaps, decoded_at) AS gaps, argMax(utc_max, decoded_at) AS u"
            f" FROM {self.db}.chunks WHERE device = {{d:String}} GROUP BY file)"
            " WHERE u >= fromUnixTimestamp64Micro({s:Int64}, 'UTC')",
            {"d": device, "s": since_us},
        ).strip()
        return int(text or 0)

    def latest(self, vehicle: str, signal: str) -> tuple[float, int] | None:
        text = self.sink.query(
            f"SELECT argMax(value, utc), toUnixTimestamp64Micro(max(utc)), count() FROM {self.db}.signals"
            " WHERE vehicle = {v:String} AND signal = {s:String} AND utc IS NOT NULL FORMAT TSV",
            {"v": vehicle, "s": signal},
        ).strip()
        value, utc, count = text.split("\t")
        return None if count == "0" else (float(value), int(utc))


class FakeSinkDb:
    """The same answers from a FakeSink's in-memory tables (tests)."""

    def __init__(self, decode_config, sink) -> None:
        self.cfg, self.sink = decode_config, sink

    def ping(self) -> None:
        self.sink._check()

    def device_status(self, device):
        return dec.device_status(self.cfg, self.sink, device)

    def last_utc(self, device):
        self.sink._check()
        # The tables hold ms (decode spec §2); this class answers in µs like the real one.
        values = [f["utc"] for f in self.sink.tables["frames"] if f["device"] == device and f["utc"] is not None]
        return max(values) * 1000 if values else None

    def gaps_since(self, device, since_us):
        self.sink._check()
        latest = {}
        for c in self.sink.tables["chunks"]:
            if c["device"] == device and (c["file"] not in latest or c["decoded_at"] >= latest[c["file"]]["decoded_at"]):
                latest[c["file"]] = c
        return sum(c["gaps"] for c in latest.values() if (c["utc_max"] or 0) * 1000 >= since_us)

    def latest(self, vehicle, signal):
        self.sink._check()
        rows = [r for r in self.sink.tables["signals"] if r["vehicle"] == vehicle and r["signal"] == signal and r["utc"] is not None]
        if not rows:
            return None
        best = max(rows, key=lambda r: r["utc"])
        return best["value"], best["utc"] * 1000


# ------------------------------------------------------------------------ collect


def _iso_us(utc_us: int | None) -> str | None:
    if utc_us is None:
        return None
    return datetime.fromtimestamp(utc_us / 1e6, tz=timezone.utc).isoformat(timespec="seconds")


def _fleet(config: Config, device: str) -> dict:
    path = config.fleet_state / f"{device}.json"
    try:
        beat = json.loads(path.read_text())
    except (OSError, ValueError):
        return {"last_pull": None, "result": None, "verdict": None}
    last = beat.get("last_success")
    if last:
        last = datetime.fromisoformat(last).astimezone(timezone.utc).isoformat(timespec="seconds")
    return {"last_pull": last, "result": beat.get("last_result"), "verdict": beat.get("verdict")}


def collect(config: Config, db, now: datetime | None = None) -> dict:
    """The whole state as one JSON-able dict. Reads only (P10)."""
    now = now or datetime.now(timezone.utc)
    reasons: list[str] = []
    db_ok = True
    try:
        db.ping()
    except (dec.SinkUnavailable, dec.SinkError):
        db_ok = False
        reasons.append("clickhouse")
    free_mb = shutil.disk_usage(config.decode.archive).free / (1024 * 1024)
    if free_mb < config.archive_min_free_mb:
        reasons.append("space")
    since_us = int((now - GAP_WINDOW).timestamp() * 1e6)
    devices = {}
    for device in config.decode.devices:
        fleet = _fleet(config, device.name)
        entry = {"last_pull": fleet["last_pull"], "result": fleet["result"],
                 "last_data": None, "ok": None, "defect": None, "pending": None, "gaps_7d": None}
        if fleet["verdict"] == "error":
            reasons.append(f"collect:{device.name}")
        if db_ok:
            try:
                entry["ok"], entry["defect"], entry["pending"] = db.device_status(device)
                entry["last_data"] = _iso_us(db.last_utc(device.name))
                entry["gaps_7d"] = db.gaps_since(device.name, since_us)
            except (dec.SinkUnavailable, dec.SinkError):
                db_ok = False
                if "clickhouse" not in reasons:
                    reasons.append("clickhouse")
            if entry["defect"]:
                reasons.append(f"defect:{device.name}")
        devices[device.name] = entry
    vehicles = {}
    for vehicle in config.vehicles:
        values = {}
        for sensor in vehicle.sensors:
            hit = None
            if db_ok:
                try:
                    hit = db.latest(vehicle.name, sensor.signal)
                except (dec.SinkUnavailable, dec.SinkError):
                    hit = None
            values[sensor.key] = {"value": hit[0] if hit else None, "time": _iso_us(hit[1]) if hit else None}
        vehicles[vehicle.name] = values
    return {
        "problem": "ON" if reasons else "OFF",
        "reasons": reasons,
        "archive_free_gb": round(free_mb / 1024, 1),
        "devices": devices,
        "vehicles": vehicles,
        "updated": now.isoformat(timespec="seconds"),
    }


# ------------------------------------------------------------------------ MQTT


def _oid(*parts: str) -> str:
    return re.sub(r"[^a-z0-9_]", "_", "_".join(parts).lower())


def discovery(config: Config) -> list[tuple[str, dict]]:
    """(topic, payload) for every entity (P6)."""
    node = config.node
    state = f"{node}/state"
    avail = f"{node}/status"
    base = {"availability_topic": avail, "state_topic": state}
    hub = {"identifiers": [node], "name": node}
    out: list[tuple[str, dict]] = []

    def add(component: str, oid: str, payload: dict, expire: bool = True) -> None:
        body = {**base, **payload, "unique_id": oid, "object_id": oid}
        if expire:
            body["expire_after"] = config.expire_after_s
        out.append((f"{config.discovery_prefix}/{component}/{node}/{oid}/config", body))

    add("binary_sensor", _oid(node, "problem"), {
        "name": "Problem", "device_class": "problem", "value_template": "{{ value_json.problem }}",
        "payload_on": "ON", "payload_off": "OFF", "json_attributes_topic": state,
        "json_attributes_template": "{{ {'reasons': value_json.reasons, 'updated': value_json.updated} | tojson }}",
        "device": hub,
    })
    add("sensor", _oid(node, "archive_free"), {
        "name": "Archive free", "unit_of_measurement": "GB", "value_template": "{{ value_json.archive_free_gb }}",
        "entity_category": "diagnostic", "device": hub,
    })
    for device in config.decode.devices:
        d = device.name
        dev = {"identifiers": [_oid(node, d)], "name": d, "via_device": node}
        path = f"value_json.devices['{d}']"
        add("sensor", _oid(node, d, "last_pull"), {"name": "Last pull", "device_class": "timestamp",
            "value_template": f"{{{{ {path}.last_pull }}}}", "device": dev})
        add("sensor", _oid(node, d, "result"), {"name": "Pull result",
            "value_template": f"{{{{ {path}.result }}}}", "device": dev})
        add("sensor", _oid(node, d, "last_data"), {"name": "Last data", "device_class": "timestamp",
            "value_template": f"{{{{ {path}.last_data }}}}", "device": dev})
        for key, label in (("pending", "Chunks pending"), ("defect", "Chunks defect"), ("gaps_7d", "Gaps 7 days")):
            add("sensor", _oid(node, d, key), {"name": label, "entity_category": "diagnostic",
                "value_template": f"{{{{ {path}.{key} }}}}", "device": dev})
    for vehicle in config.vehicles:
        dev = {"identifiers": [_oid(node, "vehicle", vehicle.name)], "name": vehicle.name, "via_device": node}
        for sensor in vehicle.sensors:
            path = f"value_json.vehicles['{vehicle.name}'].{sensor.key}"
            payload = {"name": sensor.name, "value_template": f"{{{{ {path}.value }}}}",
                       "json_attributes_topic": state,
                       "json_attributes_template": f"{{{{ {{'time': {path}.time}} | tojson }}}}", "device": dev}
            if sensor.unit:
                payload["unit_of_measurement"] = sensor.unit
            if sensor.device_class:
                payload["device_class"] = sensor.device_class
            add("sensor", _oid(node, vehicle.name, sensor.key), payload)
    add("button", _oid(node, "collect_now"), {"name": "Collect now", "command_topic": f"{node}/cmd/collect",
        "payload_press": "PRESS", "device": hub}, expire=False)
    for _, body in out:
        if "command_topic" in body:
            body.pop("state_topic", None)
    return out


class Publisher:
    """The MQTT side, against any client with publish/subscribe (paho in `run`)."""

    def __init__(self, config: Config, client, db) -> None:
        self.config, self.client, self.db = config, client, db
        self.avail = f"{config.node}/status"
        self.cmd = f"{config.node}/cmd/collect"

    def on_connect(self) -> None:
        for topic, payload in discovery(self.config):
            self.client.publish(topic, json.dumps(payload), qos=1, retain=True)
        self.client.publish(self.avail, "online", qos=1, retain=True)
        self.client.subscribe(self.cmd, qos=1)

    def on_message(self, topic: str, payload: bytes) -> None:
        if topic != self.cmd:
            return
        self.config.trigger.parent.mkdir(parents=True, exist_ok=True)
        self.config.trigger.touch()

    def publish_state(self) -> dict:
        state = collect(self.config, self.db)
        self.client.publish(f"{self.config.node}/state", json.dumps(state), qos=1, retain=True)
        return state


def cmd_run(config: Config) -> int:  # pragma: no cover - wiring around paho, exercised on the host
    import paho.mqtt.client as mqtt

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"{config.node}-publish")
    if config.mqtt_user:
        password = config.mqtt_password_file.read_text().strip() if config.mqtt_password_file else None
        client.username_pw_set(config.mqtt_user, password)
    publisher = Publisher(config, client, ClickHouseDb(config.decode))
    client.will_set(publisher.avail, "offline", qos=1, retain=True)
    client.on_connect = lambda c, u, flags, rc, props: (
        print(f"mqtt connect failed: {rc}", file=sys.stderr) if rc.is_failure else publisher.on_connect()
    )
    client.on_message = lambda c, u, msg: publisher.on_message(msg.topic, msg.payload)
    client.reconnect_delay_set(1, 60)
    client.connect_async(config.mqtt_host, config.mqtt_port, keepalive=60)
    client.loop_start()
    try:
        while True:
            try:
                publisher.publish_state()
            except Exception as err:  # noqa: BLE001 - one bad cycle must not end the service
                print(f"publish failed: {err}", file=sys.stderr)
            time.sleep(config.interval_s)
    finally:
        client.publish(publisher.avail, "offline", qos=1, retain=True)
        client.loop_stop()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "once"):
        sub.add_parser(name).add_argument("--config", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        config = load_config(args.config)
    except ConfigError as err:
        print(f"config: {err}", file=sys.stderr)
        return EX_CONFIG
    if args.command == "once":
        print(json.dumps(collect(config, ClickHouseDb(config.decode)), indent=2))
        return 0
    return cmd_run(config)


if __name__ == "__main__":
    sys.exit(main())
