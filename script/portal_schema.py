#!/usr/bin/env python3
"""Validator for the portal state envelope (``docs/portal-spec.md`` section 5, ``schema_version: 2``).

``validate_envelope`` is the one place the wire rules live: the same function runs over a fixture
on disk, over the host emitter's output and over a live response from a board
(``script/portal_schema.py FILE...``; exit 0 iff every file is valid).

The rules are almost all "X is null if and only if Y", because a hole and a real reading of zero
are indistinguishable unless something says so explicitly. ``state: "ok"`` with an age past its
budget is the worst version of that lie, which is why ``ok_past_budget`` exists.

Section 5.4/5.5 extend the action entry (``control``, ``value``, ``options``, ``min``, ``max``,
``step``, ``unit``, ``result``). An action without the extension keys (an older producer) is
still valid; once ``control`` is present every extension key is required and typed.

Spec v1.5 adds a value's optional ``level`` and the node why-codes ``limit``, ``not_requested``
and ``switched_off``; each rule applies only when its key/code is present, so older envelopes
pass unchanged.

This validator speaks ``schema_version: 2`` only. It never raises on malformed input: a non-dict,
a missing key or a wrong type all come back as ordinary ``Violation`` objects, so it is safe to
point at arbitrary JSON, including a board's raw HTTP response.
"""

from __future__ import annotations

import argparse
import json
import re
from collections.abc import Iterator
from dataclasses import dataclass

SCHEMA_VERSION = 2


class _Vocab(frozenset):
    """A frozenset whose membership test never raises: JSON can hand the validator a list or
    object where a vocabulary word belongs, and ``[] in frozenset(...)`` raises TypeError."""

    def __contains__(self, item: object) -> bool:
        try:
            return frozenset.__contains__(self, item)
        except TypeError:
            return False


STATES = _Vocab(
    {
        "ok",
        "stale",
        "degraded",
        "refused",
        "silent",
        "absent",
        "down",
        "unreachable",
        "unknown",
        "incompatible",
    }
)

ORIGINS = _Vocab({"local", "peer", "cache"})
TRUSTS = _Vocab({"confirmed", "assumed"})
CLASSIFICATIONS = _Vocab({"real", "sna", "implausible"})
CLASS_CHARS = "rsi-"
SERIES_LEGEND = {"r": "real", "s": "SNA", "i": "implausible", "-": "not measured"}

NODE_KEYS: tuple[str, ...] = (
    "id",
    "kind",
    "label",
    "state",
    "why",
    "provenance",
    "values",
    "series",
    "children",
    "actions",
)

ENVELOPE_KEYS: tuple[str, ...] = (
    "schema_version",
    "seq",
    "series_legend",
    "served_by",
    "peers",
    "tree",
)
# Spec section 5.1/5.3: optional envelope/node keys that a producer MAY emit -- checked ad hoc
# (``path``/``ancestor_role`` by ``validate_envelope`` itself, ``role``/``roles_available`` by
# ``_validate_node``) but never REQUIRED, so they cannot join the mandatory ``*_KEYS`` tuples
# above without ``validate_envelope`` then demanding them on every envelope/node. Kept here so
# coverage tooling can still see and classify them -- a legitimately optional field is not a field
# nobody checks.
OPTIONAL_ENVELOPE_KEYS: tuple[str, ...] = ("links", "path", "ancestor_role")
OPTIONAL_NODE_KEYS: tuple[str, ...] = ("role", "roles_available")
SERVED_BY_KEYS: tuple[str, ...] = (
    "board",
    "node_id",
    "mac",
    "state",
    "why",
    "uptime_ms",
    "build",
    "owns",
    "components",
)
PEER_KEYS: tuple[str, ...] = (
    "board",
    "node_id",
    "endpoint",
    "state",
    "why",
    "schema_version",
    "mounts",
    "age_ms",
    "last_ok_age_ms",
)
VALUE_KEYS: tuple[str, ...] = (
    "key",
    "label",
    "unit",
    "value",
    "raw",
    "raw_hex",
    "classification",
    "state",
    "why",
)
# Spec 5.3 (v1.5): optional, after the v2 value keys.
OPTIONAL_VALUE_KEYS: tuple[str, ...] = ("level",)
LEVELS = _Vocab({"ok", "warn", "bad"})

SERIES_KEYS: tuple[str, ...] = (
    "key",
    "label",
    "unit",
    "index_base",
    "index",
    "index_frame",
    "values",
    "class",
    "pack_index_base",
)
ACTION_KEYS: tuple[str, ...] = ("key", "label", "method", "path", "enabled", "why", "body")
WHY_KEYS: tuple[str, ...] = ("code", "detail", "since_ms", "evidence")
PROVENANCE_KEYS: tuple[str, ...] = (
    "origin",
    "board",
    "node_id",
    "age_ms",
    "budget_ms",
    "trust",
    "seq",
)
ANCESTOR_ROLE_KEYS: tuple[str, ...] = ("node", "label", "role")

_METHODS = _Vocab({"GET", "POST", "PUT", "DELETE"})
_HEX_CHARS = frozenset("0123456789ABCDEF")
_WHY_CODE_RE = re.compile(r"^[a-z][a-z0-9_]*$")
# Mirrors _WHY_CODE_RE exactly -- the vocabulary is OPEN
# (spec section 5.3), so this validates the *shape* of a frame name, never membership.
# "pack_cell" is reserved and nobody emits it yet; "flux_capacitor" is just as valid a
# string to this pattern, and a test asserts that on purpose.
_INDEX_FRAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")


@dataclass(frozen=True)
class Violation:
    """One broken invariant. ``code`` is machine-readable, snake_case, from the list
    in this module's own checks — it is what a caller branches on, never ``detail``."""

    path: str
    code: str
    detail: str

    def __str__(self) -> str:
        return f"{self.path}: {self.code} — {self.detail}"


class SchemaViolationError(Exception):
    """Raised by ``check_envelope`` when ``validate_envelope`` found anything."""

    def __init__(self, violations: list[Violation]) -> None:
        super().__init__(f"{len(violations)} schema violation(s)")
        self.violations = violations


def _is_str(value: object) -> bool:
    return isinstance(value, str)


def _is_str_or_null(value: object) -> bool:
    return value is None or isinstance(value, str)


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_bool(value: object) -> bool:
    return isinstance(value, bool)


def _is_nonneg_int_or_null(value: object) -> bool:
    return value is None or (_is_int(value) and value >= 0)


def _is_list_of_str(value: object) -> bool:
    return isinstance(value, list) and all(_is_str(x) for x in value)


# --- why blocks ---------------------------------------------------------------------

# schema §3's "four that must never collapse into one another" table names the counter each
# code's discriminating power actually depends on (spec section 5.3):
# an empty `evidence: {}` is legal for every OTHER code (the code itself is the verdict for
# entities_unavailable/not_polled/no_identity/... and there is no counter behind them to
# demand), but for these four it is the exact lie the schema's own table exists to prevent.
# Key names are the ones the golden fixtures actually use, listed explicitly rather than
# matched by prefix, so a new spelling is a visible diff rather than a silent pass.
_EVIDENCE_DISCRIMINATORS: dict[str, tuple[str, ...]] = {
    "never_announced": ("rx_frames", "announced"),
    "no_transfer": ("rx_frames", "announced"),
    "crc_fail": ("crc_fail_count",),
    "negative_response": ("nrc_hex", "nrc_code_hex", "nrc_name"),
}


def _validate_why_block(why: dict, path: str, violations: list[Violation]) -> None:
    """Structural check for a non-null why block: node/value/served_by/peer share this
    shape (schema §3). Actions carry a why too, but only the null-iff-enabled rule is
    specified for it, so its interior is not checked here."""
    for key in WHY_KEYS:
        if key not in why:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(why, WHY_KEYS, path, "why", violations)

    if "code" in why:
        code = why["code"]
        if not _is_str(code) or not _WHY_CODE_RE.match(code):
            violations.append(
                Violation(f"{path}.code", "why_code_not_machine_readable", f"code {code!r}")
            )
    if "detail" in why and not _is_str_or_null(why["detail"]):
        violations.append(Violation(f"{path}.detail", "wrong_type", "detail must be str or null"))
    if "since_ms" in why and not _is_nonneg_int_or_null(why["since_ms"]):
        violations.append(
            Violation(f"{path}.since_ms", "wrong_type", "since_ms must be int>=0 or null")
        )
    if "evidence" in why and not isinstance(why["evidence"], dict):
        violations.append(Violation(f"{path}.evidence", "wrong_type", "evidence must be an object"))

    if "code" in why and "evidence" in why and _is_str(why["code"]):
        discriminators = _EVIDENCE_DISCRIMINATORS.get(why["code"])
        evidence = why["evidence"]
        if discriminators is not None and isinstance(evidence, dict):
            if not any(key in evidence for key in discriminators):
                violations.append(
                    Violation(
                        f"{path}.evidence",
                        "evidence_without_discriminator",
                        f"why.code {why['code']!r} needs evidence carrying one of {discriminators}",
                    )
                )


def _check_why(state_is_ok: bool, why: object, path: str, violations: list[Violation]) -> None:
    """The headline invariant: why is null iff state == 'ok' (schema §3)."""
    if state_is_ok:
        if why is not None:
            violations.append(Violation(path, "why_state_mismatch", "why must be null when ok"))
            if isinstance(why, dict):
                _validate_why_block(why, path, violations)
    else:
        if why is None:
            violations.append(Violation(path, "why_state_mismatch", "why must not be null"))
        elif not isinstance(why, dict):
            violations.append(Violation(path, "wrong_type", "why must be an object or null"))
        else:
            _validate_why_block(why, path, violations)


def _check_frames_evidence(state: str, why: object, path: str, violations: list[Violation]) -> None:
    """silent/absent share age_ms: null and are told apart only by rx_frames (schema §3)."""
    if not isinstance(why, dict):
        return
    evidence = why.get("evidence")
    if not isinstance(evidence, dict) or "rx_frames" not in evidence:
        return
    rx = evidence["rx_frames"]
    if isinstance(rx, bool) or not isinstance(rx, (int, float)):
        return
    if state == "silent" and not rx > 0:
        violations.append(Violation(path, "silent_without_frames", f"rx_frames={rx!r}"))
    elif state == "absent" and rx != 0:
        violations.append(Violation(path, "absent_with_frames", f"rx_frames={rx!r}"))


def _check_announced_evidence(state: str, why: object, path: str, violations: list[Violation]) -> None:
    """A node's own evidence must not refute the verdict it is attached to. Modelled on
    ``_check_frames_evidence`` above, the sibling invariant for the other half of the
    absent/silent discrimination (schema §3): ``announced`` is the other counter that
    separates "nothing there" from "there, but nothing completes", and it was left
    unconstrained. Found on screen and confirmed in served data
    (a replayed degraded scenario, node ``ecu.pack.grp0``): ``state: "absent"`` / ``why.code: "never_announced"`` while
    ``why.evidence.announced`` was ``true``.

    The two checks below are independent by design, each keyed on one field alone, so
    neither can be satisfied by accident when the other half of a contradictory pair is
    absent: ``why.code == "never_announced"`` is a claim that stands or falls on its own,
    whatever state it is attached to; ``state == "absent"`` is a claim that stands or
    falls on its own, whatever why.code phrases it as. ``announced`` must be the literal
    ``True`` singleton — ``bool`` is an ``int`` subclass, so a loose truthy check would
    wrongly accept ``announced: 1``, which is not JSON's ``true``.
    """
    if not isinstance(why, dict):
        return
    evidence = why.get("evidence")
    announced = evidence.get("announced") if isinstance(evidence, dict) else None
    if announced is not True:
        return
    if why.get("code") == "never_announced":
        violations.append(
            Violation(
                path,
                "never_announced_with_announced",
                "why.code is never_announced but why.evidence.announced is true",
            )
        )
    if state == "absent":
        violations.append(
            Violation(
                path,
                "absent_with_announced",
                "state is absent but why.evidence.announced is true",
            )
        )


# --- provenance -----------------------------------------------------------------


def _validate_provenance(
    provenance: object, node_state: object, path: str, violations: list[Violation]
) -> None:
    if provenance is None:
        if node_state == "down":
            violations.append(
                Violation(
                    path,
                    "down_not_confirmed",
                    "state down requires provenance.trust confirmed, provenance is null",
                )
            )
        return
    if not isinstance(provenance, dict):
        violations.append(Violation(path, "wrong_type", "provenance must be an object or null"))
        return

    for key in PROVENANCE_KEYS:
        if key not in provenance:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(provenance, PROVENANCE_KEYS, path, "provenance", violations)

    if "origin" in provenance and provenance["origin"] not in ORIGINS:
        violations.append(
            Violation(f"{path}.origin", "wrong_type", f"unknown origin {provenance['origin']!r}")
        )
    if "trust" in provenance and provenance["trust"] not in TRUSTS:
        violations.append(
            Violation(f"{path}.trust", "wrong_type", f"unknown trust {provenance['trust']!r}")
        )
    for key in ("board", "node_id"):
        if key in provenance and not _is_str_or_null(provenance[key]):
            violations.append(
                Violation(f"{path}.{key}", "wrong_type", f"{key} must be str or null")
            )
    for key in ("age_ms", "budget_ms", "seq"):
        if key in provenance and not _is_nonneg_int_or_null(provenance[key]):
            violations.append(
                Violation(f"{path}.{key}", "wrong_type", f"{key} must be int>=0 or null")
            )

    if node_state == "down" and provenance.get("trust") != "confirmed":
        violations.append(
            Violation(
                path,
                "down_not_confirmed",
                f"state down requires provenance.trust confirmed, got {provenance.get('trust')!r}",
            )
        )

    age_ms = provenance.get("age_ms")
    budget_ms = provenance.get("budget_ms")
    age_known = _is_int(age_ms)
    budget_known = _is_int(budget_ms)

    if node_state == "ok" and age_known and budget_known and age_ms > budget_ms:
        violations.append(
            Violation(path, "ok_past_budget", f"age_ms {age_ms} > budget_ms {budget_ms}, state ok")
        )
    if node_state == "stale":
        if age_ms is None:
            violations.append(Violation(path, "stale_without_age", "stale requires a known age_ms"))
        elif age_known and budget_known and not age_ms > budget_ms:
            violations.append(
                Violation(
                    path,
                    "stale_within_budget",
                    f"age_ms {age_ms} <= budget_ms {budget_ms} but state stale",
                )
            )


def _validate_provenance_placement(
    node: object,
    roots: set[str],
    served_by_board: object,
    violations: list[Violation],
    path: str = "tree",
) -> bool:
    """Spec section 5.3: ownership decides provenance placement, never a node's shape or depth.
    ``roots`` is ``served_by.owns`` UNION every ``peers[i].mounts`` entry -- the union, not
    ``owns`` alone, is load-bearing for a board without identity (``owns: []``,
    ``mounts: ["ecu.pack"]``): ``car``/``ecu`` are strict ancestors of the MOUNTED pack, so
    gating on ``owns`` alone would misclassify them as unmounted and wrongly demand an
    object. String ids are hierarchical (a child's id is its parent's id + "." + a segment)
    EXCEPT the synthetic ``car`` root, whose children's ids are bare (``ecu``, not
    ``car.ecu``), so ancestor status is derived from the tree's own nesting here, never from
    string prefixing alone; a served (possibly narrowed, spec section 5.1) subtree gives this
    function exactly the nesting it needs, id string matching aside.

    Three classes, and class 3 is DETERMINED, not unconstrained -- "unconstrained" would
    permit null, and null deletes the very age_ms/budget_ms a class-3 node exists to keep:
    1. INSIDE a root (``id == r`` or ``id.startswith(r + ".")`` for some ``r in roots``) ->
       provenance MUST be an object -> ``provenance_null_inside_mount`` if null.
    2. ABOVE every root: not inside, but a strict ancestor (by tree nesting) of a node that
       is inside one -> provenance MUST be null -> ``provenance_on_skeleton_node`` if an
       object.
    3. NEITHER inside nor above any root -> provenance MUST be an object, never null ->
       ``provenance_null_unmounted`` if null (e.g. an ``ecu.vehicle``
       subtree, once ``ecu.pack`` is excluded as class 2 above: a board with no identity must
       not lose every node's age_ms/budget_ms merely because it cannot name itself).

    A fourth, independent rule, general rather than case-specific: a node whose own
    ``provenance.origin == "local"`` may not name a board when the serving board has none --
    ``served_by.board is None`` and that node's ``provenance.board is not None`` is
    ``provenance_names_board_without_identity``. A board that cannot name itself cannot stamp
    a name. Peer-mounted nodes (``origin != "local"``) are exempt by construction: their board
    name comes from the peer's own response, not this board's identity.

    ``trust`` is never gated here -- schema §3: carried, never derived.
    ``down_not_confirmed`` (checked in ``_validate_provenance`` above) is unchanged: it now
    *follows* from ownership (a ``down`` node is a measurement, therefore owned, therefore
    carries provenance) but still fires on its own, which this function does not touch.

    Returns whether ``node``'s own subtree contains anything INSIDE a root -- how the caller,
    one level up, learns whether IT is a strict ancestor of something inside."""
    if not isinstance(node, dict):
        return False
    node_id = node.get("id")
    is_inside = isinstance(node_id, str) and (
        node_id in roots or any(node_id.startswith(f"{r}.") for r in roots)
    )

    any_descendant_inside = False
    children = node.get("children")
    if isinstance(children, list):
        for i, child in enumerate(children):
            if _validate_provenance_placement(
                child, roots, served_by_board, violations, f"{path}.children[{i}]"
            ):
                any_descendant_inside = True

    provenance = node.get("provenance")
    if is_inside:
        if provenance is None:
            violations.append(
                Violation(
                    path,
                    "provenance_null_inside_mount",
                    f"{node_id!r} is inside an owned/mounted subtree but provenance is null",
                )
            )
    elif any_descendant_inside:
        if provenance is not None:
            violations.append(
                Violation(
                    path,
                    "provenance_on_skeleton_node",
                    f"{node_id!r} is a skeleton ancestor of a mounted subtree but provenance "
                    "is not null",
                )
            )
    else:
        if provenance is None:
            violations.append(
                Violation(
                    path,
                    "provenance_null_unmounted",
                    f"{node_id!r} is neither inside nor above an owned/mounted subtree, but "
                    "provenance is null",
                )
            )

    if isinstance(provenance, dict) and provenance.get("origin") == "local":
        prov_board = provenance.get("board")
        if served_by_board is None and prov_board is not None:
            violations.append(
                Violation(
                    f"{path}.provenance",
                    "provenance_names_board_without_identity",
                    f"served_by.board is null but {node_id!r}'s local provenance names board "
                    f"{prov_board!r}",
                )
            )

    return is_inside or any_descendant_inside


def _validate_mount_ages(tree: object, peers: object, violations: list[Violation]) -> None:
    """Spec section 5.3: a mounted node cannot
    read fresher than the fetch that delivered it, because that age already includes the
    fetch -- the peer's own age plus the time since it arrived -- and one round trip
    delivered every node in the subtree. One uniform clause, the mount ROOT included: for each
    peer entry whose ``last_ok_age_ms`` is a known int ``F``, every node whose ``id`` is ``m``
    or starts with ``m + "."`` for some id ``m`` in that peer's ``mounts`` must report
    ``age_ms >= F`` when not null -> ``mounted_age_below_fetch``.

    An earlier draft of this check pinned the root to ``age_ms == F`` exactly. That is
    RETIRED so it cannot creep back as a    "tightening": age-at-peer is never negative, so ``peer_age + F >= F`` holds for every
    node, but it equals ``F`` at the root only while the peer's own root age is zero -- and
    that zero is a replay tool's constant, not a fact about
    mounts. every mount root in the reference captures carried 2-4 own
    values on a declared 500 ms cadence, so a root with a real, non-zero age is the normal
    case at 5b, and the equality form would force a producer to claim four genuinely-aged
    values had just been published in order to satisfy a consumer-side gate.

    ``age_ms: null`` is exempt everywhere -- it still means never received, not fresher than
    anything. ``last_ok_age_ms: null`` (nothing was ever successfully delivered) constrains
    nothing for that peer's mounts. id-based, not depth-based, so this applies to a narrowed
    response too: a ``?path=ecu.pack.grp0`` fetch from a board that mounts ``ecu.pack`` is
    still checked against that mount's own ``last_ok_age_ms``.
    """
    if not isinstance(tree, dict) or not isinstance(peers, list):
        return

    mount_fetches: list[tuple[str, int]] = []
    for peer in peers:
        if not isinstance(peer, dict):
            continue
        fetch_ms = peer.get("last_ok_age_ms")
        if not _is_int(fetch_ms):
            continue
        mounts = peer.get("mounts")
        if not isinstance(mounts, list):
            continue
        mount_fetches.extend((m, fetch_ms) for m in mounts if _is_str(m))

    if not mount_fetches:
        return

    for path, node in iter_nodes(tree):
        node_id = node.get("id")
        if not _is_str(node_id):
            continue
        provenance = node.get("provenance")
        age_ms = provenance.get("age_ms") if isinstance(provenance, dict) else None
        if age_ms is None or not _is_int(age_ms):
            continue
        for mount_id, fetch_ms in mount_fetches:
            if node_id != mount_id and not node_id.startswith(f"{mount_id}."):
                continue
            if age_ms < fetch_ms:
                where = "mount root" if node_id == mount_id else "mounted node"
                violations.append(
                    Violation(
                        path,
                        "mounted_age_below_fetch",
                        f"{where} {node_id!r} age_ms {age_ms} < mount {mount_id!r}'s "
                        f"last_ok_age_ms {fetch_ms}",
                    )
                )


# --- values / series / actions ---------------------------------------------------


def _validate_value(value: object, path: str, violations: list[Violation]) -> None:
    if not isinstance(value, dict):
        violations.append(Violation(path, "wrong_type", "value is not an object"))
        return

    for key in VALUE_KEYS:
        if key not in value:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(value, VALUE_KEYS + OPTIONAL_VALUE_KEYS, path, "value", violations)

    if "level" in value:
        level = value["level"]
        if level is not None and level not in LEVELS:
            violations.append(Violation(f"{path}.level", "unknown_level", f"unknown level {level!r}"))
        elif level is not None and value.get("value", 0) is None:
            violations.append(
                Violation(f"{path}.level", "level_without_reading", f"level {level!r} on a value without a reading")
            )

    if "key" in value and (not _is_str(value["key"]) or value["key"] == ""):
        violations.append(Violation(f"{path}.key", "wrong_type", "key must be a non-empty string"))

    if "label" in value:
        label = value["label"]
        if not _is_str(label):
            violations.append(Violation(f"{path}.label", "wrong_type", "label must be a string"))
        elif label == "":
            violations.append(Violation(f"{path}.label", "empty_label", "label must not be empty"))

    if "unit" in value and not _is_str_or_null(value["unit"]):
        violations.append(Violation(f"{path}.unit", "wrong_type", "unit must be str or null"))

    classification = value.get("classification")
    if "classification" in value and classification not in CLASSIFICATIONS:
        violations.append(
            Violation(
                f"{path}.classification",
                "unknown_classification",
                f"unknown classification {classification!r}",
            )
        )

    if "value" in value and "classification" in value:
        is_sna = classification == "sna"
        if (value["value"] is None) != is_sna:
            violations.append(
                Violation(path, "sna_value_mismatch", "value is null iff classification is sna")
            )

    if "raw_hex" in value and value["raw_hex"] is not None:
        raw_hex = value["raw_hex"]
        if not _is_str(raw_hex) or raw_hex == "" or not set(raw_hex) <= _HEX_CHARS:
            violations.append(
                Violation(f"{path}.raw_hex", "raw_hex_not_canonical", f"raw_hex {raw_hex!r}")
            )

    state = value.get("state")
    if "state" in value and state not in STATES:
        violations.append(Violation(f"{path}.state", "unknown_state", f"unknown state {state!r}"))

    if "why" in value:
        _check_why(state == "ok", value["why"], f"{path}.why", violations)


def _validate_series(series: object, path: str, violations: list[Violation]) -> None:
    if series is None:
        return
    if not isinstance(series, dict):
        violations.append(Violation(path, "wrong_type", "series must be an object or null"))
        return

    for key in SERIES_KEYS:
        if key not in series:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(series, SERIES_KEYS, path, "series", violations)

    for key in ("key", "label"):
        if key in series and (not _is_str(series[key]) or series[key] == ""):
            violations.append(
                Violation(f"{path}.{key}", "wrong_type", f"{key} must be a non-empty string")
            )
    if "unit" in series and not _is_str_or_null(series["unit"]):
        violations.append(Violation(f"{path}.unit", "wrong_type", "unit must be str or null"))
    if "index_base" in series and not _is_int(series["index_base"]):
        violations.append(
            Violation(f"{path}.index_base", "wrong_type", "index_base must be an int")
        )

    if "index_frame" in series:
        index_frame = series["index_frame"]
        if index_frame is not None:
            if not _is_str(index_frame):
                violations.append(
                    Violation(f"{path}.index_frame", "wrong_type", "index_frame must be str or null")
                )
            elif not _INDEX_FRAME_RE.match(index_frame):
                # No membership check: the vocabulary is open (spec section 5.3) --
                # only the SHAPE is wrong here, never an unrecognised-but-valid name.
                violations.append(
                    Violation(
                        f"{path}.index_frame",
                        "index_frame_not_canonical",
                        f"index_frame {index_frame!r}",
                    )
                )

    if "pack_index_base" in series:
        pack_index_base = series["pack_index_base"]
        if pack_index_base is not None and not _is_int(pack_index_base):
            violations.append(
                Violation(f"{path}.pack_index_base", "wrong_type", "pack_index_base must be an int or null")
            )

    values = series.get("values")
    values_ok = isinstance(values, list)
    if "values" in series and not values_ok:
        violations.append(Violation(f"{path}.values", "wrong_type", "values must be a list"))

    if "index" in series and series["index"] is not None:
        index = series["index"]
        if not isinstance(index, list) or not all(_is_int(x) for x in index):
            violations.append(
                Violation(f"{path}.index", "wrong_type", "index must be null or a list of int")
            )
        else:
            if values_ok and len(index) != len(values):
                violations.append(
                    Violation(
                        path,
                        "index_length_mismatch",
                        f"index has {len(index)}, values has {len(values)}",
                    )
                )
            if any(a >= b for a, b in zip(index, index[1:], strict=False)):
                violations.append(
                    Violation(path, "index_not_increasing", "index is not increasing")
                )

    if "class" in series:
        cls = series["class"]
        if cls is not None and not _is_str(cls):
            violations.append(
                Violation(f"{path}.class", "wrong_type", "class must be null or a str")
            )
        elif cls is None:
            if values_ok and any(v is None for v in values):
                violations.append(
                    Violation(path, "class_omitted_with_holes", "class omitted but a value is null")
                )
        elif values_ok:
            if len(cls) != len(values):
                violations.append(
                    Violation(
                        path,
                        "class_length_mismatch",
                        f"class has {len(cls)} chars, values has {len(values)}",
                    )
                )
            bad_chars = sorted(set(cls) - set(CLASS_CHARS))
            if bad_chars:
                violations.append(
                    Violation(path, "unknown_class_char", f"unknown class chars {bad_chars}")
                )
            if len(cls) == len(values):
                for i, (char, elem) in enumerate(zip(cls, values, strict=True)):
                    if char in ("s", "-") and elem is not None:
                        violations.append(
                            Violation(
                                f"{path}.values[{i}]",
                                "class_element_mismatch",
                                f"class {char!r} requires a null value",
                            )
                        )
                    elif char in ("r", "i") and elem is None:
                        violations.append(
                            Violation(
                                f"{path}.values[{i}]",
                                "class_element_mismatch",
                                f"class {char!r} requires a non-null value",
                            )
                        )


ACTION_EXT_KEYS: tuple[str, ...] = (
    "control",
    "value",
    "options",
    "min",
    "max",
    "step",
    "unit",
    "result",
)
ACTION_CONTROLS = _Vocab({"switch", "button", "select", "number"})
ACTION_BODY_BY_CONTROL: dict[str, object] = {
    "switch": {"value": "bool"},
    "button": None,
    "select": {"value": "string"},
    "number": {"value": "number"},
}
ACTION_RESULT_KEYS: tuple[str, ...] = ("state", "why", "age_ms", "value")
ACTION_RESULT_STATES = _Vocab({"running", "accepted", "refused"})
ACTION_REFUSED_CODES = _Vocab(
    {"invalid_body", "wrong_type", "out_of_range", "unknown_option", "disabled", "body_too_large"}
)
ACTION_PATH_PREFIX = "/api/v1/actions/"


def _is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _value_matches_control(control: str, value: object) -> bool:
    if control == "switch":
        return _is_bool(value)
    if control == "select":
        return _is_str(value)
    if control == "number":
        return _is_number(value)
    return False


def _validate_action_result(result: object, control: object, path: str, violations: list[Violation]) -> None:
    """Spec section 5.5: result = {state, why, age_ms, value}; why is always an object."""
    if result is None:
        return
    if not isinstance(result, dict):
        violations.append(Violation(path, "wrong_type", "result must be an object or null"))
        return
    for key in ACTION_RESULT_KEYS:
        if key not in result:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(result, ACTION_RESULT_KEYS, path, "result", violations)

    state = result.get("state")
    if "state" in result and state not in ACTION_RESULT_STATES:
        violations.append(Violation(f"{path}.state", "unknown_result_state", f"result state {state!r}"))

    if "why" in result:
        why = result["why"]
        if not isinstance(why, dict):
            violations.append(
                Violation(f"{path}.why", "result_why_not_object", "result.why must always be an object")
            )
        else:
            _validate_why_block(why, f"{path}.why", violations)
            code = why.get("code")
            if state == "running" and code != "queued":
                violations.append(Violation(f"{path}.why", "result_why_mismatch", f"running needs queued, got {code!r}"))
            elif state == "accepted" and code != "applied":
                violations.append(Violation(f"{path}.why", "result_why_mismatch", f"accepted needs applied, got {code!r}"))
            elif state == "refused" and code not in ACTION_REFUSED_CODES:
                violations.append(
                    Violation(f"{path}.why", "result_why_mismatch", f"refused needs one of the refusal codes, got {code!r}")
                )

    if "age_ms" in result and not (_is_int(result["age_ms"]) and result["age_ms"] >= 0):
        violations.append(Violation(f"{path}.age_ms", "wrong_type", "age_ms must be int>=0"))

    if "value" in result:
        value = result["value"]
        if value is not None and isinstance(control, str) and control in ACTION_CONTROLS:
            if not _value_matches_control(control, value):
                violations.append(
                    Violation(f"{path}.value", "result_value_type", f"{value!r} does not fit control {control!r}")
                )
        elif value is not None and not isinstance(value, (bool, int, float, str)):
            violations.append(Violation(f"{path}.value", "wrong_type", "result.value must be a scalar or null"))


def _validate_action_extension(
    action: dict, enabled: object, path: str, violations: list[Violation]
) -> None:
    """Spec section 5.4: the extension keys, once ``control`` is present."""
    control = action["control"]
    if not _is_str(control) or control not in ACTION_CONTROLS:
        violations.append(Violation(f"{path}.control", "unknown_control", f"control {control!r}"))
        return

    for key in ACTION_EXT_KEYS:
        if key not in action:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))

    _check_key_order(action, ACTION_KEYS + ACTION_EXT_KEYS, path, "action", violations)

    if action.get("method") != "POST":
        violations.append(Violation(f"{path}.method", "action_method_not_post", f"method {action.get('method')!r}"))

    if "body" in action and action["body"] != ACTION_BODY_BY_CONTROL[control]:
        violations.append(
            Violation(f"{path}.body", "action_body_mismatch", f"body {action['body']!r} for control {control!r}")
        )

    value = action.get("value")
    options = action.get("options")
    if control in ("switch", "button") and enabled is False:
        violations.append(
            Violation(f"{path}.enabled", "action_always_enabled", f"{control} actions are always enabled")
        )

    if control in ("select", "number") and "enabled" in action and _is_bool(enabled):
        has_state = value is not None
        if enabled and not has_state:
            violations.append(Violation(path, "action_value_mismatch", "enabled action without a state"))
        if not enabled and has_state:
            violations.append(Violation(path, "action_value_mismatch", "disabled action still carries a value"))
        why = action.get("why")
        if not enabled and isinstance(why, dict) and why.get("code") != "entity_unavailable":
            violations.append(
                Violation(f"{path}.why", "action_why_code", f"disabled {control} needs entity_unavailable")
            )

    if "value" in action and value is not None and not _value_matches_control(control, value):
        violations.append(Violation(f"{path}.value", "action_value_type", f"{value!r} does not fit control {control!r}"))
    if control == "button" and value is not None:
        violations.append(Violation(f"{path}.value", "action_value_type", "button carries no value"))

    if "options" in action:
        if control == "select":
            if not _is_list_of_str(options) or len(options) == 0:
                violations.append(
                    Violation(f"{path}.options", "wrong_type", "select options must be a non-empty list of strings")
                )
        elif options is not None:
            violations.append(Violation(f"{path}.options", "action_field_not_applicable", "options only on select"))

    for key in ("min", "max", "step"):
        if key not in action:
            continue
        v = action[key]
        if control == "number":
            if not _is_number(v):
                violations.append(Violation(f"{path}.{key}", "wrong_type", f"number {key} must be a number, never null"))
        elif v is not None:
            violations.append(Violation(f"{path}.{key}", "wrong_type", f"{key} is null unless control is number"))
    if control == "number" and all(_is_number(action.get(k)) for k in ("min", "max", "step")):
        if action["min"] > action["max"]:
            violations.append(Violation(f"{path}.min", "action_range_inverted", "min > max"))
        if action["step"] <= 0:
            violations.append(Violation(f"{path}.step", "action_step_not_positive", "step must be > 0"))

    if "unit" in action:
        unit = action["unit"]
        if control == "number":
            if not _is_str_or_null(unit):
                violations.append(Violation(f"{path}.unit", "wrong_type", "unit must be str or null"))
        elif unit is not None:
            violations.append(Violation(f"{path}.unit", "action_field_not_applicable", "unit only on number"))

    if "result" in action:
        _validate_action_result(action["result"], control, f"{path}.result", violations)


def _validate_action(
    action: object, path: str, violations: list[Violation], node_id: object = None
) -> None:
    if not isinstance(action, dict):
        violations.append(Violation(path, "wrong_type", "action is not an object"))
        return

    for key in ACTION_KEYS:
        if key not in action:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))

    for key in ("key", "label"):
        if key in action and (not _is_str(action[key]) or action[key] == ""):
            violations.append(
                Violation(f"{path}.{key}", "wrong_type", f"{key} must be a non-empty string")
            )

    if "method" in action and action["method"] not in _METHODS:
        violations.append(
            Violation(f"{path}.method", "unknown_method", f"unknown method {action['method']!r}")
        )

    if "path" in action:
        action_path = action["path"]
        if not _is_str(action_path) or not action_path.startswith("/"):
            violations.append(
                Violation(f"{path}.path", "bad_action_path", f"{action_path!r} lacks a leading /")
            )
        elif "control" in action and _is_str(node_id) and _is_str(action.get("key")):
            want = f"{ACTION_PATH_PREFIX}{node_id}/{action['key']}"
            if action_path != want:
                violations.append(
                    Violation(f"{path}.path", "bad_action_path", f"{action_path!r} != {want!r}")
                )

    enabled = action.get("enabled")
    if "enabled" in action and not _is_bool(enabled):
        violations.append(Violation(f"{path}.enabled", "wrong_type", "enabled must be a bool"))

    if "why" in action:
        why = action["why"]
        is_enabled = enabled is True
        if is_enabled and why is not None:
            violations.append(Violation(f"{path}.why", "action_why_mismatch", "why must be null"))
        elif not is_enabled and why is None:
            violations.append(
                Violation(f"{path}.why", "action_why_mismatch", "disabled action needs a why")
            )
        # A why is a why wherever it appears: a disabled action carries a machine-readable
        # code, not prose.
        if isinstance(why, dict):
            _validate_why_block(why, f"{path}.why", violations)
        elif why is not None:
            violations.append(Violation(f"{path}.why", "wrong_type", "why must be an object"))

    if "control" in action:
        _validate_action_extension(action, enabled, path, violations)
    else:
        stray = [k for k in ACTION_EXT_KEYS if k in action]
        if stray:
            violations.append(
                Violation(path, "action_extension_without_control", f"{stray} present without 'control'")
            )


LINK_KEYS: tuple[str, ...] = ("rel", "label", "href")


def _validate_links(links: object, violations: list[Violation]) -> None:
    """Spec section 5.1: ``links`` is a list of {rel, label, href}, all non-empty strings, href
    starting with '/'. Optional (older producers)."""
    if not isinstance(links, list):
        violations.append(Violation("links", "links_wrong_type", "links must be a list"))
        return
    for i, link in enumerate(links):
        path = f"links[{i}]"
        if not isinstance(link, dict):
            violations.append(Violation(path, "links_wrong_type", "link must be an object"))
            continue
        for key in LINK_KEYS:
            if key not in link:
                violations.append(Violation(path, "link_missing_key", f"missing {key!r}"))
            elif not _is_str(link[key]) or link[key] == "":
                violations.append(Violation(f"{path}.{key}", "link_empty_field", f"{key} must be a non-empty string"))
        for key in link:
            if key not in LINK_KEYS:
                violations.append(Violation(path, "unknown_key", f"unexpected link key {key!r}"))
        href = link.get("href")
        if _is_str(href) and href != "" and not href.startswith("/"):
            violations.append(Violation(f"{path}.href", "link_href_not_relative", f"href {href!r} must start with /"))


def _check_key_order(
    obj: dict, expected: tuple[str, ...], path: str, label: str, violations: list[Violation]
) -> None:
    """Spec section 5.6: key sets are closed (a key outside required + optional is
    ``unknown_key``); the ORDER of keys is not checked."""
    for key in obj:
        if key not in expected:
            violations.append(Violation(path, "unknown_key", f"unexpected {label} key {key!r}"))


# --- served_by / peers ------------------------------------------------------------


def _validate_served_by(served_by: object, path: str, violations: list[Violation]) -> list[str]:
    if not isinstance(served_by, dict):
        violations.append(Violation(path, "wrong_type", "served_by must be an object"))
        return []

    for key in SERVED_BY_KEYS:
        if key not in served_by:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(served_by, SERVED_BY_KEYS, path, "served_by", violations)

    for key in ("board", "node_id", "mac", "build"):
        if key in served_by and not _is_str_or_null(served_by[key]):
            violations.append(
                Violation(f"{path}.{key}", "wrong_type", f"{key} must be str or null")
            )

    state = served_by.get("state")
    if "state" in served_by and state not in STATES:
        violations.append(Violation(f"{path}.state", "unknown_state", f"unknown state {state!r}"))

    why = served_by.get("why")
    if "why" in served_by:
        _check_why(state == "ok", why, f"{path}.why", violations)

    if "uptime_ms" in served_by and not _is_nonneg_int_or_null(served_by["uptime_ms"]):
        violations.append(
            Violation(f"{path}.uptime_ms", "wrong_type", "uptime_ms must be int>=0 or null")
        )

    owns = served_by.get("owns")
    owns_ok = _is_list_of_str(owns)
    if "owns" in served_by and not owns_ok:
        violations.append(Violation(f"{path}.owns", "wrong_type", "owns must be a list of str"))

    if "components" in served_by and not _is_list_of_str(served_by["components"]):
        violations.append(
            Violation(f"{path}.components", "wrong_type", "components must be a list of str")
        )

    board = served_by.get("board")
    node_id = served_by.get("node_id")
    mac = served_by.get("mac")

    if state == "unknown":
        why_code = why.get("code") if isinstance(why, dict) else None
        identity_clear = board is None and node_id is None and mac is None
        owns_empty = owns_ok and len(owns) == 0
        if not (identity_clear and owns_empty and why_code == "no_identity"):
            violations.append(
                Violation(
                    path,
                    "no_identity_inconsistent",
                    "state unknown requires null identity, empty owns, why.code no_identity",
                )
            )

    if state == "ok":
        identity_complete = board is not None and node_id is not None and mac is not None
        owns_nonempty = owns_ok and len(owns) > 0
        if not (identity_complete and owns_nonempty):
            violations.append(
                Violation(
                    path,
                    "identity_incomplete",
                    "state ok requires non-null identity and non-empty owns",
                )
            )

    return owns if owns_ok else []


def _validate_peer(
    peer: object, envelope_schema_version: object, path: str, violations: list[Violation]
) -> list[str]:
    if not isinstance(peer, dict):
        violations.append(Violation(path, "wrong_type", "peer is not an object"))
        return []

    for key in PEER_KEYS:
        if key not in peer:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))
    _check_key_order(peer, PEER_KEYS, path, "peer", violations)

    state = peer.get("state")
    if "state" in peer and state not in STATES:
        violations.append(Violation(f"{path}.state", "unknown_state", f"unknown state {state!r}"))

    why = peer.get("why")
    if "why" in peer:
        _check_why(state == "ok", why, f"{path}.why", violations)

    mounts = peer.get("mounts")
    mounts_ok = _is_list_of_str(mounts)
    if "mounts" in peer:
        if not mounts_ok:
            violations.append(
                Violation(f"{path}.mounts", "wrong_type", "mounts must be a list of str")
            )
        elif len(mounts) == 0:
            violations.append(
                Violation(f"{path}.mounts", "empty_mounts", "mounts must be non-empty")
            )

    age_ms = peer.get("age_ms")
    if "age_ms" in peer and not _is_nonneg_int_or_null(age_ms):
        violations.append(
            Violation(f"{path}.age_ms", "wrong_type", "age_ms must be int>=0 or null")
        )
    if "last_ok_age_ms" in peer and not _is_nonneg_int_or_null(peer["last_ok_age_ms"]):
        violations.append(
            Violation(f"{path}.last_ok_age_ms", "wrong_type", "must be int>=0 or null")
        )

    if state == "unreachable" and age_ms is not None:
        violations.append(
            Violation(path, "peer_unreachable_has_age", "unreachable peer carries a live age_ms")
        )

    peer_schema_version = peer.get("schema_version")
    if _is_int(peer_schema_version) and _is_int(envelope_schema_version):
        why_code = why.get("code") if isinstance(why, dict) else None
        if peer_schema_version > envelope_schema_version:
            if state != "degraded" or why_code != "schema_skew":
                violations.append(
                    Violation(
                        path,
                        "peer_skew_newer",
                        "newer peer requires state degraded, why.code schema_skew",
                    )
                )
        elif peer_schema_version < envelope_schema_version:
            if state != "incompatible" or why_code != "schema_skew":
                violations.append(
                    Violation(
                        path,
                        "peer_skew_older",
                        "older peer requires state incompatible, why.code schema_skew",
                    )
                )

    return mounts if mounts_ok else []


# --- nodes -------------------------------------------------------------------------


def _check_hierarchical_id(
    child_id: str, parent_id: str, parent_is_car_root: bool, path: str, violations: list[Violation]
) -> None:
    if parent_is_car_root:
        if "." in child_id:
            violations.append(
                Violation(path, "id_not_hierarchical", f"{child_id!r} must be one dotless segment")
            )
        return
    prefix = parent_id + "."
    rest = child_id[len(prefix) :] if child_id.startswith(prefix) else None
    if not rest or "." in rest:
        violations.append(
            Violation(
                path,
                "id_not_hierarchical",
                f"{child_id!r} must be {parent_id!r} + '.' + one dotless segment",
            )
        )


def _validate_node(
    node: object,
    path: str,
    parent_id: str | None,
    parent_is_car_root: bool,
    is_root: bool,
    ids_seen: set[str],
    violations: list[Violation],
) -> None:
    if not isinstance(node, dict):
        violations.append(Violation(path, "wrong_type", "node is not an object"))
        return

    for key in NODE_KEYS:
        if key not in node:
            violations.append(Violation(path, "missing_key", f"missing {key!r}"))

    _check_key_order(node, NODE_KEYS + OPTIONAL_NODE_KEYS, path, "node", violations)

    node_id = node.get("id")
    if "id" in node:
        if not _is_str(node_id) or node_id == "":
            violations.append(
                Violation(f"{path}.id", "wrong_type", "id must be a non-empty string")
            )
        else:
            if node_id in ids_seen:
                violations.append(Violation(f"{path}.id", "duplicate_id", f"id {node_id!r} reused"))
            ids_seen.add(node_id)
            if parent_id is not None:
                _check_hierarchical_id(
                    node_id, parent_id, parent_is_car_root, f"{path}.id", violations
                )

    if "kind" in node and (not _is_str(node["kind"]) or node["kind"] == ""):
        violations.append(
            Violation(f"{path}.kind", "wrong_type", "kind must be a non-empty string")
        )

    if "label" in node:
        label = node["label"]
        if not _is_str(label):
            violations.append(Violation(f"{path}.label", "wrong_type", "label must be a string"))
        elif label == "":
            violations.append(Violation(f"{path}.label", "empty_label", "label must not be empty"))

    state = node.get("state")
    if "state" in node and state not in STATES:
        violations.append(Violation(f"{path}.state", "unknown_state", f"unknown state {state!r}"))

    if "why" in node:
        _check_why(state == "ok", node["why"], f"{path}.why", violations)

    if "provenance" in node:
        _validate_provenance(node["provenance"], state, f"{path}.provenance", violations)

    if state in ("silent", "absent"):
        _check_frames_evidence(state, node.get("why"), path, violations)
    _check_announced_evidence(state, node.get("why"), path, violations)

    values = node.get("values")
    values_ok = isinstance(values, list)
    if "values" in node:
        if not values_ok:
            violations.append(Violation(f"{path}.values", "wrong_type", "values must be a list"))
        else:
            any_failing = False
            for i, v in enumerate(values):
                _validate_value(v, f"{path}.values[{i}]", violations)
                if isinstance(v, dict) and v.get("state") != "ok":
                    any_failing = True
            if state == "ok" and any_failing:
                violations.append(
                    Violation(
                        f"{path}.values",
                        "node_ok_with_failing_value",
                        "node state ok but a value is not",
                    )
                )
            if state == "ok" and any(isinstance(v, dict) and v.get("level") in ("warn", "bad") for v in values):
                violations.append(
                    Violation(f"{path}.values", "node_ok_with_limit", "node state ok but a value is at warn/bad")
                )
    _check_v15_why(node, state, values if values_ok else [], path, violations)

    if "series" in node:
        _validate_series(node["series"], f"{path}.series", violations)

    children = node.get("children")
    if "children" in node:
        if not isinstance(children, list):
            violations.append(
                Violation(f"{path}.children", "wrong_type", "children must be a list")
            )
        else:
            child_parent_id = node_id if _is_str(node_id) and node_id != "" else None
            child_is_car_root = is_root and node_id == "car"
            for i, child in enumerate(children):
                _validate_node(
                    child,
                    f"{path}.children[{i}]",
                    child_parent_id,
                    child_is_car_root,
                    False,
                    ids_seen,
                    violations,
                )

    if "actions" in node:
        actions = node["actions"]
        if not isinstance(actions, list):
            violations.append(Violation(f"{path}.actions", "wrong_type", "actions must be a list"))
        else:
            for i, a in enumerate(actions):
                _validate_action(a, f"{path}.actions[{i}]", violations, node_id)

    if "role" in node:
        role = node["role"]
        roles_available = node.get("roles_available")
        roles_ok = (
            isinstance(roles_available, list)
            and len(roles_available) > 0
            and all(_is_str(x) for x in roles_available)
        )
        if not roles_ok:
            violations.append(
                Violation(path, "role_not_available", "role needs a non-empty roles_available list")
            )
        elif role is not None and role not in roles_available:
            violations.append(
                Violation(f"{path}.role", "role_not_available", f"{role!r} not in roles_available")
            )


def _check_v15_why(node: dict, state: object, values: list, path: str, violations: list[Violation]) -> None:
    """Spec 5.3/5.6 (v1.5): `limit`, `not_requested` and `switched_off` must agree with the node."""
    why = node.get("why")
    if not isinstance(why, dict):
        return
    code = why.get("code")
    evidence = why.get("evidence")
    if code == "limit":
        ok = isinstance(evidence, dict)
        ok = ok and _is_str(evidence.get("value")) and evidence.get("level") in ("warn", "bad")
        ok = ok and "reading" in evidence and isinstance(evidence.get("bound"), dict)
        if ok:
            named = [v for v in values if isinstance(v, dict) and v.get("key") == evidence["value"]]
            if named and "level" in named[0] and named[0]["level"] != evidence["level"]:
                ok = False
        if not ok:
            violations.append(
                Violation(
                    f"{path}.why.evidence",
                    "limit_evidence_mismatch",
                    "limit needs evidence {value, level warn|bad, reading, bound} agreeing with that value's level",
                )
            )
    elif code == "not_requested":
        if state != "unknown" or any(isinstance(v, dict) and v.get("value") is not None for v in values):
            violations.append(
                Violation(path, "not_requested_inconsistent", "not_requested needs state unknown and no reading")
            )
    elif code == "switched_off" and state != "absent":
        violations.append(Violation(path, "switched_off_inconsistent", f"switched_off on state {state!r}"))


# --- ancestor_role ----------------------------------------------------------------
# Spec section 5.1. Envelope-level,
# paired with `path` (checked by the caller, which has both keys in scope); this function
# only judges the value's own shape and, given the tree's own id set, whether it lies.


def _validate_ancestor_role(
    ancestor_role: object, ids_in_tree: set[str], path: str, violations: list[Violation]
) -> None:
    if ancestor_role is None:
        return
    if not isinstance(ancestor_role, dict):
        violations.append(
            Violation(path, "ancestor_role_wrong_type", "ancestor_role must be null or an object")
        )
        return

    extra_keys = sorted(set(ancestor_role) - set(ANCESTOR_ROLE_KEYS))
    if extra_keys:
        violations.append(
            Violation(path, "ancestor_role_wrong_type", f"unexpected key(s) {extra_keys}")
        )

    shape_ok = True
    for key in ANCESTOR_ROLE_KEYS:
        value = ancestor_role.get(key)
        if key not in ancestor_role or not _is_str(value) or value == "":
            violations.append(
                Violation(
                    f"{path}.{key}", "ancestor_role_wrong_type", f"{key} must be a non-empty string"
                )
            )
            shape_ok = False

    if shape_ok and ancestor_role["node"] in ids_in_tree:
        violations.append(
            Violation(
                f"{path}.node",
                "ancestor_role_in_tree",
                f"{ancestor_role['node']!r} is declared as an ancestor but appears in the served tree",
            )
        )


# --- series_legend ---------------------------------------------------------------
# Spec section 5.1: always present, an object naming every ``class`` character used
# in THIS envelope (narrowed or not -- "this envelope", not "this board"), ``null`` only when
# the envelope carries no series at all. Run after the tree walk because it needs the tree.


def _validate_series_legend(envelope: dict, violations: list[Violation]) -> None:
    if "series_legend" not in envelope:
        return
    legend = envelope["series_legend"]
    legend_is_dict = isinstance(legend, dict)
    if legend is not None and not legend_is_dict:
        violations.append(
            Violation("series_legend", "series_legend_wrong_type", "must be null or an object")
        )
    elif legend_is_dict:
        for key, value in legend.items():
            key_ok = isinstance(key, str) and len(key) == 1 and key in CLASS_CHARS
            value_ok = isinstance(value, str) and value != ""
            if not (key_ok and value_ok):
                violations.append(
                    Violation(
                        "series_legend",
                        "series_legend_wrong_type",
                        f"bad legend entry {key!r}: {value!r}",
                    )
                )

    if legend_is_dict and legend != SERIES_LEGEND:
        violations.append(
            Violation(
                "series_legend",
                "series_legend_not_canonical",
                f"series_legend must be exactly {SERIES_LEGEND!r}",
            )
        )

    tree = envelope.get("tree")
    series_present = False
    used_chars: set[str] = set()
    if isinstance(tree, dict):
        for _, node in iter_nodes(tree):
            series = node.get("series")
            if isinstance(series, dict):
                series_present = True
                cls = series.get("class")
                if isinstance(cls, str):
                    used_chars.update(cls)

    if series_present and legend is None:
        violations.append(
            Violation(
                "series_legend",
                "series_legend_missing",
                "envelope carries a series but series_legend is null",
            )
        )
    if not series_present and legend is not None:
        violations.append(
            Violation(
                "series_legend",
                "series_legend_without_series",
                "series_legend is non-null but the envelope carries no series",
            )
        )
    if legend_is_dict:
        missing = sorted(c for c in used_chars if c not in legend)
        if missing:
            violations.append(
                Violation(
                    "series_legend",
                    "series_legend_incomplete",
                    f"class char(s) {missing} used by a series but not named by series_legend",
                )
            )


# --- envelope ------------------------------------------------------------------


def validate_envelope(envelope: object) -> list[Violation]:
    """Check one portal state envelope. Never raises: every failure mode of the input
    — wrong type, missing key, wrong shape, at any depth — comes back as a Violation.
    Collects everything it can; a container of the wrong type is reported once and not
    descended into, so one bad shape does not manufacture a cascade of children."""
    violations: list[Violation] = []
    if not isinstance(envelope, dict):
        violations.append(Violation("", "not_an_object", "top-level envelope is not an object"))
        return violations

    for key in ENVELOPE_KEYS:
        if key not in envelope:
            violations.append(Violation("", "missing_key", f"envelope missing {key!r}"))

    _check_key_order(
        envelope,
        ENVELOPE_KEYS[:-1] + OPTIONAL_ENVELOPE_KEYS + ENVELOPE_KEYS[-1:],
        "",
        "envelope",
        violations,
    )

    if "links" in envelope:
        _validate_links(envelope["links"], violations)

    schema_version = envelope.get("schema_version")
    if "schema_version" in envelope:
        if not _is_int(schema_version):
            violations.append(Violation("schema_version", "wrong_type", "must be an int"))
        elif schema_version != SCHEMA_VERSION:
            violations.append(
                Violation(
                    "schema_version",
                    "unsupported_version",
                    f"{schema_version} is not supported (this validator handles {SCHEMA_VERSION})",
                )
            )
            return violations

    if "seq" in envelope:
        seq = envelope["seq"]
        if not _is_int(seq):
            violations.append(Violation("seq", "wrong_type", "seq must be an int"))
        elif seq < 0:
            violations.append(Violation("seq", "negative", "seq must be >= 0"))

    has_path = "path" in envelope
    has_ancestor_role = "ancestor_role" in envelope
    tree = envelope.get("tree")
    if has_path:
        path_value = envelope["path"]
        tree_id = tree.get("id") if isinstance(tree, dict) else None
        if not (_is_str(path_value) and path_value == tree_id):
            violations.append(Violation("path", "path_mismatch", f"path {path_value!r} != tree.id"))

    # Spec section 5.1: `path` and `ancestor_role` are emitted together or not at all --
    # a narrowed envelope with no opinion on its ancestor is a producer that predates this
    # key, not a producer that has nothing to say (that case is the value `null`, not
    # the key's absence).
    if has_path and not has_ancestor_role:
        violations.append(
            Violation(
                "ancestor_role", "path_without_ancestor_role", "path is present but ancestor_role is absent"
            )
        )
    if has_ancestor_role and not has_path:
        violations.append(
            Violation(
                "path", "ancestor_role_without_path", "ancestor_role is present but path is absent"
            )
        )

    owns: list[str] = []
    if "served_by" in envelope:
        owns = _validate_served_by(envelope["served_by"], "served_by", violations)

    peer_mounts: list[tuple[str, str]] = []
    if "peers" in envelope:
        peers = envelope["peers"]
        if not isinstance(peers, list):
            violations.append(Violation("peers", "wrong_type", "peers must be a list"))
        else:
            for i, peer in enumerate(peers):
                mounts = _validate_peer(peer, schema_version, f"peers[{i}]", violations)
                peer_mounts.extend((f"peers[{i}].mounts", m) for m in mounts)

    ids_seen: set[str] = set()
    if "tree" in envelope:
        if not isinstance(tree, dict):
            violations.append(Violation("tree", "wrong_type", "tree must be an object"))
        else:
            root_id = tree.get("id")
            if not has_path and _is_str(root_id) and "." in root_id:
                violations.append(Violation("tree.id", "root_id_dotted", f"root id {root_id!r}"))
            _validate_node(tree, "tree", None, False, True, ids_seen, violations)

            if not has_path:
                mount_refs = [("served_by.owns", m) for m in owns] + peer_mounts
                for src_path, mount_id in mount_refs:
                    if mount_id not in ids_seen:
                        violations.append(
                            Violation(src_path, "mount_unresolved", f"{mount_id!r} not in tree")
                        )

            # Applies to narrowed responses too (served_by.owns survives
            # ?path=, and a producer copies served_by/peers verbatim into a narrowed envelope).
            roots = set(owns) | {mount_id for _, mount_id in peer_mounts}
            served_by_obj = envelope.get("served_by")
            served_by_board = served_by_obj.get("board") if isinstance(served_by_obj, dict) else None
            _validate_provenance_placement(tree, roots, served_by_board, violations, "tree")

            # C10: applies to narrowed responses too (id-based, not depth-based).
            _validate_mount_ages(tree, envelope.get("peers"), violations)

    if has_ancestor_role:
        _validate_ancestor_role(envelope["ancestor_role"], ids_seen, "ancestor_role", violations)

    _validate_series_legend(envelope, violations)

    return violations


def check_envelope(envelope: object) -> None:
    """Like ``validate_envelope``, but raises ``SchemaViolationError`` when anything failed."""
    violations = validate_envelope(envelope)
    if violations:
        raise SchemaViolationError(violations)


def iter_nodes(node: dict) -> Iterator[tuple[str, dict]]:
    """Depth-first, root first. The root's own path is ``"tree"``, matching the paths
    ``validate_envelope`` reports so a violation's path can be fed straight back here."""

    def _walk(n: object, path: str) -> Iterator[tuple[str, dict]]:
        if not isinstance(n, dict):
            return
        yield path, n
        children = n.get("children")
        if isinstance(children, list):
            for i, child in enumerate(children):
                yield from _walk(child, f"{path}.children[{i}]")

    yield from _walk(node, "tree")


def find_node(tree: dict, node_id: str) -> dict | None:
    """First node in ``tree`` whose ``id`` equals ``node_id``, or None."""
    for _, node in iter_nodes(tree):
        if node.get("id") == node_id:
            return node
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate portal state envelopes against docs/portal-spec.md section 5 (v2 only).",
    )
    parser.add_argument("files", nargs="+", help="envelope JSON file(s) to check")
    parser.add_argument("-q", "--quiet", action="store_true", help="suppress 'OK <path>' lines")
    args = parser.parse_args(argv)

    exit_code = 0
    for file_path in args.files:
        try:
            with open(file_path, encoding="utf-8") as handle:
                envelope = json.load(handle)
        except (OSError, json.JSONDecodeError) as err:
            print(f"{file_path}: {err}")
            exit_code = 1
            continue

        violations = validate_envelope(envelope)
        if not violations:
            if not args.quiet:
                print(f"OK {file_path}")
            continue
        exit_code = 1
        for violation in violations:
            print(f"{file_path}: {violation}")

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
