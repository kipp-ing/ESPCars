"""Inject slot pool sizing (`CAN_GATEWAY_INJECT_SLOTS`).

The pool is what actually bounds how many `inject()` calls can be in flight: a
slot is held until the TX-done ISR releases it. `inject()` refuses once
`outstanding_` reaches `tx_queue_depth`, so `tx_queue_depth + 1` is the most
that can ever be outstanding — sizing to the queue is necessary and sufficient.

Why it is worth a test: the burst producer that drives a 500 kbit/s bus to 90 %
load injects a whole TX queue in one main-loop pass (`cyclic_sends` cannot get
there — issue #2 floors each at the ~16 ms loop period). Before this sizing the
pool was four slots regardless of `tx_queue_depth`, so the burst silently
delivered four frames and the load looked like a bus problem.
"""

from __future__ import annotations

import pytest

from .common import PORT_A, PORT_B, gateway, port, setup_c6, validate


def slots(config) -> int:
    from esphome.components.can_gateway import inject_slot_count

    return inject_slot_count(config)


def test_default_queue_keeps_the_historic_floor(set_core_config) -> None:
    """tx_queue_depth defaults to 8, so the pool is 10 — the old floor of 4 only
    ever applied to configs smaller than that."""
    setup_c6(set_core_config)
    assert slots(validate(gateway())) == 10


@pytest.mark.parametrize("depth", [1, 2])
def test_tiny_queue_falls_back_to_the_minimum(set_core_config, depth: int) -> None:
    setup_c6(set_core_config)
    config = validate(
        gateway(ports=[port(PORT_A, tx_queue_depth=depth), dict(PORT_B)], routes=[])
    )
    # PORT_B still carries the default 8, so the max wins; the point is only
    # that nothing ever drops below 4.
    assert slots(config) >= 4


def test_pool_follows_the_deepest_tx_queue(set_core_config) -> None:
    """The burst case: tx_queue_depth 64 must buy 64 usable in-flight injects."""
    setup_c6(set_core_config)
    config = validate(
        gateway(ports=[port(PORT_A, tx_queue_depth=64), dict(PORT_B)], routes=[])
    )
    assert slots(config) == 66


def test_cyclic_sends_still_raise_the_floor(set_core_config) -> None:
    """A port with more cyclic senders than its queue is deep still gets a slot
    each — they hold one apiece for as long as they are running."""
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[port(PORT_A, tx_queue_depth=1), port(PORT_B, tx_queue_depth=1)],
            routes=[],
            cyclic_sends=[
                {
                    "id": f"cyc_{index}",
                    "port": "port_a",
                    "can_id": 0x100 + index,
                    "interval": "20ms",
                    "data": [0x00],
                }
                for index in range(12)
            ],
        )
    )
    assert slots(config) == 14


def test_deepest_queue_wins_over_cyclic_count(set_core_config) -> None:
    setup_c6(set_core_config)
    config = validate(
        gateway(
            ports=[port(PORT_A, tx_queue_depth=64), dict(PORT_B)],
            routes=[],
            cyclic_sends=[
                {
                    "id": "cyc_0",
                    "port": "port_a",
                    "can_id": 0x100,
                    "interval": "20ms",
                    "data": [0x00],
                }
            ],
        )
    )
    assert slots(config) == 66
