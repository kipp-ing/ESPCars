# `uds` — a diagnostic client driven by a flashed catalog

Design of 2026-07-30. Worked example throughout: the `MINI` catalog in
`tests/uds/fixtures/` (request `0x7E7` / response `0x7EF`), UDS over ISO-TP at
500 kbit/s — small enough to read in full, and every example below can be
reproduced by compiling and dumping it (§7).

## 1. What it is, and what it deliberately is not

`uds` is the **client** layer above `components/isotp`: it builds requests,
validates and decodes positive responses, schedules polling, and publishes
ESPHome entities. It never emulates an ECU — that role stays out of the
component, and the bench responder rig is built from a second `isotp` instance in
plain YAML instead (§8).

The layering is already in place and nothing below it changes:

```
can_gateway   frames, ISR data plane, inject()
   └ isotp    ISO-TP transport: segmentation, flow control, reassembly (exists)
       └ uds  requests, decode, schedule, entities            (this document)
```

**The decode rules do not live in the firmware and they do not live in YAML.**
They live in a `.dcat` catalog compiled from the factory diagnostic database and
flashed into its own memory-mapped partition — see
[`uds-catalog-format.md`](uds-catalog-format.md). YAML names a *field*; the
catalog owns its bit position, sign, scaling, unit, enum texts and
"not available" sentinels. This is the whole point of the design: adding or
correcting a DID is a catalog reflash, not a firmware change, and no magic
number is ever transcribed into a config file by hand.

## 2. Why a memory-mapped partition

`esp_partition_mmap(part, 0, size, ESP_PARTITION_MMAP_DATA, &ptr, &handle)` maps
flash into the data address space, so the catalog is addressable as ordinary
memory with **zero heap and no boot-time parse**. All lookups run against flash.
The component's own RAM is then only what the *live* work needs: a small polled
group table plus one reassembly buffer inside `isotp`.

The partition is requested from codegen with stock ESPHome's
`esphome.components.esp32.add_partition(name, "data", 0x06, size)` — no core
edit, no hand-written CSV, so the external-component rule holds. Custom
partitions steal from both app slots: 256 KB of catalog costs 128 KB of each
1.8 MB slot, against a ~230 KB firmware. Offsets are auto-placed by
`gen_esp32part.py`, so the flasher reads them back from the build's
`partition-table.bin` rather than assuming (§7).

A second source is supported for exactly one reason — CI must compile the whole
decode path without anything being flashed:

- `catalog: {partition: diag, size: 256KB}` — mmap. The normal case.
- `catalog: {embed: true}` — the blob is emitted as a `const uint8_t[]` in the
  generated code. Also zero-RAM (flash-resident) but needs a recompile to
  change, so it is for build tests and tiny catalogs.

Both reduce to `const uint8_t *base, size_t size` behind one reader, so the code
under test is the code that runs.

## 3. Configuration

```yaml
uds:
  - id: ecu
    isotp_id: isotp_ecu             # an existing isotp instance
    catalog:
      file: catalogs/mini.dcat      # host-side path: validated at `esphome config`
      partition: diag               # on-device home; or `embed: true`
      size: 256KB
    ecu: MINI                       # picks the Ecu record; optional if the catalog has one
    # transport parameters are taken from the catalog unless overridden here
    max_consecutive_failures: 3    # then publish unavailable
    allow_active_services: false   # gate on everything that is not a pure read
    on_value:
      - lambda: |-
          ESP_LOGD("uds", "%s = %.3f %s", x.field, x.value, x.unit);
    on_error:
      - lambda: 'ESP_LOGW("uds", "%s", x.what);'

sensor:
  - platform: uds
    uds_id: ecu
    field: DT_Mini_Temperature_Averaged       # catalog owns scale, unit, sentinels
    update_interval: 30s
    name: "Temperature mean"
  - platform: uds
    uds_id: ecu
    field: DT_Mini_Temperature_Maximum        # same DID 0x0102 →
    name: "Temperature max"
  - platform: uds
    uds_id: ecu
    field: DT_Mini_Temperature_Minimum        # … one request serves all three
    name: "Temperature min"

text_sensor:
  - platform: uds
    uds_id: ecu
    field: PRES_Mini_VIN_4Byte                # ASCII and enum fields
    update_interval: never                    # read once at boot, or on demand
```

`field:` is resolved **at config time** by the Python reader against
`catalog.file`. An unknown name fails the build with the closest matches listed;
an ambiguous name (the same field name in two groups) fails and names the
candidate groups, which `service:` then disambiguates. A typo can therefore not
survive to runtime, even though nothing is hardcoded.

### 3.0 How a field is addressed, and why it needed a second pass

The first draft of this section assumed a field name identifies a field. Compiling
a real factory database disproved it: two services can share one presentation
name in the same group — mini's DID `0x0101` names both its maximum and
minimum cell voltage `PRES_Mini_Voltage_2Byte` — and an array field can repeat
one presentation name across every element, as `0x0208` does across its four
cells. `service:` cannot break either tie, because the colliding services
merged into that one group.

The fix is in the compiler, not the YAML — the catalog now guarantees that a
group's field names are unique (format §5.1a) and collapses arrays into one
record (§5.1b). So three keys address anything, and the third is only for arrays:

| key | meaning |
|---|---|
| `field:` | the field's name as listed in the `.manifest.json` |
| `service:` | the group, by canonical name, by any original service qualifier, or by DID (`0x0101`) — required only when `field:` occurs in more than one group |
| `element:` | 0-based index into a collapsed array; `0` or omitted for a scalar |

Two consequences worth knowing. A field whose presentation name collided is
listed under its **originating service qualifier** — so mini's DID `0x0101`
maximum is `DT_Mini_Voltage_Maximum_Cell`, a factory-style name rather than an
invented one. And an array element is addressed by `element:`, not a name that
differs from its neighbour by an offset — cell 2 of `0x0208` is `element: 2`.

**The manifest is the discoverability surface.** It lists every group, every
field's final name, unit, geometry and scale rows, and every name that was
changed to keep a group unique. A YAML author reads it; nobody is expected to
guess a qualifier.

Codegen emits the *name*, not a resolved index, plus the catalog's CRC. At
`setup()` the component resolves names against whatever is actually flashed, and
if the CRC differs from the one YAML validated against it logs a warning and
keeps going — a catalog may legitimately be newer than the firmware. A name that
no longer resolves marks that entity unavailable and logs once; it is never a
crash and never a wrong value.

### 3.1 The transport must be big enough, and that is checked at config time

The catalog knows the ECU's transport parameters and the largest response any of
its groups can produce; the `isotp` instance one layer down owns the buffer that
has to hold it. Those two facts have to agree, and the failure when they do not
is quiet: a real multi-block response against a too-small `max_message_size:
256` is an `OVERFLOW_LOCAL`, which looks like a flaky ECU.

So the `uds` validator reads the referenced `isotp` config during
`esphome config` and **fails the build** when it cannot work, naming both
numbers:

- `max_message_size` smaller than the largest `resp_min_len` among the groups the
  config actually reads → error.
- `block_size` or `st_min` differing from the catalog's `CP_BLOCKSIZE_SUG` /
  `CP_STMIN_SUG` → info, not an error. Those are suggestions in the database and
  a slower client is still conforming.
- `n_cr_timeout` below the catalog's `p2_ext_ms` → warning: an ECU that answers
  `0x78` intends to take longer than the default.

The validator does **not** mutate the `isotp` config. Cross-component codegen
mutation would make the effective transport settings unreadable from the YAML,
and ownership of a key belongs to the component whose schema declares it. A build
error that names the two numbers is worth more than a silent correction.

### 3.2 `dump_config` must prove the catalog is really there

Boot output is the only evidence available on a bench, so it states what was
found rather than what was configured: catalog source (partition name and mapped
size, or `embedded`), CRC and whether it matched the config-time value, format
version, ECU qualifier with its request/response identifiers, group and field
counts, how many groups are polled and at what intervals, and — one line per
entity that failed to resolve — the name that is missing. A catalog that failed
validation says so in one line with the reason, and the component then does
nothing at all.

## 4. Requests, responses, and what counts as an error

One request in flight per hub, because ISO-TP has no multiplexing and `isotp`
enforces the same rule one layer down.

A response is accepted only when **all** of these hold, in order:

1. It is a positive response: `resp[0] == req[0] + 0x40`.
2. The identifier echo matches the request byte for byte. The echo is
   **`req_len - 1` bytes** — every byte of the request after the SID — compared
   against `resp[1 .. req_len-1]`. For SID 0x22 that is the two DID bytes; for
   0x1A the one identifier byte; for a subfunction service the subfunction. The
   rule is stated as a length rather than per-service so the catalog does not
   have to carry an echo width, and it holds for the whole read family the
   `SAFE_READ` flag admits. A stale answer to the *previous* request is
   otherwise indistinguishable from a fresh one, and on a shared bus that is not
   a theoretical concern.
3. `len(resp) >= 1 + echo_len` — enough to *be* a response. That is the only
   length that rejects.

`resp_min_len` is **not** a fourth condition. A response at or above it needs no
per-field bounds checks; below it the response is `PARTIAL` — the covered fields
decode, the uncovered ones are skipped and counted, and it still counts as a
success. Rejecting short responses wholesale would make every array element
past what a given ECU has wired unavailable against a healthy battery, because
a factory database can declare more slots for a DID than a specific unit
actually populates. An ECU that returns fewer elements than its database
declares is ordinary.

Negative responses are classified rather than lumped together, because the
correct reaction differs:

| NRC | meaning | reaction |
|---|---|---|
| `0x78` | request correctly received, response pending | extend the deadline to the catalog's `p2_ext_ms`, keep waiting, bounded retries |
| `0x21`, `0x22` | busy / conditions not correct | back off, retry later — not an error |
| `0x31` | request out of range | **permanent**: mark the group unsupported, stop polling it, log once |
| `0x33`, `0x35` | security access denied | mark unsupported unless a session/security step is configured |
| other | | count, back off |

`0x31` deserves its own row: polling a DID this ECU does not implement forever
is how a diagnostic client makes itself a nuisance on a bus.

Timeouts and transport failures come from `isotp` as `IsoTpError`; they feed the
same per-group backoff (double from the group interval, capped at 60 s, reset on
success). After `max_consecutive_failures` the group's entities publish `NaN`, so
Home Assistant shows unavailable rather than a value that stopped being true.

### 4.1 Decode in the callback, publish from `loop()`

`IsoTpProtocol::deliver_message_()` calls its consumer from inside
`can_gateway`'s bounded observation drain, and its own comment says consumers are
"expected to be brief: parse and buffer, act on the next tick". A single wide
response decoded and published straight from that callback could push dozens of
entity states through the API/MQTT/logger stack in one drain — the same
unbounded-work shape as `HANDOVER.md` Todo 7, and it would stretch a loop
iteration in proportion to how much the ECU said.

So the two halves are split:

- **In the callback:** validate, then decode every bound field into its entity's
  value slot and mark the slot dirty. Bounded, allocation-free, no I/O — the work
  is one pass over `field_count` with a few float operations each.
- **In `loop()`:** publish at most `PUBLISH_PER_LOOP` dirty slots (8), oldest
  first, then return. A long response drains over a few iterations instead of
  one.

The value slots are a fixed array sized at codegen from the number of bound
entities, so this costs 4 bytes per entity and no copy of the response. Copying
a wide multi-block payload out instead would be the obvious alternative and is
strictly worse: same deferral, ten times the RAM, and the decode still has to
happen somewhere.

Note this also settles the MessageView lifetime rule in the only safe direction:
the bytes are consumed inside the call, and nothing retains the pointer.

## 5. Scheduling

Per **group**, not per sensor. A group's interval is the minimum
`update_interval` across the sensors bound to it; every one of its sensors
publishes from each response, on change. The alternative — per-sensor due times
against a shared request — would put the same three bytes on the wire three
times, which is exactly what the group format exists to prevent.

Selection is a linear scan over the polled-group table picking the most overdue
group. The table holds one ~16-byte record per group in use (interval, next due,
backoff, failure count, flags), so 32 polled groups cost ~512 B of RAM and the
scan is unmeasurable next to a CAN frame.

`update_interval: never` declares a group that is only read on demand — identity
DIDs, a wide multi-block capacity read — via `uds.read`.

## 6. Safety

The repo rule is that TX on a vehicle bus is safety-relevant and defaults must
never widen into transmission. Diagnostics transmits by definition, so the rule
is honoured by *bounding what can be transmitted* rather than by refusing to
transmit:

- The scheduler polls **`SAFE_READ` groups only** — the catalog flags them, and
  the flag covers only service identifiers that cannot change ECU state.
- `uds.read` accepts `SAFE_READ` groups only, whatever `allow_active_services`
  says.
- Sessions, routines, IO control, writes, security access and reset are
  reachable only through `uds.execute`/`uds.raw` **and** only when the hub sets
  `allow_active_services: true`. Both the action and the flag are explicit; the
  config-time validator rejects an `execute` of an unsafe group without the flag,
  naming it.
- Declaring a sensor with an `update_interval` *is* the opt-in for reading. No
  polling happens without one.

## 7. Toolchain

`script/uds_catalog.py`, with `components/uds/catalog.py` as the shared reader
(shared because ESPHome codegen needs it too, and a second implementation would
drift):

| command | does |
|---|---|
| `compile` | factory-database JSON + profile → `.dcat` |
| `dump` | `.dcat` → human-readable listing; also the debugging tool |
| `verify` | header/CRC/bounds check plus limit enforcement |
| `flash` | write a `.dcat` to the device's partition |

`compile` reads a JSON export produced by an external, vendor-specific
conversion tool for the ECU's factory diagnostic database. That tool and its
multi-megabyte export stay in a private repo, out of this one: what is checked
in is the compiled `.dcat`, the profile that selected it, and the command in
the profile's header. Same precedent as the DBC in `script/dbc2yaml.py`.
`tests/uds/fixtures/mini-source.json` is a hand-authored stand-in shaped
exactly like that export — small enough to read in full and to recompile in
CI without the real one (see its own header comment for what each of its
groups is chosen to exercise).

The **profile** is a small YAML naming the ECU, the variant, which groups to
include, and any per-scale `INVALID` override. It is the reviewable artifact:
the compiler's sentinel heuristic proposes, the profile decides.

`flash` reads the partition offset out of the build's `partition-table.bin`
(magic `0xAA50`, 32-byte entries) and calls `esptool write_flash`. Offsets are
auto-placed, so reading them back is the only correct way to know them.

## 7a. The ESPHome-facing shape

Written down because codegen and runtime have to agree on it and they are
implemented together; the freestanding core below them is already fixed
(`uds_catalog.h`, `uds_decode.h`, `uds_proto.h`, `uds_sched.h`).

**`UdsHub : Component, isotp::IsoTpConsumer`** — one per `uds:` entry.

- Construction takes the `IsoTpProtocol *`; everything else is a setter called by
  codegen before `setup()`, per the project's no-partially-initialised-object
  convention where it can be honoured and setters where ESPHome requires them.
- `set_catalog_partition(const char *name)` **or**
  `set_catalog_embedded(const uint8_t *data, size_t len)` — exactly one. The
  first calls `esp_partition_find_first(DATA, ANY, name)` then
  `esp_partition_mmap(..., ESP_PARTITION_MMAP_DATA, ...)`; the second points
  straight at a generated `const uint8_t[]`. Both end at
  `Catalog::open(base, size)`, so one code path is under test either way.
- `set_expected_crc(uint32_t)`, `set_ecu_name(const char *)`,
  `set_max_consecutive_failures(uint8_t)`, `set_allow_active_services(bool)`.
- `add_binding(UdsBinding *)` — one per entity (see below). Codegen also calls
  `reserve_bindings(n)` and `reserve_poll_slots(n)` so both fixed arrays are
  allocated once, in `setup()`, and never again.
- `setup()`: open the catalog, resolve the ECU, resolve every binding's
  `field`/`service` names, build the poll table from the distinct groups that
  have an interval, then log the §3.2 report. A catalog that fails to open leaves
  the hub inert — no requests, every entity unavailable, one explanatory line.
- `loop()`: `sched_.poll(now)` for an expired deadline → the failure path;
  `sched_.next(now, p2_ms)` for the next due group → build the request with
  `build_request()` and hand it to `isotp_->send()`, calling
  `sched_.cancel_in_flight()` when send is refused (backpressure, not failure);
  then publish at most `PUBLISH_PER_LOOP` dirty bindings (§4.1).
- `on_message()` (the `IsoTpConsumer` hook, called inside can_gateway's drain):
  `classify_response()`, then per verdict — `ACCEPT` decodes every binding of the
  in-flight group into its value slot and marks it dirty; `NEGATIVE` with
  `PENDING` calls `extend_deadline()`; `UNSUPPORTED` calls `suspend()`; `BUSY` and
  `OTHER` call `on_failure()`; `NOT_OURS` and `TOO_SHORT` are counted and
  ignored, never allowed to complete the in-flight request.
- `on_error(IsoTpError)` routes to the same `on_failure()`.

**`UdsBinding`** — the per-entity record: resolved field index, group index,
array element, a `float` value slot, a dirty flag, and a pointer to either a
`sensor::Sensor` or a `text_sensor::TextSensor`. Kept as a plain struct owned by
the hub rather than a component per entity: the entities themselves are ordinary
ESPHome sensors registered by their platform, and the binding is what the hub
iterates.

**Actions**, all `Parented<UdsHub>`: `uds.read` (a `SAFE_READ` group by name or
DID, queued once), `uds.execute` and `uds.raw` (gated on
`allow_active_services`, rejected at config time when the flag is absent),
`uds.set_enabled`. **Triggers**: `on_value` with a small POD view
(`field`, `value`, `unit`, `text`, `valid`) and `on_error` with a `what` string.

**Codegen** (`__init__.py`, `sensor.py`, `text_sensor.py`) reads the catalog with
`components/uds/catalog.py` at validation time, so every `field:`/`service:`
resolves, ambiguity is reported with candidates, `SAFE_READ` is enforced, §3.1's
transport check runs, and the partition is requested via
`esp32.add_partition(name, "data", 0x06, size)`. Names — not indices — are
emitted, because the flashed catalog may legitimately be newer than the firmware.

## 8. Verification, per tier

| tier | what it must show |
|---|---|
| `tests/host` | catalog reader against a truncated-at-every-length blob under ASan, with no out-of-bounds read and no crash; extraction truth table (signed, byteswap, cross-byte, ASCII); scale evaluation incl. sentinel → NaN and no-row-matches → NaN; response validation incl. the identifier-echo and short-response guards; NRC classification; scheduler due order, backoff schedule and one-in-flight |
| `tests/uds` (pytest) | schema accept **and** reject paths; unknown/ambiguous `field:` diagnostics; the `SAFE_READ` gate; partition sizing; compiler round-trip; byte-identical golden `mini.dcat`; **cross-check of a compiled catalog against a hand-curated reference table for the same ECU, when that private fixture is present locally** |
| `tests/build/uds` | `embed: true` config compiled by CI (no partition needed) and a partition config compiled beside it |
| `tests/hil` | a responder rig (§9) proving multi-frame reassembly on our own hardware, then real hardware |

The cross-check is the interesting one, where it can run. Two independent
derivations of the same interface can exist: the factory database, and a
table reverse-engineered independently and flagged wherever its author
distrusted a formula. Asserting them against each other turns those flags into
either agreement or a named, located discrepancy — for free, in pytest. Where
they disagree the test records both and the factory value wins; that is a
finding worth writing down, not a test to relax. The real fixture this runs
against is bench-specific and kept out of this repo (`private/`, see
`docs/CONVENTIONS.md`); `tests/uds/test_crosscheck_bms.py` skips cleanly when
it is absent, so a fresh clone still passes on the synthetic `mini` fixture
alone.

## 9. Bench validation

A responder rig is built first, because a real ECU cannot be asked to reproduce
a flow-control edge on demand: a second `isotp` instance with `tx_id` and
`rx_id` swapped, plus an `on_message` automation that answers with a canned
payload, including one large enough to force multi-frame reassembly. This
needs **no component code** — `isotp` already sends flow control as receiver
and segments as sender — so the rig is pure YAML (a per-bench config under
the gitignored `private/hil/`, see `docs/CONVENTIONS.md`).

Then real hardware, in increasing order of what it exercises: a single-frame,
shortest-possible group first, then a group whose one request serves several
fields, then the largest multi-frame response the catalog declares, to reach
the reassembly path at full stretch.

Findings specific to one bench — a bus-id collision with another node's
traffic, a front end that turns out mute, and the like — belong in the
per-bench private notes under `private/` (§`docs/CONVENTIONS.md`), not in this
design doc: they are facts about that hardware, not about the component.
