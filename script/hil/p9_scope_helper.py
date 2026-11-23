#!/usr/bin/env python3
"""One block per JSON-line request, using pico-bench's own x86 driver process.

No SDK/numpy import until main; serve() accepts a fake Scope for offline tests.
The API's volts are converted back to the original ADC integers by rounding.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys

from p9_scope_format import external, validate_request, write_block


def capture(scope, req: dict) -> dict:
    n = validate_request(req)  # before opening/arming hardware
    out = external(Path(req["out"]))
    if out.exists():
        raise ValueError("output directory must be new")
    with scope as device:
        info = device.info()
        if info.variant != "3203D":
            raise ValueError(f"expected 3203D, got {info.variant}")
        for ch in ("A", "B"):
            device.set_channel(ch, req["range_v"], enabled=ch in req["channels"])
        t = req["trigger"]
        device.set_simple_trigger(
            t["channel"], t["threshold_v"], t["direction"], auto_ms=0
        )
        block = device.capture_block(
            req["channels"], req["rate"], n, timeout_s=req["seconds"] + 15
        )
        if any(len(v) != n for v in block.volts.values()):
            raise ValueError("short block returned by driver")
        # Public Capture has volts only; _max_adc is the driver's actual ADC scale.
        adc = device._max_adc.value
        samples = {
            ch: (round(v / req["range_v"] * adc) for v in block.volts[ch])
            for ch in req["channels"]
        }
        meta = {
            "what": req["what"],
            "dt_s": block.dt_s,
            "range_v": req["range_v"],
            "max_adc": adc,
            "probe": req["probe"],
            "trigger": t,
            "scope_serial": info.serial,
            "scope_variant": info.variant,
            "driver": info.driver,
            "overflow": block.overflow,
            "requested_rate": req["rate"],
            "requested_seconds": req["seconds"],
        }
        path = write_block(out, meta, samples)
        return {
            "saved": str(path),
            "capture_sha256": json.loads(path.read_text(encoding="utf-8"))[
                "capture_sha256"
            ],
        }


def serve(source, sink, factory):
    for line in source:
        try:
            req = json.loads(line)
            if req.get("cmd") != "capture":
                raise ValueError("expected capture command")
            validate_request(req)
            reply = capture(factory(), req)
        except (ValueError, KeyError, OSError, RuntimeError) as exc:
            reply = {"error": f"{type(exc).__name__}: {exc}"}
        sink.write(json.dumps(reply) + "\n")
        sink.flush()


def main():
    sys.path.insert(0, str(Path(sys.argv[1]).expanduser().resolve() / "src"))
    from picobench.driver import Scope

    serve(sys.stdin, sys.stdout, Scope)


if __name__ == "__main__":
    main()
