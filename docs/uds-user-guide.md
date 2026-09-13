# Using `uds` — diagnostics in an ESPHome project

A practical guide: what the component does, how a project is actually put
together with ESPHome `packages:`, and the end-to-end workflow from a catalog
file to a value on a dashboard. For *why* it is built this way, see
[`DESIGN-uds.md`](DESIGN-uds.md) (design rationale) and
[`uds-catalog-format.md`](uds-catalog-format.md) (the binary format). This
document only covers the *how*.

## 1. The shape of it

```
can_gateway   frames, ISR data plane
   └ isotp    ISO-TP transport: segmentation, flow control, reassembly
       └ uds  requests, decode, scheduling, entities
```

`uds` is a **client**. It builds requests, validates and decodes positive
responses, polls on a schedule, and publishes ESPHome entities. It never
emulates an ECU.

The one thing that makes this different from a normal ESPHome sensor
component: **the decode rules are not in the firmware and not in the YAML.**
They live in a `.dcat` catalog — a small binary blob compiled from a factory
diagnostic database (or hand-written for a simple case) — which is flashed
into its own flash partition and memory-mapped at boot. YAML only ever names a
*field*; the catalog owns that field's bit position, sign, scaling, unit, enum
text and "not available" sentinel. Two consequences follow directly:

- **Firmware and catalog are two separate flashing steps**, and they change
  independently. Correcting a scale factor or adding a DID is a catalog
  reflash, never a firmware recompile.
- **A field is addressed by name**, resolved against the catalog both at
  `esphome config` time (so a typo is a build error, not a runtime NaN) and
  again at boot (so a catalog that is newer than the firmware still works).

## 2. The three things a working setup needs

1. **A catalog** — `<name>.dcat` plus `<name>.manifest.json`. Either compiled
   from a factory database with `script/uds_catalog.py compile`, or a small
   hand-authored one for a simple ECU. The manifest is what you read to find
   field names; never guess one.
2. **Firmware YAML** — `can_gateway` (the bus) → `isotp` (the transport,
   addressed to the ECU) → `uds` (the catalog + hub) → `sensor:`/`text_sensor:`
   (the fields you actually want).
3. **Two flashing steps, in order**: firmware first, catalog second. A
   firmware with no catalog flashed boots fine, logs
   `catalog unavailable` once, sends nothing, and leaves every entity
   unavailable — it never guesses. Flashing the catalog before the firmware
   exists doesn't work either: the partition's on-flash offset is only known
   once the firmware's build has placed it.

## 3. Workflow, step by step

### 3.1 Get a catalog

If you have a factory diagnostic database export (a vendor-specific conversion
tool's JSON output — see [`DESIGN-uds.md`](DESIGN-uds.md) §7 for the shape this
repo expects), write a **profile**: a short YAML naming the ECU, which DIDs to
include, and an optional per-row sentinel override. `catalogs/*.profile.yaml`
in this repo are worked examples. Then:

```bash
script/uds_catalog.py compile BMS.json --profile catalogs/my-ecu.profile.yaml \
    -o catalogs/my-ecu.dcat
script/uds_catalog.py dump catalogs/my-ecu.dcat      # human-readable listing
script/uds_catalog.py verify catalogs/my-ecu.dcat    # header/CRC/limit checks
```

This also writes `catalogs/my-ecu.manifest.json` — **the discoverability
surface**. It lists every group, every field's final name, its unit, its
geometry and every renamed field. Read it before writing `field:` lines;
nobody is expected to invent or guess a name.

No factory database yet? A tiny hand-authored catalog is exactly as valid —
the format doesn't care where the JSON came from, only that it round-trips.
`tests/uds/fixtures/mini.dcat`/`mini.manifest.json` in this repo are a minimal
worked example small enough to read in full.

### 3.2 Write the firmware YAML

```yaml
external_components:
  - source: github://kipp-ing/ESPCars
    components: [can_gateway, isotp, uds]

can_gateway:
  id: gw
  ports:
    - id: diag_bus
      tx_pin: GPIO10
      rx_pin: GPIO11
      bit_rate: 500kbps

isotp:
  - id: tp_ecu
    port_id: diag_bus
    tx_id: 0x7E7        # request id the tester sends on
    rx_id: 0x7EF        # id the ECU answers on
    max_message_size: 512   # must hold the largest response you read (checked at config time)
    block_size: 8
    st_min: 20ms

uds:
  - id: ecu
    isotp_id: tp_ecu
    catalog:
      file: catalogs/my-ecu.dcat   # host-side path, checked at `esphome config`
      partition: diag              # on-device partition name
      size: 256KB
    ecu: MY_ECU                     # only needed if the catalog has more than one
    max_consecutive_failures: 3

sensor:
  - platform: uds
    uds_id: ecu
    field: DT_Mini_Temperature_Averaged   # from the manifest — unit/scale come from the catalog
    update_interval: 30s
    name: "Temperature mean"
```

`esphome config` at this point already resolves `field:` against the catalog
file on your workstation — an unknown name fails the build and lists the
closest matches; a name that exists in two groups fails and names both
candidates, which a `service:` line then disambiguates.

### 3.3 Flash, in order

```bash
.venv/bin/esphome run my-ecu.yaml                                        # 1. firmware
script/uds_catalog.py flash catalogs/my-ecu.dcat \
    --build-dir .esphome/build/<node_name> --partition diag --port <dev> # 2. catalog
```

`flash` reads the partition's actual offset out of that build's
`partition-table.bin` — offsets are auto-placed by ESPHome/`gen_esp32part.py`,
so this is the only correct way to know them; never assume one.

### 3.4 Confirm it, from the log

`dump_config` states what was actually found on boot, not what was configured
— this is the only evidence available on a bench:

```
Catalog: partition 'diag', 262144 bytes mapped
Catalog size: 1830 bytes, format version 1
CRC: 0x040BC11F (matches the config-time catalog)
ECU: 'MY_ECU', request 0x7E7, response 0x7EF
Bindings: 6 of 6    Polled groups: 3 of 3
```

A CRC mismatch is a warning, not an error — the catalog may legitimately be
newer than the firmware. `Bindings: 6 of 6` is what to check after any catalog
reflash: if it drops below the count you expect, a field stopped resolving.

## 4. `packages:` — splitting "this board" from "this diagnostic setup"

Everything in §3.2 is genuinely reusable across boards, targets (embedded vs.
partitioned catalog), and even across an ECU and a test rig that answers the
same catalog. ESPHome's `packages:` key is what makes that reuse a file split
instead of copy-paste: it merges another YAML file's top-level keys into this
one (dicts merge key-by-key, lists concatenate), loaded with `!include`.

This repo uses exactly that split everywhere a `uds:` config exists:

```yaml
# leaf file — everything that is genuinely per-target
esphome:
  name: my-diag-node
esp32:
  board: esp32-c6-devkitc-1
  framework:
    type: esp-idf
logger:
external_components:
  - source: github://kipp-ing/ESPCars
    components: [can_gateway, isotp, uds]

packages:
  diag: !include diagnostics.yaml   # everything in §3.2's example
```

```yaml
# diagnostics.yaml — the reusable package: bus, transport, hub, entities
can_gateway: {...}
isotp: {...}
uds: {...}
sensor: {...}
text_sensor: {...}
```

Two concrete reasons this split earns its keep, both visible in this repo:

- **Swap the catalog source without touching a single field name.**
  `tests/build/uds/common.yaml` (`catalog: {embed: true}`, the `mini` fixture —
  the config CI can compile with nothing flashed) and
  `tests/build/uds/common_partition.yaml` (`catalog: {partition: diag, size:
  256KB}`, the same fixture through a real 256 KiB partition) are the *same*
  sensors and field names, included by two near-identical leaf files
  (`test.esp32-c6-idf.yaml` / `test-partition.esp32-c6-idf.yaml`) that differ
  only in which package they pull in.
- **Share board bring-up across every diagnostic config on that board.**
  `tests/hil/hil_common.yaml` carries the parts that are genuinely about the
  *board* (power rail sequencing, status LED, wifi/api/logger) and is included
  by every HIL config for that board; a per-bench diagnostic config adds only
  the bus pins, the `isotp`/`uds` blocks and the sensors on top. The same
  pattern works for two hubs reading one catalog on two different bus segments
  (a real ECU and a responder rig) in one firmware — real, bench-specific HIL
  configs live under the gitignored `private/hil/` (docs/CONVENTIONS.md), so
  this repo does not carry a worked example of that pairing, only the
  single-hub build tests above.

The rule of thumb: if a YAML block would be identical across two configs
except for pin numbers or a board name, it belongs in a package; if it's the
one thing that's actually different (which catalog, which pins, which board),
it stays in the leaf.

## 5. Config reference

### `uds:` (one hub per ECU / per `isotp` instance)

| key | meaning |
|---|---|
| `isotp_id` | the `isotp:` instance this hub talks over — **one hub per instance**; `IsoTpProtocol::set_consumer()` takes exactly one consumer |
| `catalog.file` | host-side path to the `.dcat`, resolved at `esphome config` |
| `catalog.partition` **or** `catalog.embed: true` | exactly one, never both — a named flash partition (normal case) or compiled into the firmware (CI/tiny catalogs) |
| `catalog.size` | partition size (`256KB` etc.); two hubs sharing one partition name must agree on it |
| `ecu` | which ECU record to use; only required if the catalog holds more than one |
| `max_consecutive_failures` | after this many, a group's entities publish unavailable (default 3) |
| `allow_active_services` | default **false**. Gates `uds.execute`/`uds.raw` and any group that is not a pure read — see §6 |
| `on_value` | fires per decoded field: `x.field`, `x.value` (float, NaN if `!x.valid`), `x.unit`, `x.text`, `x.valid` |
| `on_error` | fires on timeout/negative response/transport error: `x.what`, `x.nrc` (0 for a timeout/transport error) |

### `sensor: platform: uds` — two shapes

A **value sensor** names a field:

| key | meaning |
|---|---|
| `field` | the field's name, from the manifest |
| `service` | required only when `field` occurs in more than one group; a canonical group name, an original service qualifier, or a DID (`0x0207`) |
| `element` | 0-based array index for a collapsed array field; `0`/omitted for a scalar. Element 0 is the factory's element 1 |
| `update_interval` | polls this field's group at the fastest interval any bound sensor asks for; `never` means read-on-demand only (via `uds.read`) |

A **diagnostics** entry names counters instead of a field, one polling
component reporting on the hub itself: `requests_sent`,
`responses_accepted`, `timeouts`, `negative_responses`, `decode_unmatched`,
`groups_suspended`, `partial_responses`, `fields_uncovered`, `last_nrc`. Read
`partial_responses`/`fields_uncovered` together when an entity is
unexpectedly unavailable — see §7.

### `text_sensor: platform: uds`

Same `field`/`service`/`element` keys. Used for ASCII fields (a VIN), HEXDUMP
fields, and enumerated fields whose scale rows carry text — the catalog
decides which of the three, and declaring a numeric field here (or an ASCII
one under `sensor:`) is a config-time error naming the platform it actually
belongs on. Defaults `update_interval: never` — identity fields don't change,
so the honest default is "read once, on demand."

### Actions

| action | needs `allow_active_services` | does |
|---|---|---|
| `uds.read: {id, service}` | no | queue a read of a `SAFE_READ` group once, regardless of the flag |
| `uds.execute: {id, service}` | **yes** | run a non-read service (session, routine, IO control, …) — rejected at config time without the flag |
| `uds.raw: {id, data}` | **yes** | send exact request bytes — `data` is a list of hex bytes or a `!lambda` returning `std::vector<uint8_t>` |
| `uds.set_enabled: {id, enabled}` | no | pause/resume this hub's scheduler entirely |

## 6. Safety model

TX on a vehicle bus is safety-relevant (a repo-wide rule — diagnostics
transmits by definition, so the bound is on *what* can be transmitted, not on
refusing to transmit at all):

- The scheduler polls **`SAFE_READ` groups only** — the catalog flags which
  service identifiers cannot change ECU state, and `uds.read` is held to the
  same gate no matter what `allow_active_services` says.
- Sessions, routines, IO control, writes, security access and reset are
  reachable only through `uds.execute`/`uds.raw`, **and** only when the hub
  sets `allow_active_services: true`. Both the flag and the action are
  explicit; the config-time validator rejects an `execute`/`raw` on a hub
  without the flag.
- Declaring a sensor with an `update_interval` *is* the opt-in for polling —
  nothing is read without one being declared somewhere.

Default every hub to `allow_active_services: false` unless you specifically
need it, and keep it observe-first the way the rest of this repo does.

## 7. Troubleshooting

| symptom | what it means |
|---|---|
| `catalog unavailable`, logged once at boot | no catalog flashed yet, or the partition name/size in YAML doesn't match what's on flash — flash it (§3.3) |
| CRC mismatch warning in `dump_config` | the catalog was reflashed independently of the firmware — harmless as long as `Bindings: N of N` still matches |
| a field reads NaN | the catalog's `INVALID` sentinel matched the raw value — check `on_error`/counters for whether the *group* actually failed (a negative response or timeout) before assuming a decode bug |
| `partial_responses` climbing, `fields_uncovered` climbing with it | the ECU answered with fewer array elements than the database declares (e.g. an actual pack with fewer cells wired than the catalog's array models) — this is `resp_min_len` acting as a hint, not a gate, and is ordinary |
| `timeouts` climbing | the ECU is silent for that group — the one counter that means something is actually wrong, along with `negative_responses` |
| "unknown field" at config time | read the manifest; the closest matches are listed in the error |
| "field occurs in more than one group" | add `service:` — the error names the candidate groups |
| "N-byte request this build cannot assemble" | the catalog and the C++ reader disagree, or a request exceeds the component's limit — see `DESIGN-uds.md` §7a for the one historical case of this |
| a config error about `max_message_size` | §3.1's transport check: the `isotp` instance's buffer can't hold the largest group this config reads — raise `max_message_size`, don't lower what you read |

## 8. Going deeper

- [`DESIGN-uds.md`](DESIGN-uds.md) — full design rationale: why a memory-mapped
  partition, the request/response validation rules, NRC handling, scheduling,
  the ESPHome-facing shape of the C++ classes.
- [`uds-catalog-format.md`](uds-catalog-format.md) — the normative `.dcat`
  binary format, if you're compiling your own catalog or debugging one.
- `script/uds_catalog.py --help` — `compile` / `dump` / `verify` / `flash`.
- `tests/build/uds/common.yaml` + `common_partition.yaml` — minimal,
  fully-annotated worked examples of both catalog sources, including two hubs
  sharing one catalog on two address pairs (an ECU and a responder rig
  shape), all against the checked-in `mini` fixture.
- `private/hil/` (gitignored, bench-specific, see `docs/CONVENTIONS.md`) is
  where a full real-hardware configuration — a real ECU and a responder rig
  that can be told to misbehave on demand — actually lives for a given bench.
