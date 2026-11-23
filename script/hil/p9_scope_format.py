"""Portable P9 scope blocks: signed little-endian ADC counts and hashed metadata."""

from __future__ import annotations

from array import array
import hashlib
import json
import math
from pathlib import Path
import sys

VERSION = "p9-scope/1"
BUFFER_SAMPLES = 64_000_000
RANGES = (0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50)
REPO = Path(__file__).resolve().parents[2]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def external(path: Path) -> Path:
    path = path.expanduser().resolve()
    if path.is_relative_to(REPO):
        raise ValueError("keep bench evidence outside this repository")
    return path


def validate_request(req: dict) -> int:
    channels = req["channels"]
    if (
        not channels
        or len(set(channels)) != len(channels)
        or set(channels) - {"A", "B"}
    ):
        raise ValueError("channels must be A, B, or A,B")
    for key in ("rate", "seconds", "range_v", "probe"):
        if not math.isfinite(req[key]) or req[key] <= 0:
            raise ValueError(f"{key} must be finite and positive")
    if req["range_v"] not in RANGES:
        raise ValueError(f"range_v must be a hardware range: {RANGES}")
    n = math.ceil(req["rate"] * req["seconds"])
    if n < 2 or n * len(channels) > BUFFER_SAMPLES:
        raise ValueError("capture exceeds 64 MS shared buffer (or is too short)")
    t = req["trigger"]
    if t["channel"] not in channels or t["direction"] not in ("falling", "rising"):
        raise ValueError("trigger must use an enabled channel and rising/falling edge")
    if not math.isfinite(t["threshold_v"]) or abs(t["threshold_v"]) >= req["range_v"]:
        raise ValueError("trigger threshold must be inside the BNC voltage range")
    return n


def write_block(out: Path, meta: dict, samples: dict) -> Path:
    """Write a new block; capture hash binds metadata as well as every channel."""
    out.mkdir(parents=True, exist_ok=False)
    meta = dict(meta, schema=VERSION, tool_version=VERSION, channels={})
    for ch, values in samples.items():
        raw = array("h", values)
        if sys.byteorder != "little":
            raw.byteswap()
        data = raw.tobytes()
        name = f"{ch}.bin"
        (out / name).write_bytes(data)
        meta["channels"][ch] = {
            "file": name,
            "samples": len(raw),
            "sha256": digest(data),
        }
    meta["capture_sha256"] = digest(json.dumps(meta, sort_keys=True).encode())
    path = out / "capture.json"
    path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return path


def read_block(path: Path) -> tuple[dict, dict]:
    if path.is_dir():
        path /= "capture.json"
    meta = json.loads(path.read_text(encoding="utf-8"))
    unsigned = {k: v for k, v in meta.items() if k != "capture_sha256"}
    if meta.get("schema") != VERSION or meta.get("capture_sha256") != digest(
        json.dumps(unsigned, sort_keys=True).encode()
    ):
        raise ValueError("capture metadata hash/schema mismatch")
    for key in ("dt_s", "range_v", "max_adc", "probe"):
        if not math.isfinite(meta[key]) or meta[key] <= 0:
            raise ValueError(f"invalid {key}")
    samples = {}
    for ch, entry in meta["channels"].items():
        if ch not in ("A", "B") or entry["file"] != f"{ch}.bin":
            raise ValueError("invalid channel filename")
        data = (path.parent / entry["file"]).read_bytes()
        if digest(data) != entry["sha256"] or len(data) != 2 * entry["samples"]:
            raise ValueError(f"{ch}: sample hash/length mismatch")
        raw = array("h")
        raw.frombytes(data)
        if sys.byteorder != "little":
            raw.byteswap()
        samples[ch] = raw
    if not samples or len({len(v) for v in samples.values()}) != 1:
        raise ValueError("missing/unequal channel lengths")
    return meta, samples
