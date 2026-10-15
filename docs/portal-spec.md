# `portal` — specification v1

One self-describing state tree per device, served as one web page plus a JSON API from
ESPHome's shared `web_server_base`, with optional controls (actions) and an optional login
that guards only those controls.

This document is the contract between implementer and tester. Behaviour that is not here is
not required; behaviour that contradicts it is a bug in one of the two, and the spec owner
decides which (see §11).

**Status:** v1.5, 2026-10-14 (v1.1: access model and AP behaviour decided, §6, §6.3, §10;
v1.2: provider body contract §4.6, number traits non-null §5.4, validator contract §5.6;
v1.3: rejections outside the portal and 503 §6.2, shutdown rule §6.2, kind views §7;
v1.4: page repaint hold and local clocks §7, from the bench;
v1.5: UI hints injected into the page §4.9, value limits and `level` §4.3/§5.3, `on_demand` /
`not_requested` §4.1/§5.3, the built-in absent provider `switch_off` §4.6, derivation
precedence §5.3, validator rules §5.6). Origin: the `webui_node` component of a private bench-simulator
project (wire format `schema_version: 2`), lifted here and made vehicle-neutral. Nothing in
this repository may name that project, its boards, MACs, IPs or captures
(`script/check_no_private_data.py`, `docs/CONVENTIONS.md`).

---

## 1. Goals and non-goals

**Goals**

- G1 One generic ESPHome external component, `components/portal/`, that serves a declared
  tree of nodes (values, series, children, actions) at `GET /api/v1/state` and a single
  embedded HTML page at `GET /`.
- G2 Wire format of `/api/v1/state` stays compatible with `schema_version: 2` consumers:
  every key and every rule of §5 that existed before still holds; new information is added
  only as new keys: inside `actions[]` entries, the envelope's `links` and a value's `level`
  (all ignored by a consumer that does not know them).
- G3 Vehicle-neutral: nothing in the component, the asset or its tests names a specific
  vehicle, board or bench. Position (`owns`) is declared in YAML, never derived from which
  components are compiled.
- G4 One httpd per device: the portal is an `AsyncWebHandler` on `web_server_base`, shared
  with `captive_portal` (§8).
- G5 Actions: a node may carry controls bound to ESPHome `switch`, `button`, `select` and
  `number` entities. Actions exist only when declared in YAML (nothing is controllable by
  default), and every invocation resolves into the tree (accepted / refused / running, with
  reason and age).
- G6 The device can never be locked out by its own configuration: viewing (`/`,
  `/api/v1/state`) never needs a login, and the optional `auth:` guards only the actions (§6).

**Non-goals (v1)**

- Peer mounting (`peers` stays `[]`), runtime roles, live CAN frame views.
- Any built-in data provider for series, and any absence provider beyond `switch_off` (§4.6).
- Arduino or non-ESP32 targets.

---

## 2. File ownership (three agents, one worktree)

| owner | paths |
|---|---|
| implementer | `components/portal/**` (Python, C++, `index.html`) |
| tester | `tests/portal/**`, `tests/host/*portal*`, `tests/build/portal/**`, `script/portal_schema.py`, `tests/host/Makefile` (include path only), `.github/workflows/ci.yml` (compile-matrix entries only) |
| spec owner | `docs/portal-spec.md`, `docs/**` for the portal, `README.md` component list, `docs/HANDOVER.md` portal entry |

Commit only your own paths: `git commit <paths>`; never `git add -A` / `git commit -a`.
Questions go to the team board; behaviour changes land here first (§11).

---

## 3. Component layout

```
components/portal/
  __init__.py      config schema, final validation, codegen, provider registry
  portal.h/.cpp    ESPHome binding: AsyncWebHandler, sampling, actions queue
  portal_core.h    freestanding core: <cstddef>, <cstdint>, <cstring> only, no ESPHome/IDF,
                   no allocation. Resolver, ChunkWriter, envelope emitter, fingerprint,
                   action-body parser, action-result emitter. Namespace esphome::portal::core
  index.html       the built-in page (gzipped into flash at codegen)
```

The freestanding core is what the host tests compile (`make -C tests/host`). Everything the
host can test (JSON shape, state derivation, request parsing, action validation, result
emission, fingerprint) lives in `portal_core.h`; `portal.cpp` only binds entities and HTTP.

---

## 4. YAML surface

```yaml
portal:
  id: portal                   # optional
  port: 80                     # optional, default 80; sets web_server_base's port
  build: "demo-1"              # required, string -> served_by.build
  board: demo                  # optional identity (all three or the identity is "unknown")
  node_id: 0x0A                # optional; int or int-string, normalised to "0xA"
  mac: "02:00:00:00:00:01"     # optional; 6 hex octets, normalised upper-case
  owns: [vehicle]              # optional list of node ids (see 4.2)
  root_label: Vehicle          # optional, default "Vehicle"; label of the synthetic root
  text_max: 48                 # optional 16..256, default 48; text value cap in bytes
  submodule_size: 12           # optional int >= 1; required iff any node declares submodule_position
  web_asset: my_page.html      # optional path (relative to the YAML); replaces index.html
  lang: en                     # optional en (default) | de; page render texts only (4.9)
  layout:                      # optional; static UI hints for the page (4.9)
    tabs: [ <tab>, ... ]
  auth:                        # optional, also with actions; guards only POST actions
    username: !secret web_username
    password: !secret web_password
  tree: [ <node>, ... ]        # required, >= 1 node; the children of the synthetic root
```

### 4.1 Node

| key | type | rule |
|---|---|---|
| `segment` | `[a-z][a-z0-9_]*` | required; unique among siblings |
| `kind` | non-empty string | required; open vocabulary |
| `label` | non-empty string | required |
| `budget_ms` | int >= 1, or null (`null`/`~`/empty/`none`) | optional; omitted -> derived (4.4) |
| `values` | list of value (4.3), >= 1 | optional |
| `series` | series (4.5) | optional |
| `children` | list of node, >= 1 | optional |
| `actions` | list of action (4.7), >= 1 | optional |
| `role` / `roles_available` | identifier / list of identifiers | `role` requires a non-empty `roles_available` containing it |
| `absent` | why-code | optional; node is `absent` with this code |
| `absent_when` | `{provider, source, <provider args>}` | requires `absent`; provider from the registry (4.6) |
| `unknown_when` | `{value, equals, why}` | `value` must be one of this node's own value keys; not combinable with `children` |
| `trust_from` | value key | must be one of this node's own value keys |
| `submodule_position` | int >= 1 | optional; unique across the whole tree |
| `on_demand` | bool, default false | the node's values are only published when asked for (5.3 `not_requested`); requires `values` |
| `borrow` | list of `{action, before?}` | optional, UI hint only (4.9): render another node's action on this node's page |

Node ids are dotted paths: a top-level node's id is its `segment`, a child's id is
`<parent id>.<segment>`. The synthetic root has id `car`, kind `car`, label `root_label`
(id and kind stay `car` for wire compatibility; the root's children carry bare ids).

Cross-field rules (each a config error naming the node):

- `absent` without `absent_when` is exclusive with `values`, `children`, `role`,
  `unknown_when`, `series`, `actions`. With `absent_when` only `role` and `unknown_when` stay
  exclusive: a conditionally absent node keeps its values, and they still emit while it is
  absent (classification as usual), so a page can show them as "off" with the reason.
- `absent_when` requires `absent`, unless the provider declares a default why-code
  (`switch_off`: `switched_off`, 4.6); then `absent` may be omitted and is filled in.
- `on_demand: true` without `values` is an error (a node with nothing to publish would be
  `not_requested` for ever).
- The evidence-gated why-codes `never_announced`, `no_transfer`, `crc_fail`,
  `negative_response` are refused in `unknown_when.why`, and in `absent` unless the chosen
  `absent_when` provider declares evidence for exactly that code.
- A series `key` must not equal one of the node's own value keys; an action `key` must not
  equal another action key on the same node.
- A node with any `binary_sensor` value may not declare a positive `budget_ms`.

### 4.2 `owns`

- Each entry must be the id of a node in `tree` -> else error
  `owns entry 'x' names no node in 'tree'`.
- No duplicates -> else error.
- If `board`, `node_id` and `mac` are all set, `owns` must be non-empty -> else error
  (an identity with `served_by.state: ok` must own something).
- `owns` is never derived from compiled components. `served_by.components` is still the
  sorted list of top-level component keys of the config, minus `substitutions`, `packages`,
  `esphome`, `external_components`; it is informational only.
- With an incomplete identity, `served_by.owns` is emitted as `[]` whatever YAML says (wire
  rule, §5.2).

### 4.3 Value

| key | rule |
|---|---|
| `key` | `[a-z][a-z0-9_]*`, required |
| `label` | non-empty, required |
| `unit` | string, optional (absent -> JSON null) |
| `decimals` | 0..6, default 0 |
| `cadence_ms` | int >= 1, optional; forbidden on `binary_sensor` |
| exactly one of `sensor` / `text_sensor` / `binary_sensor` | entity id |
| `expert` | bool, default false; UI hint (4.9): the page hides the value unless "expert" is on or the value is not fine |
| `warn`, `bad` | `{below: float, above: float}`, at least one key; `sensor` values only |
| `warn_when`, `bad_when` | bool; `binary_sensor` values only |

Limits (`warn`, `bad`, `warn_when`, `bad_when`) drive the value's `level` (5.3). Numeric:
`below: x` matches a reading `< x`, `above: x` a reading `> x`; `bad` is checked before `warn`.
Boolean: `bad_when: b` matches a reading `== b`, then `warn_when`. Limits on a `text_sensor`
value, numeric limits on a `binary_sensor` value and `*_when` on a `sensor` value are config
errors. The limits are compiled into flash as one constant table (one small struct per limited
value); a value names its row by a 16-bit slot that sits in existing struct padding, so limits
cost no heap and nothing per request.

### 4.4 Budget derivation

Explicit `budget_ms` (int or null) wins. Otherwise: no own values, or any own value without
`cadence_ms` -> null; else `3 * max(cadence_ms)`. The resolved value is written back into the
validated config (visible in `esphome config` output). A component-level `budget_ms` is a
config error with an explanatory message.

### 4.5 Series

| key | rule |
|---|---|
| `key`, `label` | as for values |
| `unit` | optional |
| `index_base` | int >= 0, required |
| `index_frame` | `[a-z][a-z0-9_]*` or explicit null, **required** |
| `count` | 1..64, required |
| `decimals` | 0..6, default 0 |
| `provider`, `source`, provider args | optional, all-or-nothing, from the registry (4.6) |

Without a provider every element is a hole. `pack_index_base` (wire) derivation, written back
as `resolved_pack_index_base`: walk from the series' node upward to the nearest node
declaring `submodule_position` (itself included). None found -> null. Found and
`index_frame == submodule_cell` -> `(position - 1) * submodule_size + index_base`. Found and
`index_frame` null -> config error (an unnamed frame never gets an offset). Found and any
other frame -> null. Two nodes with the same `submodule_position` -> config error. A
`submodule_position` without top-level `submodule_size` -> config error.

### 4.6 Provider registry

`components/portal/__init__.py` exposes:

```python
register_absent_provider(name, *, args: dict[str, validator], source_type: MockObjClass,
                         body: str, evidence: str | None = None, for_why: tuple[str, ...] = (),
                         default_why: str | None = None)
register_series_provider(name, *, args: dict[str, validator], source_type: MockObjClass,
                         body: str)
```

Another external component calls these at import time to add providers. The portal ships
exactly one provider of its own, `switch_off` (below). Unknown provider -> error listing the registered names. Missing/extra
provider args -> error. `source` is `cv.use_id(source_type)`. Codegen emits one thunk per
distinct (provider, args) into `main.cpp` via `cg.add_global`, named deterministically
(`portal_prov_<provider>_<argletter><value>...`), deduplicated within one build.

**`body` contract (public: other components write these).** `body` is C++ statement text,
the body of a function returning `bool`. Before emission it is expanded with Python
`str.format(**args)`, where `args` maps each declared arg name to its **validated** value
(after the arg's validator ran), so `{name}` inserts that value; a literal `{` or `}` in the
C++ text must be written `{{` / `}}`. No other placeholder exists; an unknown placeholder is a
`KeyError` at codegen, which the registering component's tests must catch. Names in scope
inside `body`:

| provider kind | thunk signature | in scope |
|---|---|---|
| absent | `bool f(void *ctx)` | `source`: `<source_type> *` cast from `ctx`, **may be `nullptr`**; return `true` = absent (a null `source` must yield `true`) |
| series | `bool f(void *ctx, uint16_t index, esphome::portal::core::SeriesElement *out)` | `source`: `<source_type> *`, never `nullptr` (the thunk returns `false` before `body` when it is); `index`: zero-based ordinal within the series; `out`: element to fill (`cls` in `r s i -`, `has_value`, `value`); return `false` = hole |

The thunk text is emitted after `#include "esphome.h"`, so every compiled component's
headers are visible; the portal includes nothing of the provider's component.

**Built-in absent provider `switch_off`** (generic, no extra args, `source`: a `switch` id):

```yaml
- segment: monitor
  values: [ ... ]                       # kept, see 4.1
  absent_when: {provider: switch_off, source: monitor_switch}
  # absent: switched_off                # optional, this is the default
```

- The node is `absent` while the switch is off (or the switch is missing), `why.code`
  `switched_off` (the provider's default why-code; any other non-gated code may be declared).
- `why.evidence` is `{"action": "<node id>/<key>"}`: the first action in the tree (pre-order,
  YAML order) whose `switch:` is that same entity, resolved at codegen. No such action ->
  `evidence: {}`. This lets a page put the switch next to the cause.
- The switch's state is part of the fingerprint (5.1), so turning it advances `seq`.

### 4.7 Action

```yaml
actions:
  - key: logging            # [a-z][a-z0-9_]*, unique per node
    label: Logging          # non-empty
    switch: logging_switch  # exactly one of switch / button / select / number
  - key: restart
    label: Restart
    button: restart_button
  - key: mode
    label: Mode
    select: mode_select
  - key: interval
    label: Interval
    number: interval_number
    decimals: 1             # number only, 0..6, default 2
    group: Settings         # optional UI hint (4.9): non-empty section name
  - key: send
    label: Send
    button: send_button
    confirm: hold           # optional UI hint (4.9): only `hold` exists
    before:                 # optional UI hint (4.9): set these first, in order
      - {action: ecus/target, value: "a"}
```

- Exactly one entity key -> else error `an action needs exactly one of ...`.
- `decimals` on a non-number action -> error.
- Actions without `auth:` are valid. Codegen logs one config-time warning
  (`actions are reachable without a login; access control is the network`), and
  `dump_config` states `auth: none`. Access control for the device is the network it sits on
  (AP password, protected LAN); `auth:` is an additional, optional layer for the actions only.
- A domain's control code compiles only when that domain exists in the build
  (`USE_SWITCH`, `USE_BUTTON`, `USE_SELECT`, `USE_NUMBER`).
- The portal never changes an entity's own state logic (`restore_mode`, optimistic, etc.).
- `confirm`, `group` and `before` are UI hints (4.9). They never change what the device does
  with a POST: a client that ignores them gets the plain action.
- `before[]`: each entry is `{action: <ref>, value: <v>}`. `<ref>` is `<node id>/<key>` of a
  `select`, `number` or `switch` action anywhere in the tree (not the action itself). `value`
  must fit the control: select a string, number a float, switch a bool. Where the target
  entity's config is known at config time, the value is checked against it too: a select's
  `options`, a number's `min_value`/`max_value`. Any mismatch is a config error naming the ref.

### 4.8 Component-level validation

- `cv.only_on_esp32`. Depends on `network`; auto-loads `web_server_base`.
- `web_server:` in the same config -> error: both serve `/` on the one `web_server_base`
  and `web_server`'s auth defines/credentials would govern the portal's. The portal replaces
  `web_server`.
- `port` 32767 and 65535 -> error (they wedge `httpd_stop()`).
- `web_asset` file missing -> error naming the path.
- `auth:` present with an empty `username` or `password` -> error (a half-configured login
  is caught at build time, never on the device). `auth:` has no other keys; `protect:` does
  not exist.
- Consumes 3 TCP sockets (`socket.consume_sockets(3, "portal")`); the listening socket is
  `web_server_base`'s.
- Sizing defines from Python: `PORTAL_MAX_NODES` (tree nodes + root), `PORTAL_MAX_VALUES`,
  `PORTAL_MAX_SERIES`, `PORTAL_MAX_ACTIONS`, `PORTAL_TEXT_MAX` (each >= 1). Feature define
  `USE_PORTAL`. With `auth:`: `USE_WEBSERVER_AUTH` and `USE_WEBSERVER_AUTH_DIGEST`.

### 4.9 UI hints (static, injected into the page at codegen)

The page-only keys (`lang`, `layout`, value `expert` and limits, action `confirm` / `group` /
`before`, node `borrow`) become one JSON object that codegen writes **into the page source**
before it is gzipped: every occurrence of the literal token `/*PORTAL_UI*/null` in the page
(built-in `index.html` or `web_asset:`) is replaced by the JSON. A page reads it as
`const UI = /*PORTAL_UI*/null;` (null = no hints, use defaults). A page without the token
(an older custom page) is embedded byte for byte as before. The hints cost flash only, no heap
and no bytes per request; the device never reads them back.

```json
{"lang": "de",
 "tabs": [{"label": "Drive", "glance": ["bat.main/soc"], "nodes": []},
          {"label": "Bus", "glance": ["can0/awake"], "nodes": ["can0", "ecus"]}],
 "values": {"can0/rec": {"expert": true, "warn": {"above": 95}, "bad": {"above": 127}},
            "can0/bus_off": {"bad_when": true}},
 "actions": {"dev/restart": {"confirm": "hold", "group": "Device"},
             "ecus/send": {"before": [{"action": "ecus/target", "value": "A"}]}},
 "borrow": {"ecus.a": [{"action": "ecus/service"},
                       {"action": "ecus/send", "before": [{"action": "ecus/target", "value": "a"}]}]}}
```

- All five top-level keys are always present. `lang` is `en` or `de`. `tabs` is `[]` when
  `layout` is omitted (the page then makes one tab per root node).
- `values` / `actions` carry an entry only for a value / action that sets at least one hint,
  and inside it only the keys that are set (`expert` only when true). Numbers that are whole
  are written as integers. `borrow` carries an entry per node that declares `borrow:`.
- Refs: a value ref and an action ref are `<node id>/<key>`, node id the dotted id the state
  JSON uses. Codegen validates every ref: an unknown ref is a config error listing the valid
  ones.
- JSON is compact, UTF-8, and every `<` is written `\u003c`, so no string can close the
  `<script>` element it lands in.

`layout`:

| key | rule |
|---|---|
| `tabs` | list of 0..5 tabs |
| tab `label` | non-empty string, required |
| tab `glance` | list of 0..4 value refs, default `[]` |
| tab `nodes` | list of node ids (dotted), default `[]` |

`borrow` (node key): a list of `{action: <action ref>, before: [...]}` (`before` optional, same
rules as the action-level `before`, 4.7). The page renders the referenced action (any control,
from any node) on this node's page and, when it is used there, first sets the `before`
entries. This is a hint only: no runtime action, no heap; the POST still goes to the referenced
action's own path. Use it instead of declaring the same select/button again per child node
(each declared action costs RAM on the device).

---

## 5. Wire format (`schema_version: 2`)

Producer: `portal_core.h`. Validator: `script/portal_schema.py` (`validate_envelope(obj) ->
list[Violation]`, CLI `script/portal_schema.py FILE...`, exit 0 iff all valid). The validator
never raises on malformed input.

### 5.1 Envelope

Keys in this order: `schema_version` (2), `seq`, `series_legend`, `served_by`, `peers` (`[]`),
`links`, then `path` and `ancestor_role` (only for `?path=`), then `tree`.

- `links` (new, additive; a consumer that does not know it ignores it): list of
  `{rel, label, href}`, all non-empty strings, `href` starting with `/`. Empty unless the AP
  is active (§6.3). A change of `links` advances `seq`. The validator treats the key as
  optional (older producers) and checks its shape when present.

- `seq` advances (wraps 0 -> 1) when the fingerprint changes. The fingerprint covers every
  value's reading and derived state (not raw ages), every `absent_when` verdict, and every
  action's emitted `value`, `enabled` and `result.state`/`result.why.code` (not
  `result.age_ms`).
- `series_legend` is `{"r":"real","s":"SNA","i":"implausible","-":"not measured"}` when the
  emitted (possibly narrowed) tree carries at least one emitted series, else `null`.
- `ancestor_role` (with `path` only): nearest strict ancestor declaring a role, as
  `{node,label,role}`, or `null`.

### 5.2 `served_by`

`{board, node_id, mac, state, why, uptime_ms, build, owns, components}`. Identity complete
(all three set) -> `state: "ok"`, `why: null`, `owns` from YAML. Otherwise `board`, `node_id`,
`mac` null, `state: "unknown"`, `why.code: "no_identity"`, `owns: []`.

### 5.3 Node

Every node has exactly these keys, always present, in this order: `id`, `kind`, `label`,
`state`, `why`, `provenance`, `values`, `series`, `children`, `actions`, then `role` and
`roles_available` only when a role is declared.

- `state` in {ok, stale, degraded, refused, silent, absent, unknown} from this producer
  (`down`, `unreachable`, `incompatible` exist in the vocabulary, never emitted here).
- `why` is null iff `state` is `ok`; otherwise `{code, detail, since_ms, evidence}`.
- Derivation: own value never published / non-finite -> `degraded`
  (`entities_unavailable`); text over `text_max` -> value `degraded`; any own value older than
  a known budget (booleans excepted) -> `stale` (`stale_data`); `absent` / `absent_when` ->
  `absent`; `unknown_when` match -> `unknown`; any non-ok child upgrades an ok/stale parent to
  `degraded` (`child_degraded`, evidence `children: {id: state}`).
- `on_demand` node none of whose own values has a reading (never published since boot) ->
  `unknown`, `why.code: "not_requested"`, `evidence: {}`. Such a child does **not** upgrade
  its parent and is left out of the parent's `child_degraded` evidence. Once any own value has
  a reading, normal derivation applies.
- Any own value with `level` `warn` or `bad` -> `degraded`, `why.code: "limit"`,
  `evidence: {"value": "<key>", "level": "warn"|"bad", "reading": <reading>, "bound": {...}}`.
  The worst value wins (`bad` over `warn`), a tie goes to the first in YAML order. `reading`
  is the value as emitted (number with the value's decimals, or bool). `bound` is the limit
  that matched: `{"below": x}` or `{"above": x}` (x with the fewest decimals that represent
  it, at most 6), or `{"when": b}` for a boolean.
- **Precedence**, first match wins: `absent` (incl. `absent_when`) > `unknown_when` >
  `not_requested` > `entities_unavailable` > `limit` > `stale_data` > `child_degraded`.
- `provenance` is `null` on a strict ancestor of an owned node; otherwise
  `{origin:"local", board, node_id, age_ms, budget_ms, trust, seq}`.

Values, series, provenance and transport rules are exactly those of `schema_version: 2`:
`value` null iff `classification` is `sna`; booleans have `raw: null`; series carry
`key,label,unit,index_base,index(null),index_frame,values,class,pack_index_base`; an absent
node carries `series: null`.

Value entry (extended, additive): after the v2 keys (`key, label, unit, value, raw, raw_hex,
classification, state, why`) comes `level`: `"ok"`, `"warn"`, `"bad"`, or `null` when the value
declares no limits or has no reading (`classification: sna`). `level` never changes the value's
own `state`/`why` (they keep their v2 meaning); it feeds the node's derivation above.

### 5.4 Action entry (extended)

Order of keys: the seven v2 keys, then the extension keys.

```json
{"key": "logging", "label": "Logging", "method": "POST",
 "path": "/api/v1/actions/vehicle.logger/logging",
 "enabled": true, "why": null, "body": {"value": "bool"},
 "control": "switch", "value": true, "options": null,
 "min": null, "max": null, "step": null, "unit": null,
 "result": {"state": "accepted", "why": {"code": "applied", "detail": null,
            "since_ms": null, "evidence": {}}, "age_ms": 2140, "value": true}}
```

| key | meaning |
|---|---|
| `method` | always `"POST"` |
| `path` | `/api/v1/actions/<node id>/<key>` |
| `enabled` | false only when the entity cannot be driven: `select`/`number` without a state -> `why.code: "entity_unavailable"`. `switch` and `button` are always enabled |
| `why` | null iff `enabled` |
| `body` | the request body shape: switch `{"value":"bool"}`, select `{"value":"string"}`, number `{"value":"number"}`, button `null` |
| `control` | `switch` / `button` / `select` / `number` |
| `value` | current entity state: bool (switch), string (select), number with `decimals` (number); `null` for button and for an entity without state |
| `options` | select: list of option strings (copied at setup); else `null` |
| `min`, `max`, `step` | number: from the number's traits, emitted with `decimals`, **always numbers, never `null`** (ESPHome number traits always carry them); every other control: `null`. The validator rejects `null` for a `number` and a non-null value for any other control (`wrong_type`) |
| `unit` | number: unit of measurement or `null`; else `null` |
| `result` | `null` until first invoked since boot; then the last result (5.5) |

The validator accepts an action without the extension keys (older producer). If present,
each extension key must have the type above; `control` outside the four known values is a
violation `unknown_control` for the validator and is rendered as unrecognised (disabled) by
the page.

### 5.5 Action result (R26–R28)

`result = {state, why, age_ms, value}`:

- `state` in {`running`, `accepted`, `refused`}.
- `why` is **always** an object (a reason for every outcome): `running` -> `queued`;
  `accepted` -> `applied`; `refused` -> one of `invalid_body`, `wrong_type`, `out_of_range`,
  `unknown_option`, `disabled`, `body_too_large`. `detail` may carry prose.
- `age_ms`: ms since this result was set.
- `value`: the requested value as parsed (`null` for a button or an unparseable body).

`accepted` means the entity took the command on the main loop. It is not a claim about what
the commanded device did; the entity's own state shows in `value` of the action entry and in
any bound values.

### 5.6 Validator contract (`script/portal_schema.py`)

The validator is the conformance gate for envelopes of `schema_version: 2`, from this producer
and from older v2 producers alike. It is not the page: the page ignores unknown keys (R32);
the validator does not.

- **Closed key sets.** Envelope, `served_by`, peer, node, value, series, action, action
  result, why, provenance, `ancestor_role` and `links` entries are closed: a key outside the
  required + optional set of §5 is a violation `unknown_key`. Optional keys are: envelope
  `links`, `path`, `ancestor_role`; node `role`, `roles_available`; value `level`; action
  `control`, `value`, `options`, `min`, `max`, `step`, `unit`, `result`. A missing optional key is
  never a violation; a missing required key is `missing_key`.
- **Older envelopes must pass.** An otherwise valid v2 envelope with no `links`, actions
  carrying only the seven v2 keys, `series.class: null`, non-empty `peers` (validated per
  v2 peer rules) and every v2 `state` (incl. `down`, `unreachable`, `incompatible` with
  their v2 rules) is valid. A rejecting test for each of these would be a contract bug.
- **Key order is not checked by the validator.** JSON object order carries no meaning, and
  another v2 producer may serialise in a different order. This producer's order (§5.1, §5.3,
  §5.4) is pinned by the byte-identical golden fixture (PS-M1-08), not by the validator.
- `series_legend`: exactly the four-entry object of §5.1 or `null`; anything else is a
  violation. It must be `null` iff the tree carries no non-null `series`.
- v1.5 rules (each only when the key/code is present, so older envelopes pass): `level` outside
  `ok`/`warn`/`bad`/`null` -> `unknown_level`; a non-null `level` on a value with
  `value: null` -> `level_without_reading`; a node `ok` with a value at `warn`/`bad` ->
  `node_ok_with_limit`; `why.code: "limit"` whose evidence lacks `value` (string), `level`
  (`warn`/`bad`), `reading` or `bound` (object), or names a value of the node whose `level`
  differs -> `limit_evidence_mismatch`; `not_requested` on a state other than `unknown`, or
  on a node with a value that has a reading -> `not_requested_inconsistent`; `switched_off`
  on a state other than `absent` -> `switched_off_inconsistent`.

---

## 6. HTTP surface

All routes are claimed by the portal's `canHandle()`; anything else falls through to other
handlers on `web_server_base` (e.g. `captive_portal`) or to the server's 404.

| route | method | login | responses |
|---|---|---|---|
| `/` | GET | never | 200 `text/html; charset=utf-8`, `Content-Encoding: gzip`, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, CSP as before |
| `/api/v1/state` | GET | never | 200 chunked JSON (`Cache-Control: no-store`), 204, 400, 404, 503 per §6.1 |
| `/api/v1/actions/<node>/<key>` | POST | only when `auth:` is set | §6.2; route not claimed when the build has no action |
| AP-only routes | GET | never | §6.3 |

No GET route ever answers 401, whatever `auth:` says. With `auth:` set, a POST without valid
credentials gets `401` with
`WWW-Authenticate: Digest realm="Login Required", qop="auth", nonce=..., opaque=...`, empty
body, and **no side effect** (the tree and `seq` do not change). A valid Digest response
(`curl --digest -u user:pass`) passes.

### 6.1 State transport rules (unchanged)

| rule | response |
|---|---|
| query string >= 32 bytes | 400 |
| non-empty query with neither `path` nor `after` | 400 |
| unparseable `after` (decimal uint32 only) | 400 |
| producer cannot snapshot itself | 503 |
| `path` naming no node (checked before `after`) | 404 |
| `after` equal to `seq` | 204 |

The body is streamed in frames of at most 4096 bytes (`httpd_resp_send_chunk`); the response
buffer never grows with the tree.

### 6.2 Actions

Request body, one of:
- `Content-Type: application/json`: `{"value": <bool|string|number>}`; for a button the body
  may be empty or `{}`.
- `Content-Type: application/x-www-form-urlencoded`: `value=<text>`; switch accepts
  `true/false/on/off/1/0`, number a decimal, select the option text.

Body over 256 bytes -> refused `body_too_large`.

| situation | HTTP | recorded in tree | response body |
|---|---|---|---|
| unknown node or key | 404 | no | empty |
| `auth:` set, credentials missing/invalid | 401 | no | empty |
| action disabled | 409 | `refused` / `disabled` | result JSON |
| previous invocation still `running` | 409 | no (the running result stays) | `{"error":"busy"}` |
| body not parseable / no `value` (non-button) | 400 | `refused` / `invalid_body` | result JSON |
| value of the wrong JSON type | 400 | `refused` / `wrong_type` | result JSON |
| body too large | 400 | `refused` / `body_too_large` | result JSON |
| number outside [min, max] or non-finite | 422 | `refused` / `out_of_range` | result JSON |
| select value not an option | 422 | `refused` / `unknown_option` | result JSON |
| valid | 202 | `running` / `queued`, then `accepted` / `applied` on the next main-loop pass | result JSON |
| the portal's lock not obtained within 200 ms | 503 | no | empty |

Rejected by `web_server_idf` **before** the portal sees the request, so never recorded and
not the portal's to change: a POST without a `Content-Length` header -> `411` (for a button
send an empty body, e.g. `curl -X POST -d '' ...`); a form-encoded body over 1024 bytes
(`CONFIG_HTTPD_MAX_REQ_HDR_LEN`) -> `400`. Note that `curl -d` without `-H 'Content-Type:
application/json'` sends a form body, so `{"value":...}` sent that way is `invalid_body`;
use `-d 'value=true'` or set the JSON content type.

Result JSON: `{"seq": <seq after recording>, "node": "<id>", "key": "<key>", "result": {...}}`,
`Content-Type: application/json`.

Execution: the httpd task validates and enqueues (one pending slot per action, guarded by the
mutex); `loop()` performs `turn_on()/turn_off()`, `press()`, `make_call().set_option().perform()`,
`make_call().set_value().perform()` and records `accepted`. **No entity method is called on
the httpd task.**

Shutdown: `web_server_base` keeps serving until the restart, so the portal must not destroy
anything the httpd task uses (mutex, buffers) in `on_shutdown()`/`teardown()`. A request that
arrives during shutdown is served or refused normally, never a crash.

### 6.3 AP fallback and captive detection

While `captive_portal` is active (`captive_portal::global_captive_portal->is_active()`, only
compiled with `USE_CAPTIVE_PORTAL`):

- The portal owns `/`, `/api/v1/state` and the actions route as always.
- The captive portal's own routes stay with it: `/config.json`, `/wifisave`, and the WiFi setup
  page at the fixed path **`/wifi`** (the portal never claims these three).
- Every other GET is claimed by the portal and answered `302 Found` with
  `Location: http://<soft-AP IP>/` (e.g. `/generate_204`, `/hotspot-detect.html`,
  `/connecttest.txt`, `/ncsi.txt`). A phone's captive detection therefore opens the portal.
- The envelope carries `links` (§5.1) with
  `{"rel":"wifi_setup","label":"WiFi setup","href":"/wifi"}`; the page shows it as a link in
  the header.

While the AP is not active, the portal claims only its three routes and `links` is `[]`.

---

## 7. Page (`index.html`)

- One file, no external resource, no build step; built-in asset <= 64 KiB gzipped (as
  embedded: `gzip -9`, mtime 0).
- No board name, address, MAC, vehicle name or private marker in the file.
- `KINDS` includes the neutral nouns `vehicle`, `component`, `bus`, `device`, `ecu`,
  `charger`, `battery`, `logger`, `gateway`, `network`, `sensor`, `actuator`, `module` in
  addition to the existing ones; an unknown kind renders generically and is named.
- Board hue table is keyed by nothing board-specific; a board without a known hue gets a hue
  derived from its name or the neutral dot.
- Actions render per `control`: switch -> toggle, button -> button, select -> dropdown,
  number -> number input + set button (min/max/step), plus a result chip (state, why code or
  detail, age). Disabled actions stay visible with their reason. Unknown `control` -> shown
  disabled as unrecognised. Requests use `fetch` with JSON bodies; a 401 on a POST lets the
  browser's own Digest prompt handle the login. The page itself never needs a login.
- Repaint: a toggle or a button never holds the repaint. Only a number input or a dropdown
  the user touched within the last 10 s holds the node card; header, banners, crumbs and peers
  repaint regardless, a dropdown change blurs it, and navigation always repaints (bench
  2026-10-12: a focused toggle or dropdown froze the whole page, result chip included).
- Clocks: while the envelope is held (204), the page advances `served_by.uptime_ms` and
  `result.age_ms` by the time since the envelope arrived (neither is in the fingerprint and
  both only grow). Provenance ages are not advanced (a value re-published unchanged gets
  younger without a new `seq`); instead the page fetches the full envelope without `after` at
  least every 5 s.
- Each entry of the envelope's `links` is shown as a link in the header (the WiFi setup link
  while the AP is active).
- Kind views (an extra or replacing section for one `kind`, e.g. the inherited `pack`, `cse`,
  `ti`, `dtc`, `dtcs`, `wake_holds`, `capture`, `census` views) are allowed as catalogue
  entries: they are keyed by `kind` only, fall back to the generic card when their data is not
  there, and contain no vehicle, board or bench identity. A kind no view knows renders
  generically.

---

## 8. M2 decision: `web_server_base` handler, not an own httpd

**Decided: the portal is an `AsyncWebHandler` on `web_server_base`.** Investigation against
the pinned ESPHome 2026.7.0 (`esphome/components/web_server_idf/web_server_idf.{h,cpp}`,
`web_server_base/web_server_base.{h,cpp}`, `captive_portal/captive_portal.{h,cpp}`):

1. `AsyncWebServer::request_handler_()` calls the first handler whose `canHandle()` is true and
   runs `handleRequest()` synchronously on the httpd task.
2. `AsyncWebServerRequest` has `operator httpd_req_t *()`. Inside `handleRequest()` the portal
   can call `httpd_resp_set_status/type/hdr` and `httpd_resp_send_chunk` exactly as the
   previous own-httpd code did. Streaming is therefore available unchanged; the
   `AsyncResponseStream` (which buffers into a `std::string`) is **not** used.
3. POST bodies: `application/json` arrives via `handleBody()` in chunks of <= 1460 bytes,
   then `handleRequest()`; form bodies arrive pre-parsed (`arg("value")`).
4. Auth: `AsyncWebServerRequest::authenticate()` / `requestAuthentication()` exist when
   `USE_WEBSERVER_AUTH` (+ `_DIGEST`) is defined; the portal emits those defines itself. The
   portal registers with `add_handler_without_auth()` and checks credentials itself, on the
   POST route only, so GET can never be locked and the portal's credentials are not shared
   with other handlers. This deviates from "set the base credentials" in the original plan on
   purpose.
5. Handler order: the portal registers in `setup()` (priority `WIFI - 1`, like `web_server`),
   `captive_portal` only when the AP starts, so the portal is asked first. While the AP is
   up the portal also claims every GET except `/wifi`, `/config.json` and `/wifisave` (§6.3);
   `captive_portal`, whose `canHandle()` accepts any GET while active, gets exactly those
   three and serves its WiFi page at `/wifi`.
6. The raw path skips `init_response_()`, so the base's default
   `Access-Control-Allow-Origin: *` header is not sent: same-origin, as before.

**Risk: httpd task stack.** `web_server_idf` starts httpd with the IDF default stack
(4096) + 256 = 4352 bytes; the previous own httpd used 6144. Mitigation required: no buffer
>= 256 bytes on the stack in the request path (the 4096-byte chunk buffer and the per-series
element buffer are members), and the bench measures the httpd task's stack high-water mark
after serving the largest test tree (PS-M2-07).

**Fallback trigger (D4):** the default stays `web_server_base`. Only if PS-M2-06 or PS-M2-07
fails on the bench and cannot be fixed inside the handler does the spec owner decide, from the
measured numbers, on an own httpd (stack 6144, own Digest check) and record the decision and
its heap cost here.

**Numbers** (filled in by M4/M5; tester reports, spec owner records):

| build (same fictional tree) | static RAM (compile) | free heap idle | max block idle |
|---|---|---|---|
| portal + captive_portal | tbd | tbd | tbd |
| previous component + `web_server` v3 on :81 + captive_portal | tbd | tbd | tbd |

---

## 9. Acceptance criteria

Tier legend: **py** pytest (schema/codegen/asset/tool), **host** `make -C tests/host`,
**cfg** `esphome config`, **cc** `esphome compile`, **bench** a bench board per
`tests/hil/HIL.md`, **rev** code review by the spec owner.

### Global

| ID | criterion | tier |
|---|---|---|
| PS-G-01 | `script/check.sh` exits 0 (private-data guard, pytest, host, clang-format, config of every `tests/build/*/test*.yaml`) | all |
| PS-G-02 | No existing component's files change; nothing private (vehicle names, real MACs/IPs, captures, sibling-repo paths) in any tracked file, commit message or PR text | py, rev |
| PS-G-03 | Every `tests/build/portal/test*.yaml` is in the CI compile matrix | rev |

### M0 — spec

| ID | criterion | tier |
|---|---|---|
| PS-M0-01 | This file exists, is neutral, and passes `check_no_private_data.py` | py |

### M1 — move, generic

| ID | criterion | tier |
|---|---|---|
| PS-M1-01 | `components/portal/{__init__.py,portal.h,portal.cpp,portal_core.h,index.html}` exist; `portal_core.h` includes only `<cstddef>`, `<cstdint>`, `<cstring>` and compiles in the host build | host, py (include grep) |
| PS-M1-02 | Every schema rule of §4.1, 4.3–4.5 has an accepting and a rejecting pytest case (segment/key patterns, duplicates, exclusivity, evidence-gated codes, budget derivation incl. all null spellings, binary_sensor cadence/budget, series frame/provider rules, role rules, unknown_when/trust_from targets, text_max range, port 32767/65535) | py |
| PS-M1-03 | `owns` rules of §4.2: unknown id, duplicate, identity-without-owns rejected; a config that compiles `uds`/`isotp` but declares `owns: []` without identity is valid; `resolved` owns equal YAML | py |
| PS-M1-04 | Provider registry: only `switch_off` by default (v1.5); unknown provider rejected with the (empty) list; a provider registered by the test validates, missing/extra args rejected, codegen emits one deduplicated thunk per (provider, args) whose text has each `{arg}` replaced by the validated value, `{{`/`}}` as literal braces, and the §4.6 names in scope | py |
| PS-M1-05 | Root node: id `car`, kind `car`, label `root_label` (default `Vehicle`) | py (codegen), host |
| PS-M1-06 | `submodule_position`/`submodule_size` rules of §4.5 incl. the derived `resolved_pack_index_base` | py |
| PS-M1-07 | Envelope emitter: all ported core cases pass (ChunkWriter framing at 4096, JSON escaping, number formatting, `parse_after`, `has_node`, resolve/derivation table, child propagation, skeleton provenance, `path`/`ancestor_role` order and cycle guard, series pairing/clamp/legend, absent_fn single evaluation, fingerprint ignores age but not state, flush failure propagates) under ASan/UBSan | host |
| PS-M1-08 | Wire compatibility: fictional golden fixtures in `tests/portal/fixtures/` validate with `portal_schema`; at least one fixture is byte-identical to the core's output for the same snapshot (host or py-driven) | host, py |
| PS-M1-09 | `script/portal_schema.py`: every rule of §5 has a rejecting case with its violation code; the §5.6 compatibility list has an accepting case each (no `links`, 7-key actions, `class: null`, a non-empty `peers`, a reordered envelope); CLI exit codes 0/1; never raises on non-dict / missing keys | py |
| PS-M1-10 | `web_asset:` replaces the embedded bytes (codegen embeds the gzip of the given file); missing file rejected | py |
| PS-M1-11 | Built-in `index.html`: <= 64 KiB gzipped, no `http://`/`https://` resource reference, no private marker, `KINDS` contains the §7 neutral nouns | py |
| PS-M1-12 | `tests/build/portal/test-minimal.esp32-c6-idf.yaml` (no actions, no auth) passes cfg and cc | cfg, cc |

### M2 — on `web_server_base`

| ID | criterion | tier |
|---|---|---|
| PS-M2-01 | No `httpd_start` in `components/portal/`; the portal registers via `web_server_base` (`init()` + `add_handler_without_auth`) | py (grep), rev |
| PS-M2-02 | `portal` + `web_server` in one config is rejected with the §4.8 message | py |
| PS-M2-03 | `port:` reaches `web_server_base::set_port`; with `captive_portal` + `wifi: ap:` the config compiles | py (codegen) or cc |
| PS-M2-04 | `auth:` emits `USE_WEBSERVER_AUTH` and `USE_WEBSERVER_AUTH_DIGEST`, no `auth:` emits neither; actions without `auth:` are **valid** (warning only); `auth:` with empty username or password rejected; `protect:` is rejected as an unknown key | py |
| PS-M2-05 | `canHandle()` claims exactly the three routes of §6 (POST route only when actions exist) while the AP is inactive, and additionally every GET except `/wifi`, `/config.json`, `/wifisave` while it is active | host (if the route matcher is in the core) or rev |
| PS-M2-06 | On a board: `GET /api/v1/state` with the full test tree (> 4096 bytes) arrives with `Transfer-Encoding: chunked`, complete, and passes `script/portal_schema.py` | bench |
| PS-M2-07 | On a board: httpd task stack high-water mark after serving the full state is >= 1024 bytes (logged at DEBUG by the portal) | bench |
| PS-M2-08 | On a board: the §6.1 table (400 / 404 / 204 / 503-not-required) holds | bench; host for parsing |
| PS-M2-09 | On a board with `auth:`: `GET /` and `GET /api/v1/state` without credentials 200; POST without credentials 401 with a Digest challenge and `seq` unchanged; POST with `curl --digest` 202. On a board without `auth:`: POST without credentials 202 | bench |
| PS-M2-10 | AP fallback (STA not reachable, AP up): `curl http://192.168.4.1/` returns the portal page; `/api/v1/state` validates and carries the `wifi_setup` link; `/wifi` returns the captive portal's WiFi page; `/config.json` returns its JSON | bench |
| PS-M2-12 | Captive detection lands on the portal: with the AP up, `GET /generate_204`, `/hotspot-detect.html`, `/connecttest.txt` and an arbitrary path answer `302` with `Location: http://192.168.4.1/`; a phone joining the AP opens the portal page in its captive sheet; with the AP down those paths are not claimed and `links` is `[]` | bench (phone check optional, curl mandatory) |
| PS-M2-13 | `links` emission: empty list without AP; the `wifi_setup` entry with AP; shape validated by `portal_schema` (rejecting cases for missing/empty fields and `href` without leading `/`); a change advances `seq` | host, py |
| PS-M2-11 | §8 numbers table filled: static RAM of both builds, free heap and max block idle before/after on the bench board | cc, bench |

### M3 — actions

| ID | criterion | tier |
|---|---|---|
| PS-M3-01 | §4.7 schema: each control type accepted; zero or two entity keys, duplicate key, `decimals` on non-number, bad key pattern rejected | py |
| PS-M3-02 | A node without actions emits `"actions":[]`; a build without any action claims no POST route | host/py, bench |
| PS-M3-03 | Action entry emission for each control (enabled, disabled with `entity_unavailable`, value null without state, options, min/max/step/unit) matches §5.4 key order and types, and validates | host, py |
| PS-M3-04 | Request parsing per §6.2 (JSON and form, each control, type errors, range, options, size) yields the right outcome code; refusals record `refused` with the right `why.code`; busy is not recorded | host |
| PS-M3-05 | Fingerprint/seq advances on action value, enabled or result change and not on `result.age_ms` alone; `?after=<old seq>` returns a body after a POST | host, bench |
| PS-M3-06 | On a board: switch round-trip — POST `{"value":true}` -> 202 `running`; a following state shows `accepted`/`applied` and `value: true`; POST false reverts; same for one select or number | bench |
| PS-M3-07 | On a board: unknown node/key 404; with `auth:` a POST without credentials 401 and `seq` unchanged; out-of-range 422 recorded as refused | bench |
| PS-M3-08 | Page renders toggle, button, dropdown, number input, the result chip and the envelope's `links`; sends JSON via `fetch`; never requests credentials for GET (static checks; optional `node` smoke test skips without node) | py |
| PS-M3-09 | No entity control call on the httpd task; pending slots and results guarded by the mutex | rev |
| PS-M3-10 | Page repaint hold and local clocks per §7 (core `holdsRepaint`/`advanceClocks` under node; on a board a toggle shows its result chip without moving the focus, uptime and chip ages keep counting) | py, bench |

### M4 — docs and example

| ID | criterion | tier |
|---|---|---|
| PS-M4-01 | `tests/build/portal/test.esp32-c6-idf.yaml`: fictional vehicle (bus, device, logger nodes), values of all three entity kinds, a series, all four action kinds (template entities, nothing on a vehicle bus), `auth:`, `wifi: ap:` + `captive_portal`; cfg + cc green | cfg, cc |
| PS-M4-02 | `tests/build/portal/test-asset.esp32-c6-idf.yaml` with `web_asset:` passes cfg + cc | cfg, cc |
| PS-M4-03 | `docs/portal-user-guide.md`, README component list, `docs/HANDOVER.md` entry for open bench items exist | rev |

### M5 — bench

| ID | criterion | tier |
|---|---|---|
| PS-M5-01 | One bench board identified by MAC, flashed with the M4 test config; its previous image restored afterwards (same build string as before) | bench |
| PS-M5-02 | PS-M2-06..10 and PS-M3-06..07 run on that board, results in the PR | bench |

### M6 — Daumen (v1.5)

| ID | criterion | tier |
|---|---|---|
| PS-M6-01 | §4.3 limits: `warn`/`bad` (below/above, at least one) on sensor values, `warn_when`/`bad_when` on binary values accepted; on the wrong kind or on text rejected; `expert` accepted | py |
| PS-M6-02 | §4.7/4.9 `confirm: hold`, `group`, `before` (select/number/switch ref, typed value, select options and number range where known) accepted; unknown ref, non-control ref, wrong type, unknown option rejected | py |
| PS-M6-03 | §4.9 `lang`, `layout.tabs` (0..5, glance 0..4, node ids) and node `borrow` accepted; bad counts and unknown refs rejected with the valid list | py |
| PS-M6-04 | §4.9 injection: the token in the page is replaced by the hints JSON (built-in and `web_asset:`), a page without the token is embedded unchanged, `<` is escaped | py |
| PS-M6-05 | §5.3 `level` derivation (below/above strictness, bad over warn, bool, no reading -> null) and node `limit` with evidence, worst-wins, first-in-order tie | host |
| PS-M6-06 | §5.3 precedence table, incl. `not_requested` (no parent upgrade, not in parent evidence, normal after first reading) | host |
| PS-M6-07 | §4.6 `switch_off`: absent while off with `switched_off` and the action evidence, `{}` without an action, values still emitted, seq advances on a switch change | host, py |
| PS-M6-08 | §5.6 v1.5 validator rules: each has a rejecting case; envelopes without `level` still pass | py |
| PS-M6-09 | `tests/build/portal/test.esp32-c6-idf.yaml` carries every v1.5 key; cfg + cc green; static RAM before/after recorded in the PR | cfg, cc |

---

## 10. Open decisions

All four were decided by the owner on 2026-10-12.

| # | question | decision |
|---|---|---|
| D1 | Busy POST (409) is not recorded in the tree | accepted: the running result stays visible |
| D2 | Does viewing need a login? | no: `/` and `/api/v1/state` never need one; `auth:` is optional, also with actions, and guards only the POST route; `protect:` removed. Network access (AP password, protected LAN) is the access control |
| D3 | Who owns `/` while the AP is up? | the portal; the WiFi setup page stays at `/wifi`, the portal links to it, and captive detection is redirected to the portal (§6.3) |
| D4 | Own httpd? | default stays `web_server_base`; the spec owner decides from bench numbers (§8) |

---

## 11. Change process

Spec changes are made by the spec owner, noted on the team board, and committed with the
acceptance IDs they touch. The implementer and tester do not edit this file.
