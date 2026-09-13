"""sd_logger: buffered SD-card bus datalogger for the ESP32-C6.

Captures records into a RAM ring from producer context (the ``sd_logger.log``
action for now; native can_gateway/linbus taps are a later increment) and drains
them to a FAT file from a dedicated writer task, so SD write-latency spikes never
reach the CAN forwarding ISR or the LIN task. See docs/sd_logger-spec.md.

Validation rules are numbered V1-V28 to match the sibling components' convention;
each validator names its number, and the schema tests in tests/sd_logger/
reference the same numbers. V10 is deferred; V7/V11/V12 arrived with the native
can_gateway tap (M2), V13-V18 with write format v1 (spec §6), V19-V21 with card
recovery (spec §7 Layer C), V22-V26 with chunk collection
(docs/sdlog-collection-design.md), V27-V28 with the in-band card reset
(components/sd_logger/card_reset.h).
"""

from __future__ import annotations

import logging
import math
import re

from esphome import automation, core
import esphome.codegen as cg
from esphome.components.esp32 import (
    include_builtin_idf_component,
    only_on_variant,
    require_vfs_dir,
)
from esphome.components.esp32.const import VARIANT_ESP32C6
from esphome.components.logger import (
    CONF_TASK_LOG_BUFFER_SIZE,
    LOG_LEVEL_SEVERITY,
    request_log_listener,
)
from esphome.components import time
import esphome.config_validation as cv
from esphome.const import (
    CONF_DATA,
    CONF_ID,
    CONF_LEVEL,
    CONF_LOGGER,
    CONF_PORT,
    CONF_SOURCE,
    CONF_WEB_SERVER,
    CONF_WIFI,
)
from esphome.core import CORE
import esphome.final_validate as fv
from esphome import pins

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@kipp-ing"]
DEPENDENCIES = ["esp32"]

# The native CAN tap (S2) layers on can_gateway, but sd_logger must keep working
# without it: a user who pulls only `components: [sd_logger]` through
# external_components has no can_gateway package to import. So the import is
# optional and `can_ports:` is the only thing that needs it (V7).
try:
    from esphome.components.can_gateway import (
        CONF_LOG_TAP,
        CONF_PORTS as CAN_GATEWAY_PORTS,
        GatewayPort,
    )

    HAS_CAN_GATEWAY = True
except ImportError:  # pragma: no cover - depends on which components were pulled
    CONF_LOG_TAP = "log_tap"
    CAN_GATEWAY_PORTS = "ports"
    GatewayPort = None
    HAS_CAN_GATEWAY = False

sd_logger_ns = cg.esphome_ns.namespace("sd_logger")
SdLogger = sd_logger_ns.class_("SdLogger", cg.Component)
LogAction = sd_logger_ns.class_("LogAction", automation.Action)

CONF_CLK_PIN = "clk_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_CS_PIN = "cs_pin"
CONF_CLOCK = "clock"
CONF_MOUNT_POINT = "mount_point"
CONF_CARD_POWER_PIN = "card_power_pin"
CONF_BUFFER_DEPTH = "buffer_depth"
CONF_SYNC_INTERVAL = "sync_interval"
CONF_MAX_FILE_SIZE = "max_file_size"
CONF_FORMAT_IF_MOUNT_FAILED = "format_if_mount_failed"
CONF_STATISTICS = "statistics"
CONF_LOG_INTERVAL = "log_interval"
CONF_VCC_MONITOR = "vcc_monitor"
CONF_ADC_PIN = "adc_pin"
CONF_THRESHOLD = "threshold"
CONF_DIVIDER = "divider"
CONF_ADC_FULL_SCALE = "adc_full_scale"
CONF_CAN_ID = "can_id"
CONF_CAN_PORTS = "can_ports"
CONF_LABEL = "label"
CONF_SOURCES = "sources"
CONF_KIND = "kind"
CONF_TAG = "tag"
CONF_ESPHOME_LOGS = "esphome_logs"
CONF_RECOVERY = "recovery"
CONF_ENABLED = "enabled"
CONF_INITIAL_DELAY = "initial_delay"
CONF_MAX_DELAY = "max_delay"
CONF_MAX_ATTEMPTS = "max_attempts"
CONF_POWER_CYCLE = "power_cycle"
CONF_IN_BAND_RESET = "in_band_reset"
CONF_BUSY_TIMEOUT = "busy_timeout"
CONF_MAX_BUSY_TIMEOUT = "max_busy_timeout"
CONF_MAX_FILE_SECONDS = "max_file_seconds"
CONF_COLLECTION = "collection"
CONF_RETENTION_PERCENT = "retention_percent"
CONF_MAX_CHUNKS = "max_chunks"
CONF_SERVE = "serve"
CONF_TIME_ID = "time_id"
CONF_ORIGIN = "origin"

# Source tag 0 is the `sd_logger.log` action (S1). Native taps start at 1 so a
# record's origin stays readable in the file without a side table (V11).
ACTION_SOURCE_TAG = 0

# The `kind` of a declared source, and the type letter it becomes in the file
# (spec §6 F1a). `I` is reserved for isotp (S5) and is not configurable yet.
KIND_LETTERS = {"can": "C", "lin": "L", "user": "U"}

# V13: a label is an identifier, and a short one. Every character costs ~3.6 KB/s
# at production load (spec F1g), which is why 8 is the cap and 4 the advice.
LABEL_MAX = 8
LABEL_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,8}$")

# V18: text-ring depth. Small because a log line is 192 B of slot, and the ring
# only has to cover one writer poll period.
TEXT_DEPTH_MIN = 8
TEXT_DEPTH_MAX = 256

# Where _final_validate leaves the label -> tag map, so the `sd_logger.log`
# action can accept a declared label instead of a bare number. Validation of
# every component finishes before any to_code runs, so the map is always there
# by the time the action is generated.
KEY_SOURCE_LABELS = "sd_logger_source_labels"

# H1: SD over SPI on the C6 is capped at SDMMC_FREQ_DEFAULT (20 MHz). Do not
# silently clamp a higher value — reject it so the ceiling is explicit (V2).
SDSPI_MAX_HZ = 20_000_000
SDSPI_MIN_HZ = 400_000

# H4: the C6 has one SAR ADC (ADC1) on GPIO0-GPIO6. A VCC-divider tap must land
# on one of those (V3).
ADC1_GPIO_MIN = 0
ADC1_GPIO_MAX = 6


def _validate_clock(value):
    """V2: SPI clock, accepts '20MHz'/'400kHz'/plain Hz, capped at the SD-over-SPI
    ceiling on the C6."""
    hz = int(cv.frequency(value))
    if not SDSPI_MIN_HZ <= hz <= SDSPI_MAX_HZ:
        raise cv.Invalid(
            f"clock must be between 400kHz and 20MHz; SD-over-SPI on the ESP32-C6 "
            f"is capped at SDMMC_FREQ_DEFAULT (20MHz)"
        )
    return hz


def _validate_size(value):
    """A byte size as an int, or a '16MB' / '512KB' string."""
    if isinstance(value, int):
        return value
    text = str(value).strip().upper().replace(" ", "")
    mult = 1
    if text.endswith("MB"):
        mult, text = 1024 * 1024, text[:-2]
    elif text.endswith("KB"):
        mult, text = 1024, text[:-2]
    elif text.endswith("B"):
        text = text[:-1]
    try:
        return int(float(text) * mult)
    except ValueError as err:
        raise cv.Invalid(f"invalid size {value!r}") from err


# V23: the time bound on rotation. The floor keeps the writer from sealing chunks
# faster than it can close them; the ceiling keeps a chunk small enough that a
# failed transfer is a cheap retry over car WiFi (design §4).
MAX_FILE_SECONDS_MIN = 1
MAX_FILE_SECONDS_MAX = 3600


def _max_file_seconds_invalid(value) -> cv.Invalid:
    return cv.Invalid(
        f"'{CONF_MAX_FILE_SECONDS}' must be between 1s and 3600s (got {value!r}). "
        f"Below a second the writer would be sealing chunks faster than it can "
        f"close and reopen one; above an hour a chunk is no longer the cheap "
        f"retry over car WiFi the pull model is built on."
    )


def _max_file_seconds_not_whole(value, milliseconds) -> cv.Invalid:
    """A *precision* complaint, deliberately not the range one.

    `1500ms` is inside [1s, 3600s]; what is wrong with it is that the writer checks
    the rotation deadline once per drain pass, so half a second cannot be honoured
    and would be rounded away in silence. Telling the user it is out of range —
    which the first cut of this validator did — sends them looking for a bound
    that is not the problem.
    """
    return cv.Invalid(
        f"'{CONF_MAX_FILE_SECONDS}' is a whole number of seconds; {value!r} is "
        f"{milliseconds:g}ms. The rotation deadline is only tested once per writer "
        f"drain pass, so sub-second precision would be rounded away rather than "
        f"honoured. Write the nearest whole second."
    )


def _max_file_seconds_milliseconds(value):
    """Every accepted spelling of `max_file_seconds`, normalised to milliseconds.

    Three spellings arrive here and all three are legitimate:

    * a bare number — `60` — because the key names its own unit, so demanding
      `60s` would be pedantry;
    * that same number as a **string** — `'60'` — because a quoted scalar and
      *every* `${substitution}` produce one, and `max_file_size` has always taken
      the string form (`int(float(text))`) for exactly that reason. Sending `'60'`
      down the duration parser instead would read it as 60 **milliseconds** and
      then reject it as out of range, which is a lie about a perfectly good value;
    * an ESPHome duration — `'60s'`, `'2min'`, `'1h'` — because every other
      duration in this schema is written with a unit, so rejecting `60s` would be
      a trap.

    Milliseconds rather than seconds is what lets the caller separate "outside the
    bounds" from "finer than a second"; both used to collapse into the range error.
    """
    number = None
    if isinstance(value, (int, float)):
        number = value
    elif isinstance(value, str):
        text = value.strip()
        try:
            number = int(text)
        except ValueError:
            try:
                number = float(text)
            except ValueError:
                number = None  # not a bare number: try the duration grammar

    if number is None:
        try:
            return cv.positive_time_period_milliseconds(value).total_milliseconds
        except cv.Invalid as err:
            raise _max_file_seconds_invalid(value) from err

    if isinstance(number, float) and not math.isfinite(number):
        # `.inf` and `.nan` are legal YAML. `int(inf)` raises OverflowError, which
        # voluptuous does not convert into a config error, so it escapes as a
        # traceback out of `esphome config`; `int(nan)` raises ValueError and
        # surfaces as the generic "not a valid value" with no mention of the key.
        # Reject both here, in the key's own words.
        raise _max_file_seconds_invalid(value)
    return number * 1000


def _validate_max_file_seconds(value):
    """V23: rotation's time bound, in [1s, 3600s], as a whole number of seconds."""
    if isinstance(value, bool):
        # YAML `true` is an int in Python, and `max_file_seconds: true` would
        # otherwise validate as a one-second rotation.
        raise _max_file_seconds_invalid(value)
    milliseconds = _max_file_seconds_milliseconds(value)
    if not (
        MAX_FILE_SECONDS_MIN * 1000 <= milliseconds <= MAX_FILE_SECONDS_MAX * 1000
    ):
        raise _max_file_seconds_invalid(value)
    if milliseconds % 1000:
        # Reached only from inside the range, so the message can be about precision
        # instead of bounds. `1.9` lands here too: truncating it to 1 s silently
        # would be the same bug wearing a number.
        raise _max_file_seconds_not_whole(value, milliseconds)
    return int(milliseconds) // 1000


def _validate_buffer_depth(value):
    """V5: ring depth must be a power of two (index masking) in a sane range."""
    depth = cv.int_(value)
    if not 256 <= depth <= 16384:
        raise cv.Invalid("buffer_depth must be between 256 and 16384 records")
    if depth & (depth - 1):
        raise cv.Invalid(f"buffer_depth must be a power of two (got {depth})")
    return depth


def _validate_can_port_ref(value):
    """V7: `can_ports:` needs the can_gateway component to be available at all."""
    if not HAS_CAN_GATEWAY:
        raise cv.Invalid(
            f"'{CONF_CAN_PORTS}' needs the 'can_gateway' component, which is not "
            f"installed. Pull it alongside sd_logger in your external_components "
            f"block, or drop '{CONF_CAN_PORTS}' and feed records with the "
            f"'sd_logger.log' action instead."
        )
    return cv.use_id(GatewayPort)(value)


def _validate_label(value):
    """V13: a label is an identifier of at most 8 characters.

    It is the column that names a bus on every single line, so it has to be
    parseable without quoting (no comma, no newline) and cheap: per-field cost is
    ~3.6 KB/s per character at production load, which is why the cap is 8 and the
    recommendation is 4 (spec F1g).
    """
    text = cv.string_strict(value)
    if not LABEL_PATTERN.match(text):
        raise cv.Invalid(
            f"'{text}' is not a usable label: use 1-{LABEL_MAX} characters from "
            f"A-Z a-z 0-9 _ - . The label names its bus on every line of the "
            f"file, so it must survive being read back without quoting."
        )
    return text


CAN_PORT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PORT): _validate_can_port_ref,
        # Written into every record from this port, so the two segments stay
        # distinguishable in one file. Defaults to 1 + position in the list.
        cv.Optional(CONF_SOURCE): cv.int_range(min=1, max=255),
        # How that tag reads in the file. Defaults to the interface number —
        # C1, C2, ... 1-based, in the order the entries are declared.
        cv.Optional(CONF_LABEL): _validate_label,
    }
)

SOURCE_SCHEMA = cv.Schema(
    {
        # V14: the tag a producer stamps. 0 is the `sd_logger.log` action's own
        # default, so declaring it is how the action's records get a name.
        cv.Required(CONF_TAG): cv.int_range(min=0, max=255),
        cv.Required(CONF_KIND): cv.one_of(*KIND_LETTERS, lower=True),
        cv.Required(CONF_LABEL): _validate_label,
    }
)


def _default_can_port_label(index: int) -> str:
    """A tapped port's label defaults to its **interface number**: `C1`, `C2`, ...
    1-based, in the order `can_ports:` declares them.

    Until format v2 it defaulted to the port's own ESPHome id, which reads better in
    the YAML and is the most expensive field on the card: the label is written out on
    *every record*, so `seg1` against `C1` is 2 B at ~3600 rec/s — ~7 KB/s — and an id
    is unbounded where a label is capped at 8 (F1g).

    Positional numbering also deletes a whole failure mode rather than moving it. The
    id-derived default had to reject an id that was not a legal label (`seg 1`, or
    anything over 8 characters), because truncating one risked two ports answering to
    the same name; a config could therefore be invalid for a reason that had nothing
    to do with logging. `C<n>` is legal by construction and unique by construction, so
    that error path is gone, not merely unreachable.

    Two characters is the floor that stays legible. A bare `1` would be one byte
    narrower and the `#src` table already maps it, but the line would then name its bus
    with a number indistinguishable from the other numbers on it, and reading a card at
    the bench would need the header open alongside. The kind letter plus its index
    keeps the column self-evident.

    `sources:` entries have no default — their label is required — so nothing else in
    the schema derives a name from an id. When the LIN tap lands (S3) the parallel is
    `L1`, `L2`: same kind letter, same 1-based interface order.

    `label:` still overrides, and is the right answer whenever a rig has names worth
    carrying (`eng`, `body`). An explicit one still has to be unique — writing
    `label: C2` on the first interface collides with the second one's default and is
    rejected by V13.
    """
    return f"{KIND_LETTERS['can']}{index + 1}"


def _validate_can_ports(config):
    """V11: source tags must be unique, and a port may only be tapped once.

    Both mistakes produce a file that looks fine and is silently ambiguous: two
    segments sharing a tag cannot be told apart afterwards, and a port listed
    twice would have its single ring drained by two entries, splitting one
    segment's frames across two tags at random.
    """
    seen_ports: dict[str, int] = {}
    seen_tags: dict[int, str] = {}
    for index, entry in enumerate(config):
        port = str(entry[CONF_PORT])
        if port in seen_ports:
            raise cv.Invalid(
                f"can_gateway port '{port}' is tapped twice (entries "
                f"{seen_ports[port]} and {index}); a port has one tap ring, so "
                f"list it once",
                path=[index, CONF_PORT],
            )
        seen_ports[port] = index
        tag = entry.get(CONF_SOURCE, index + 1)
        if tag in seen_tags:
            raise cv.Invalid(
                f"'{CONF_SOURCE}' {tag} is already used by port "
                f"'{seen_tags[tag]}'; every tapped port needs a distinct source "
                f"tag or its records cannot be told apart in the log",
                path=[index, CONF_SOURCE],
            )
        seen_tags[tag] = port
        entry[CONF_SOURCE] = tag
        entry.setdefault(CONF_LABEL, _default_can_port_label(index))
    return config


def _validate_sources_and_labels(config):
    """V13/V14: tags and labels are unique across `can_ports` **and** `sources`.

    Same reasoning as V11, in both directions. Two wires answering to one label
    makes every line ambiguous, and two labels claiming one tag makes the file
    disagree with its own `#src` header — and the file is the only artifact that
    survives the run.
    """
    can_ports = config.get(CONF_CAN_PORTS) or []
    sources = config.get(CONF_SOURCES) or []

    tags: dict[int, str] = {}
    labels: dict[str, str] = {}

    def claim(tag: int, label: str, where: str, path: list) -> None:
        if tag in tags:
            raise cv.Invalid(
                f"source tag {tag} is claimed by both {tags[tag]} and {where}; "
                f"a tag names exactly one producer or its records cannot be told "
                f"apart in the log",
                path=path + [CONF_TAG if where.startswith("sources") else CONF_SOURCE],
            )
        if label in labels:
            raise cv.Invalid(
                f"label '{label}' is claimed by both {labels[label]} and {where}; "
                f"two producers answering to one label makes every line that "
                f"carries it ambiguous",
                path=path + [CONF_LABEL],
            )
        tags[tag] = where
        labels[label] = where

    for index, entry in enumerate(can_ports):
        claim(
            entry[CONF_SOURCE],
            entry[CONF_LABEL],
            f"can_ports[{index}] ('{entry[CONF_PORT]}')",
            [CONF_CAN_PORTS, index],
        )
    for index, entry in enumerate(sources):
        claim(
            entry[CONF_TAG],
            entry[CONF_LABEL],
            f"sources[{index}] ('{entry[CONF_LABEL]}')",
            [CONF_SOURCES, index],
        )
    return config


def _validate_text_depth(value):
    """V18: text-ring depth, a power of two (index masking) in a small range."""
    depth = cv.int_(value)
    if not TEXT_DEPTH_MIN <= depth <= TEXT_DEPTH_MAX:
        raise cv.Invalid(
            f"{CONF_ESPHOME_LOGS}.{CONF_BUFFER_DEPTH} must be between "
            f"{TEXT_DEPTH_MIN} and {TEXT_DEPTH_MAX} slots"
        )
    if depth & (depth - 1):
        raise cv.Invalid(
            f"{CONF_ESPHOME_LOGS}.{CONF_BUFFER_DEPTH} must be a power of two "
            f"(got {depth})"
        )
    return depth


ESPHOME_LOGS_SCHEMA = cv.Schema(
    {
        # The ceiling on what is captured. It can never see more than
        # `logger.level` already let through — V16 warns when it is asked to.
        cv.Optional(CONF_LEVEL, default="INFO"): cv.one_of(
            *LOG_LEVEL_SEVERITY, upper=True
        ),
        cv.Optional(CONF_BUFFER_DEPTH, default=32): _validate_text_depth,
    }
)


RECOVERY_DELAY_MIN_MS = 100
RECOVERY_DELAY_MAX_MS = 300_000


def _validate_recovery_delay(value):
    """V20: a retry delay, between 100 ms and 300 s."""
    period = cv.positive_time_period_milliseconds(value)
    milliseconds = period.total_milliseconds
    if not RECOVERY_DELAY_MIN_MS <= milliseconds <= RECOVERY_DELAY_MAX_MS:
        raise cv.Invalid(
            f"a recovery delay must be between 100ms and 300s (got {milliseconds}ms). Below "
            f"100ms the retry lands inside the card's own power-on ramp and fails for that "
            f"reason alone, which reads as a dead card."
        )
    return period


BUSY_TIMEOUT_MIN_MS = 100
BUSY_TIMEOUT_MAX_MS = 60_000


def _validate_busy_timeout(value):
    """V27: how long one in-band reset may wait for the card to release the bus."""
    period = cv.positive_time_period_milliseconds(value)
    milliseconds = period.total_milliseconds
    if not BUSY_TIMEOUT_MIN_MS <= milliseconds <= BUSY_TIMEOUT_MAX_MS:
        raise cv.Invalid(
            f"a busy timeout must be between 100ms and 60s (got {milliseconds}ms). Below "
            f"100ms it is no more patient than the SDSPI driver's own 40ms, which is the "
            f"limitation this exists to lift; above 60s the writer task is stalled inside one "
            f"recovery attempt for longer than an orderly shutdown can wait for it."
        )
    return period


RECOVERY_SCHEMA = cv.Schema(
    {
        # On by default. Before this existed, any card failure — including the one a reflash
        # causes by resetting the board mid-write — disabled logging until the next 12 V cycle.
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_INITIAL_DELAY, default="1s"): _validate_recovery_delay,
        # The ceiling the doubling ladder walks up to.
        cv.Optional(CONF_MAX_DELAY, default="30s"): _validate_recovery_delay,
        # 0 is "keep trying for as long as the board runs", which is the right default in a car:
        # nobody is there to power-cycle it, and a logger that gave up an hour in is worth less
        # than one that comes back for the second half of the drive.
        cv.Optional(CONF_MAX_ATTEMPTS, default=0): cv.int_range(min=0, max=1000),
        # Defaulted in _validate_recovery() from whether card_power_pin exists (V19).
        cv.Optional(CONF_POWER_CYCLE): cv.boolean,
        # On by default, and unlike power_cycle it needs no hardware: it is the only recovery step
        # that addresses what a reset mid-write actually leaves behind — a multi-block write the
        # card is still waiting to have ended. From the second attempt on (the first is a plain
        # remount, which is all a transient failure needs).
        cv.Optional(CONF_IN_BAND_RESET, default=True): cv.boolean,
        # How long one attempt waits for the card to release the bus. The SDSPI driver's own budget
        # is 40 ms and cannot exceed 127 ms; a card finishing an interrupted write can need far
        # more, and nothing in this project has ever measured how much.
        cv.Optional(CONF_BUSY_TIMEOUT, default="2s"): _validate_busy_timeout,
        cv.Optional(CONF_MAX_BUSY_TIMEOUT, default="10s"): _validate_busy_timeout,
    }
)


# V24: the card-fill percentage that arms "drop oldest un-collected" (design §7).
# Both ends are exclusive: at 99 % the headroom left is smaller than one chunk, so
# retention would only ever arm once the card is already full and logging has
# stopped; at 1 % it would delete chunks about as fast as the writer seals them,
# which is a permanent gap dressed up as a policy.
#
# Note this is *tighter* than the runtime: CollectionPolicy::configure() clamps into
# [1, 99] (collection_policy.h), and that clamp is a defensive last resort for a
# value that did not come through this schema, not a second opinion about policy.
# The schema is the narrower of the two on purpose, so nothing a user can write is
# silently rewritten by the clamp. collection_policy.h and design §11 describe the
# same split; if the three ever drift again, this schema is the truth and they are
# the ones to correct.
RETENTION_PERCENT_MIN = 1
RETENTION_PERCENT_MAX = 99


def _validate_retention_percent(value):
    """V24: card fill that arms retention, strictly between 1 and 99."""
    try:
        return cv.int_range(
            min=RETENTION_PERCENT_MIN,
            max=RETENTION_PERCENT_MAX,
            min_included=False,
            max_included=False,
        )(value)
    except cv.Invalid as err:
        raise cv.Invalid(
            f"'{CONF_RETENTION_PERCENT}' must be strictly between 1 and 99 (got "
            f"{value!r}). It is the card fill at which the oldest un-collected "
            f"chunk starts being discarded, so it needs at least a chunk's worth "
            f"of headroom above it and more than a chunk's worth of history below."
        ) from err


# How many chunks the in-RAM index tracks — `SD_LOG_MAX_CHUNKS` in
# collection_policy.h, which the index sizes a static array from. The bounds here
# are RAM, not representation: the header's static_assert allows [1, 65535]
# because `count_` is a uint16_t, and 65535 entries would be 768 KB on a chip with
# ~100-200 KB free.
#
# 12 B an entry (see `ChunkEntry`), so 2048 entries is 24 KB — the largest index
# this component is willing to spend without the config saying something else has
# to give, and ~14 hours of coverage at the bench's ~25 s per chunk. The floor is
# 16 rather than 1 because the boot scan reserves one slot for the file the boot
# is about to open and retention needs a history to choose a victim from: an index
# of 2 is not a small index, it is a broken one, and the refusal it produces looks
# exactly like the bug this range exists to prevent.
MAX_CHUNKS_MIN = 16
MAX_CHUNKS_MAX = 2048


def _max_chunks_invalid(value, reason):
    """The V26 refusal, in the key's own words. `reason` says which rule was broken."""
    return cv.Invalid(
        f"'{CONF_MAX_CHUNKS}' {reason} (got {value!r}). It is how many chunks the "
        f"in-RAM index can track at 12 bytes each, so {MAX_CHUNKS_MAX} is 24 KB of "
        f"static RAM; a chunk the index cannot hold is never listed, never served "
        f"and never reclaimed by retention, so size it for the longest run between "
        f"collections: at 'max_file_size: 4MB' on a busy bus a chunk lands roughly "
        f"every 25 s."
    )


def _validate_max_chunks(value):
    """V26: index capacity, in chunks. See MAX_CHUNKS_MIN/MAX above for the bounds."""
    in_range = f"must be between {MAX_CHUNKS_MIN} and {MAX_CHUNKS_MAX}"

    # No boolean special case, unlike V23's validator: `true` is an int in Python
    # and reaches the range check as 1, which the floor of 16 refuses on its own.
    # V23 needs one because its floor is 1 second and `true` would have validated.

    if isinstance(value, float) and not math.isfinite(value):
        # `.inf` and `.nan` are legal YAML, and this is the SAME defect V23 already
        # found and fixed a few hundred lines up — reintroduced here and caught by
        # the V26 tests. `int(inf)` raises OverflowError, which is neither
        # `cv.Invalid` nor `ValueError`, so voluptuous does not convert it and it
        # escapes as a traceback out of `esphome config` instead of as a config
        # error naming the key. Reject both before `cv.int_range` can see them.
        raise _max_chunks_invalid(value, in_range)

    try:
        return cv.int_range(min=MAX_CHUNKS_MIN, max=MAX_CHUNKS_MAX)(value)
    except cv.Invalid as err:
        # Two different refusals arrive here and they must not share a message.
        # `cv.int_range` rejects `256.5` for having a fractional part, and saying
        # "must be between 16 and 2048" of a value that IS between 16 and 2048
        # sends the reader after a bound that is not the problem. Ask the value
        # whether it is a whole number rather than parsing the upstream message.
        try:
            whole = int(value) == float(value)
        except (TypeError, ValueError):
            whole = True  # not numeric at all: the bounds message is the honest one
        reason = in_range if whole else "must be a whole number of chunks"
        raise _max_chunks_invalid(value, reason) from err


COLLECTION_SCHEMA = cv.Schema(
    {
        # Declared but down is a real state: `enabled: false` keeps the port and the
        # retention threshold written down while nothing is indexed, retained or
        # served — the same shape `recovery:` uses.
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        # The **network** half, split from `enabled` because the two fail separately.
        # `enabled` bounds the *card* — the chunk index, "drop oldest un-collected"
        # and the `#gap` line that states the loss in-band. `serve` is whether a
        # puller ever comes to fetch the chunks, and it is what V22 and V25 are
        # about: a server needs an interface to bind and a port nobody else holds.
        #
        # `serve: false` is a real configuration twice over. A device whose card is
        # bounded and whose chunks are collected by pulling the card out wants
        # exactly it — and so does the bench, which has to soak rotation, retention
        # and `#gap` *without* WiFi: WiFi brings tasks above the LIN task's priority
        # and its effect on bus timing is unmeasured (design §9.1), so a soak that
        # had to associate would be measuring two unknowns at once. Defaults to
        # true, so no config that validated before this key existed changes meaning.
        cv.Optional(CONF_SERVE, default=True): cv.boolean,
        # A private esp_http_server on its own port, deliberately not esphome's
        # `web_server` (design §5a). 8080 rather than 80 so the common config does
        # not collide with a web_server that a user adds later (V25).
        cv.Optional(CONF_PORT, default=8080): cv.port,
        cv.Optional(CONF_RETENTION_PERCENT, default=80): _validate_retention_percent,
        # The index capacity, not a card bound: it decides how many chunks
        # retention can *see*, and a chunk it never saw is one it can never
        # reclaim. Defaults to the header's own default so a config that omits the
        # key and one that writes `max_chunks: 256` produce the same firmware.
        cv.Optional(CONF_MAX_CHUNKS, default=256): _validate_max_chunks,
    }
)


VCC_MONITOR_SCHEMA = cv.Schema(
    {
        # V3: ADC1-capable pin only.
        cv.Required(CONF_ADC_PIN): cv.int_range(min=ADC1_GPIO_MIN, max=ADC1_GPIO_MAX),
        # Rail voltage that triggers the emergency close. cv.voltage -> float volts.
        cv.Required(CONF_THRESHOLD): cv.voltage,
        # Rail-to-ADC divider ratio: V_rail = V_adc * divider.
        cv.Required(CONF_DIVIDER): cv.positive_float,
        # ADC full-scale in millivolts at 12 dB attenuation (linear approximation;
        # this is a sag detector, not a calibrated meter — see the .cpp note).
        cv.Optional(CONF_ADC_FULL_SCALE, default=3100): cv.int_range(
            min=1000, max=3300
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SdLogger),
            cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_MOSI_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_MISO_PIN): pins.internal_gpio_input_pin_number,
            cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_CLOCK, default="20MHz"): _validate_clock,
            cv.Optional(CONF_MOUNT_POINT, default="/sdcard"): cv.string_strict,
            cv.Optional(
                CONF_CARD_POWER_PIN
            ): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_BUFFER_DEPTH, default=2048): _validate_buffer_depth,
            cv.Optional(
                CONF_SYNC_INTERVAL, default="2s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_FILE_SIZE, default="16MB"): cv.All(
                _validate_size, cv.int_range(min=64 * 1024, max=1024 * 1024 * 1024)
            ),
            # V23: no default on purpose. `max_file_size` already guarantees
            # rotation has a bound, so defaulting this to '60s' would silently start
            # rotating every minute on every already-released config — and rotation
            # is the one path that has never fired on hardware (design §4). Absent
            # is size-only, i.e. exactly today's behaviour.
            cv.Optional(CONF_MAX_FILE_SECONDS): _validate_max_file_seconds,
            cv.Optional(CONF_FORMAT_IF_MOUNT_FAILED, default=False): cv.boolean,
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_ORIGIN): cv.string,
            cv.Optional(CONF_CAN_PORTS): cv.All(
                cv.ensure_list(
                    cv.maybe_simple_value(CAN_PORT_SCHEMA, key=CONF_PORT)
                ),
                cv.Length(min=1),
                _validate_can_ports,
            ),
            cv.Optional(CONF_SOURCES): cv.All(
                cv.ensure_list(SOURCE_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(CONF_ESPHOME_LOGS): ESPHOME_LOGS_SCHEMA,
            # Always present, so recovery is on unless a config turns it off.
            cv.Optional(CONF_RECOVERY, default={}): RECOVERY_SCHEMA,
            # No `default={}` here, unlike recovery: serving chunks costs a socket,
            # a task and a port, and V22 makes the block require `wifi:` — so a
            # default-present block would demand WiFi of every existing config.
            cv.Optional(CONF_COLLECTION): COLLECTION_SCHEMA,
            cv.Optional(CONF_VCC_MONITOR): VCC_MONITOR_SCHEMA,
            cv.Optional(CONF_STATISTICS): cv.Schema(
                {
                    cv.Optional(
                        CONF_LOG_INTERVAL, default="60s"
                    ): cv.positive_time_period_milliseconds,
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32C6], msg_prefix="sd_logger"),
    # esp_vfs_fat_sdspi_mount / the node-friendly SD-SPI host live in IDF >= 5.5,
    # matching the sibling components' floor.
    cv.require_framework_version(esp_idf=cv.Version(5, 5, 0)),
)


def _pins_distinct(config):
    """V1/V8: SPI pins, CS, an optional card-power pin and an optional ADC tap
    must all be distinct GPIO."""
    used: dict[int, str] = {}
    keys = [CONF_CLK_PIN, CONF_MOSI_PIN, CONF_MISO_PIN, CONF_CS_PIN]
    if CONF_CARD_POWER_PIN in config:
        keys.append(CONF_CARD_POWER_PIN)
    collected = [(config[key], key) for key in keys]
    if (vcc := config.get(CONF_VCC_MONITOR)) is not None:
        collected.append((vcc[CONF_ADC_PIN], f"{CONF_VCC_MONITOR}.{CONF_ADC_PIN}"))
    for pin, key in collected:
        if pin in used:
            raise cv.Invalid(
                f"GPIO{pin} is used by both '{used[pin]}' and '{key}'; "
                f"every sd_logger pin must be distinct"
            )
        used[pin] = key
    return config


def _validate_recovery(config):
    """V19/V20/V21/V28: the recovery block against the rest of the config.

    V19  `power_cycle: true` needs `card_power_pin:` — without a high-side switch there is
         nothing to cycle, and remounting alone does not revive a wedged card.
    V20  `max_delay` cannot sit below `initial_delay`, or the ladder would step backwards.
    V21  `format_if_mount_failed:` deliberately does not apply to retries; say so.
    V28  `max_busy_timeout` cannot sit below `busy_timeout`, for the same reason as V20.
    """
    recovery = config[CONF_RECOVERY]
    has_power_pin = CONF_CARD_POWER_PIN in config

    if CONF_POWER_CYCLE not in recovery:
        # Cycle the card whenever there is a switch to do it with: that is the entire reason the
        # switch is on the board (spec §7 H5), and a wedged card is the failure it addresses.
        recovery[CONF_POWER_CYCLE] = has_power_pin
    elif recovery[CONF_POWER_CYCLE] and not has_power_pin:
        raise cv.Invalid(
            f"'{CONF_POWER_CYCLE}: true' needs '{CONF_CARD_POWER_PIN}:' on the sd_logger "
            f"block. Without a high-side switch on the card's supply there is nothing to "
            f"power-cycle, and a card wedged mid-write does not come back from a remount "
            f"alone — it needs its power dropped.",
            path=[CONF_RECOVERY, CONF_POWER_CYCLE],
        )

    initial_ms = recovery[CONF_INITIAL_DELAY].total_milliseconds
    max_ms = recovery[CONF_MAX_DELAY].total_milliseconds
    if max_ms < initial_ms:
        raise cv.Invalid(
            f"'{CONF_MAX_DELAY}' ({max_ms}ms) is below '{CONF_INITIAL_DELAY}' "
            f"({initial_ms}ms). The retry delay doubles from the initial value up to the "
            f"maximum, so a lower maximum would make the ladder step backwards.",
            path=[CONF_RECOVERY, CONF_MAX_DELAY],
        )

    busy_ms = recovery[CONF_BUSY_TIMEOUT].total_milliseconds
    max_busy_ms = recovery[CONF_MAX_BUSY_TIMEOUT].total_milliseconds
    if max_busy_ms < busy_ms:
        raise cv.Invalid(
            f"'{CONF_MAX_BUSY_TIMEOUT}' ({max_busy_ms}ms) is below '{CONF_BUSY_TIMEOUT}' "
            f"({busy_ms}ms). The busy budget doubles from the initial value up to the maximum "
            f"on each further attempt, so a lower maximum would make a card that needs more "
            f"patience get less of it.",
            path=[CONF_RECOVERY, CONF_MAX_BUSY_TIMEOUT],
        )

    if config[CONF_FORMAT_IF_MOUNT_FAILED] and recovery[CONF_ENABLED]:
        _LOGGER.warning(
            "sd_logger.%s is true, but card recovery deliberately never formats — only the "
            "mount at boot honours it. A retry ladder that reformatted would erase the logs "
            "it is being run to save, once per attempt.",
            CONF_FORMAT_IF_MOUNT_FAILED,
        )
    return config


CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA, _pins_distinct, _validate_sources_and_labels, _validate_recovery
)


def _find_port_config(full_config, port_id):
    """Locate a can_gateway port block by its declared id, whatever shape the
    gateway config has. Same helper as isotp's — kept local so sd_logger still
    imports without can_gateway present."""
    gateway_config = full_config.get("can_gateway")
    if gateway_config is None:
        return None
    blocks = gateway_config if isinstance(gateway_config, list) else [gateway_config]
    for block in blocks:
        for port in block.get(CAN_GATEWAY_PORTS, []):
            if port.get(CONF_ID) == port_id:
                return port
    return None


def _validate_esphome_logs(config, full_config):
    """V15/V16/V17 — the three ways log capture fails *silently* (spec §4a).

    Every one of them leaves a firmware that boots, mounts, logs bus traffic and
    simply has no `X` lines in the file, which reads exactly like a quiet run.
    """
    if (logs := config.get(CONF_ESPHOME_LOGS)) is None:
        return

    logger_config = full_config.get(CONF_LOGGER)
    # V15: without a `logger:` block, request_log_listener() is never called,
    # USE_LOG_LISTENERS is never defined, and add_log_callback() compiles to an
    # empty function. Capture would do nothing at all, with no diagnostic.
    if logger_config is None:
        raise cv.Invalid(
            f"'{CONF_ESPHOME_LOGS}' needs a 'logger:' block. Without it the log "
            f"callback compiles to a no-op and nothing is captured — silently. "
            f"Add 'logger:' to the config, or drop '{CONF_ESPHOME_LOGS}'.",
            path=[CONF_ESPHOME_LOGS],
        )

    # V16: capture can only ever see what the logger already let through, so a
    # logger at WARN with capture asking for DEBUG quietly yields WARN.
    wanted = config[CONF_ESPHOME_LOGS][CONF_LEVEL]
    global_level = logger_config.get(CONF_LEVEL, "DEBUG")
    if LOG_LEVEL_SEVERITY.index(global_level) < LOG_LEVEL_SEVERITY.index(wanted):
        _LOGGER.warning(
            "sd_logger.%s.%s is %s but logger.%s is %s: the capture only ever "
            "sees what the logger already let through, so the card will hold %s "
            "and quieter, not %s.",
            CONF_ESPHOME_LOGS,
            CONF_LEVEL,
            wanted,
            CONF_LEVEL,
            global_level,
            global_level,
            wanted,
        )

    # V17: with the task log buffer disabled, messages from tasks other than the
    # main loop bypass listeners entirely — which is exactly the writer and
    # VCC-monitor tasks, i.e. the messages a card most wants during a stall or an
    # emergency close.
    if logger_config.get(CONF_TASK_LOG_BUFFER_SIZE) == 0:
        _LOGGER.warning(
            "logger.%s is 0, so messages logged from any task other than the "
            "main loop never reach a log listener. sd_logger's own writer and "
            "VCC-monitor tasks are exactly those tasks, so their messages will "
            "be missing from the card.",
            CONF_TASK_LOG_BUFFER_SIZE,
        )
    return logs


def _validate_collection(config, full_config):
    """V22/V25 — the collection block against the rest of the config.

    V22  the chunks are fetched over the network, so a `collection:` block without a
         `wifi:` block is a server nobody can ever reach. It fails the way every
         other M6 mistake fails — by looking like a completely healthy boot — which
         is what makes it worth a config error rather than a warning.
    V25  the collection server is a private `esp_http_server` instance, deliberately
         not esphome's `web_server` (design §5a), so nothing stops the two being
         pointed at one port. Two servers binding the same port is a runtime failure
         with no config-time symptom.

    Both rules are about a *running* server, so both are gated on `enabled:` and
    on `serve:`. `collection: {enabled: false}` is declared-but-down — the same
    real state `recovery:` has, the one `CollectionPolicy::disable()` is written
    for — and `serve: false` keeps the card half (the chunk index, retention and
    the `#gap` line) while asking for no server at all. Neither opens a socket:
    there is no interface for V22 to require and no bind for V25 to lose.
    Demanding `wifi:` for a server that will never start is an argument about
    nothing, and it would make "comment the block out to try without WiFi" the
    only way to get there, which throws the port, the threshold *and retention*
    away — that last one is what made the bench unable to soak this at all. V21
    gates its own diagnostic on `recovery[enabled]` for the same reason.
    """
    if (collection := config.get(CONF_COLLECTION)) is None:
        return
    if not collection[CONF_ENABLED] or not collection[CONF_SERVE]:
        return

    if CONF_WIFI not in full_config:
        raise cv.Invalid(
            f"'{CONF_COLLECTION}' needs a 'wifi:' block. The collector pulls sealed "
            f"chunks off the card over the network, so without WiFi the server "
            f"comes up on an interface that never exists and nothing is ever "
            f"collected — while the card keeps filling. Add 'wifi:', or set "
            f"'{CONF_COLLECTION}.{CONF_SERVE}: false' to keep the chunk index, "
            f"retention and the '#gap' marker on a device whose card is collected "
            f"by hand.",
            path=[CONF_COLLECTION],
        )

    if (web_server := full_config.get(CONF_WEB_SERVER)) is None:
        return
    blocks = web_server if isinstance(web_server, list) else [web_server]
    for block in blocks:
        if block.get(CONF_PORT) == collection[CONF_PORT]:
            raise cv.Invalid(
                f"'{CONF_COLLECTION}.{CONF_PORT}' is {collection[CONF_PORT]}, which "
                f"'web_server' is already using. sd_logger serves chunks from its "
                f"own esp_http_server instance rather than through web_server, so "
                f"the two would race to bind one port at boot and the loser fails "
                f"with nothing in the config to explain it. Give one of them a "
                f"different port.",
                path=[CONF_COLLECTION, CONF_PORT],
            )


def _final_validate(config):
    """V12: a tapped port must have `log_tap: true` on the can_gateway side.

    A config error rather than a warning on purpose. Without it the port never
    allocates a ring, sd_logger drains nothing, and the failure looks exactly
    like a bus with no traffic — a very expensive thing to debug on a bench.

    Also declares this component's need for VFS directory support — see the
    note on `require_vfs_dir` below. It has to happen in a validator rather
    than in `to_code()`: the esp32 component reads the flag inside its own
    `to_code()`, and neither coroutine is priority-ordered against the other,
    so setting it from ours lands too late to matter. openthread and zigbee
    call their equivalents from validators for the same reason.
    """
    # ESPHome disables CONFIG_VFS_SUPPORT_DIR by default (esp32/__init__.py: it
    # saves ~0.5 KB and core ESPHome never enumerates directories). With it off,
    # opendir() is compiled out and returns nullptr *without setting errno*, so
    # scan_next_seq_() silently restarted at sequence 0 on every boot and the
    # writer then walked the whole existing file set one fopen at a time,
    # dropping every record produced during the walk. Measured on the bench:
    # 201 693 records lost across a 397-file walk, and it grows every boot.
    require_vfs_dir()

    full_config = fv.full_config.get()

    # The `sd_logger.log` action accepts a declared label in place of a bare
    # number. Validation of every component finishes before any to_code runs, so
    # stashing the map here is what makes it available when the action is
    # generated — the action's own schema cannot see the logger's config.
    labels: dict[str, int] = {
        entry[CONF_LABEL]: entry[CONF_TAG] for entry in config.get(CONF_SOURCES) or []
    }
    labels.update(
        {
            entry[CONF_LABEL]: entry[CONF_SOURCE]
            for entry in config.get(CONF_CAN_PORTS) or []
        }
    )
    CORE.data[KEY_SOURCE_LABELS] = labels

    _validate_esphome_logs(config, full_config)
    _validate_collection(config, full_config)

    if (entries := config.get(CONF_CAN_PORTS)) is None:
        return config
    for index, entry in enumerate(entries):
        port = _find_port_config(full_config, entry[CONF_PORT])
        if port is None:
            # The id resolved at schema time, so a miss here means an unexpected
            # config shape; leave it to can_gateway's own validation to report.
            continue
        if not port.get(CONF_LOG_TAP):
            raise cv.Invalid(
                f"can_gateway port '{entry[CONF_PORT]}' is listed in "
                f"'{CONF_CAN_PORTS}' but does not have '{CONF_LOG_TAP}: true'. "
                f"Without it the port never allocates a tap ring and nothing is "
                f"logged from that segment.",
                path=[CONF_CAN_PORTS, index, CONF_PORT],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    cg.add_define("USE_SD_LOGGER")
    # IDF components the SD-over-SPI + FAT + ADC path needs; not all are pulled
    # into an ESPHome build by default, so name them explicitly (idempotent).
    for component in (
        "fatfs",
        "sdmmc",
        "esp_driver_sdspi",
        "esp_driver_spi",
        "esp_adc",
    ):
        include_builtin_idf_component(component)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(
        var.set_spi_pins(
            config[CONF_CLK_PIN],
            config[CONF_MOSI_PIN],
            config[CONF_MISO_PIN],
            config[CONF_CS_PIN],
        )
    )
    cg.add(var.set_clock_hz(config[CONF_CLOCK]))
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
    cg.add(var.set_buffer_depth(config[CONF_BUFFER_DEPTH]))
    cg.add(var.set_sync_interval(config[CONF_SYNC_INTERVAL].total_milliseconds))
    cg.add(var.set_max_file_size(config[CONF_MAX_FILE_SIZE]))
    cg.add(var.set_format_if_mount_failed(config[CONF_FORMAT_IF_MOUNT_FAILED]))
    if (origin := config.get(CONF_ORIGIN)) is not None:
        cg.add(var.set_origin(origin))
    if (time_id := config.get(CONF_TIME_ID)) is not None:
        clock = await cg.get_variable(time_id)
        cg.add(
            clock.add_on_time_sync_callback(
                cg.RawExpression(
                    f"[]() {{ {var}->set_utc_anchor("
                    f"static_cast<uint64_t>({clock}->utcnow().timestamp) * 1000000ULL, "
                    f"static_cast<uint64_t>(esp_timer_get_time())); }}"
                )
            )
        )

    # M6 rotation and collection. Both keys are read by the firmware as of the
    # Phase A wiring: `max_file_seconds` is the second rotation bound (whichever
    # fires first), and `collection:` configures the chunk index and retention.
    # The server on `port` is Phase B and is deliberately not built yet — the
    # port is passed anyway so dump_config reports what the config asked for.
    #
    # test_m6_keys_are_wired_into_codegen_exactly_when_the_setters_exist in
    # tests/sd_logger/test_schema.py holds these calls against the setters
    # declared in sd_logger.h: a firmware that accepts `retention_percent: 80`
    # and never arms retention looks exactly like one that honours it, so
    # nothing else would notice the two sides drifting apart.
    if (seconds := config.get(CONF_MAX_FILE_SECONDS)) is not None:
        cg.add(var.set_max_file_seconds(seconds))
    if (collection := config.get(CONF_COLLECTION)) is not None:
        # Gates the retention pass — the card-fill poll and the unlink — so a
        # config without a `collection:` block does not carry code that can never
        # run. The chunk index itself is unconditional: sealing is what the
        # index is for, and it is the same bookkeeping either way.
        cg.add_define("USE_SD_LOGGER_COLLECTION")
        # A sizing define rather than a setter: the index is a static array in
        # collection_policy.h, so its capacity has to be known at compile time —
        # the same shape as SD_LOG_MAX_SOURCES below. Emitted from here and never
        # from esphome core's defines.h, because this component ships as an
        # external component and must build without core edits.
        cg.add_define("SD_LOG_MAX_CHUNKS", collection[CONF_MAX_CHUNKS])
        if collection[CONF_SERVE]:
            # M6 Phase B: the private esp_http_server that serves SEALED chunks
            # and takes the confirm (design §5). Gated by its own define so
            # `serve: false` links none of it — not the handlers, not the index
            # mutex, not the confirm queue — which is the shape the bench soaks
            # rotation and retention in, deliberately without WiFi (§9.1).
            cg.add_define("USE_SD_LOGGER_COLLECTION_SERVER")
            # `esp_http_server` is not in esphome's default IDF exclusion list
            # today, so this is a no-op — and it is here so it stays a no-op if
            # that list ever grows. Naming what we use is the whole reason the
            # helper exists, and an external component cannot patch core to fix
            # it after the fact.
            include_builtin_idf_component("esp_http_server")
        cg.add(
            var.set_collection(
                collection[CONF_ENABLED],
                # Generated, not dropped: `serve:` decides whether V22 demands
                # `wifi:`, so a firmware that never reported it would leave the
                # two sides of that decision impossible to compare on a bench.
                collection[CONF_SERVE],
                collection[CONF_PORT],
                collection[CONF_RETENTION_PERCENT],
            )
        )

    if CONF_CARD_POWER_PIN in config:
        cg.add(var.set_card_power_pin(config[CONF_CARD_POWER_PIN]))
    recovery = config[CONF_RECOVERY]
    cg.add(
        var.set_recovery(
            recovery[CONF_ENABLED],
            recovery[CONF_INITIAL_DELAY].total_milliseconds,
            recovery[CONF_MAX_DELAY].total_milliseconds,
            recovery[CONF_MAX_ATTEMPTS],
            recovery[CONF_POWER_CYCLE],
        )
    )
    cg.add(
        var.set_recovery_reset(
            recovery[CONF_IN_BAND_RESET],
            recovery[CONF_BUSY_TIMEOUT].total_milliseconds,
            recovery[CONF_MAX_BUSY_TIMEOUT].total_milliseconds,
        )
    )
    if (vcc := config.get(CONF_VCC_MONITOR)) is not None:
        cg.add(
            var.set_vcc_monitor(
                vcc[CONF_ADC_PIN],
                vcc[CONF_THRESHOLD],
                vcc[CONF_DIVIDER],
                vcc[CONF_ADC_FULL_SCALE],
            )
        )
    if (stats := config.get(CONF_STATISTICS)) is not None:
        cg.add(var.set_stats_log_interval(stats[CONF_LOG_INTERVAL].total_milliseconds))

    # S2: drain each tapped can_gateway port's ring from the writer task. The
    # define is what compiles the can_gateway include and the drain in at all,
    # so a logger with no taps costs nothing and still builds without the
    # gateway component present.
    can_ports = config.get(CONF_CAN_PORTS) or []
    if can_ports:
        cg.add_define("USE_SD_LOGGER_CAN_TAP")
        cg.add_define("SD_LOGGER_CAN_TAP_MAX", len(can_ports))
        for entry in can_ports:
            port = await cg.get_variable(entry[CONF_PORT])
            cg.add(var.add_can_tap(port, entry[CONF_SOURCE], entry[CONF_LABEL]))

    # Sources declared but not backed by a native tap: fed by the sd_logger.log
    # action today, by the linbus tap (S3) later. add_can_tap() already declared
    # the tapped ones, so this is the rest.
    sources = config.get(CONF_SOURCES) or []
    for entry in sources:
        cg.add(
            var.add_source(
                entry[CONF_TAG],
                cg.RawExpression(f"'{KIND_LETTERS[entry[CONF_KIND]]}'"),
                entry[CONF_LABEL],
            )
        )
    # The table is fixed-size in log_format.h so it costs nothing on the hot
    # lookup path; raise the ceiling only when a config actually needs it. An
    # entry that does not fit still logs, as `U,<decimal>` — but the file would
    # stop naming that bus, so this is sized rather than left to degrade.
    if (declared := len(can_ports) + len(sources)) > 16:
        cg.add_define("SD_LOG_MAX_SOURCES", declared)

    # S4: ESPHome's own log as `X` lines in the same file.
    if (logs := config.get(CONF_ESPHOME_LOGS)) is not None:
        # THE call that makes capture work at all: it is what emits
        # USE_LOG_LISTENERS and sizes ESPHOME_LOG_MAX_LISTENERS. Without it
        # add_log_callback() is an empty function and nothing is ever captured,
        # with no error anywhere (spec §4a). V15 guarantees `logger:` exists.
        request_log_listener()
        cg.add(
            var.set_esphome_logs(
                LOG_LEVEL_SEVERITY.index(logs[CONF_LEVEL]), logs[CONF_BUFFER_DEPTH]
            )
        )


# ---------------------------------------------------------------------------
# Action: sd_logger.log — the generic producer (source S1). Any automation can
# push a record; this is also the seam a linbus on_frame lambda uses today.
# ---------------------------------------------------------------------------

LOG_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(SdLogger),
        cv.Optional(CONF_CAN_ID, default=0): cv.templatable(
            cv.int_range(min=0, max=0x1FFFFFFF)
        ),
        # A declared label or the bare tag. The tag derivation does not change —
        # 0 stays the action's own default (spec §4a).
        cv.Optional(CONF_SOURCE, default=0): cv.Any(
            cv.int_range(min=0, max=255), _validate_label
        ),
        cv.Required(CONF_DATA): cv.templatable(
            cv.Any(cv.All(cv.ensure_list(cv.hex_uint8_t), cv.Length(max=8)), cv.string)
        ),
    }
)


def _resolve_action_source(value) -> int:
    """Turn the action's `source:` into a tag, whichever spelling was used.

    Resolved here rather than in the schema because the map lives in the
    sd_logger config, which an action's own schema cannot reach.
    """
    if isinstance(value, int):
        return value
    labels: dict[str, int] = CORE.data.get(KEY_SOURCE_LABELS, {})
    if value not in labels:
        known = ", ".join(sorted(labels)) or "none declared"
        raise cv.Invalid(
            f"'{CONF_SOURCE}: {value}' does not name a declared source. Declare "
            f"it under sd_logger '{CONF_SOURCES}:' (or '{CONF_CAN_PORTS}:'), or "
            f"use the numeric tag directly. Declared labels: {known}."
        )
    return labels[value]


@automation.register_action(
    "sd_logger.log", LogAction, LOG_ACTION_SCHEMA, synchronous=True
)
async def log_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_source(_resolve_action_source(config[CONF_SOURCE])))

    # TEMPLATABLE_VALUE: always wrap through cg.templatable (constants included),
    # then hand the wrapper to the single generated setter.
    can_id = await cg.templatable(config[CONF_CAN_ID], args, cg.uint32)
    cg.add(var.set_can_id(can_id))

    data = config[CONF_DATA]
    if isinstance(data, str):
        data = list(data.encode())
    if isinstance(data, core.Lambda):
        template_ = await cg.templatable(data, args, cg.std_vector.template(cg.uint8))
        cg.add(var.set_data_template(template_))
    else:
        cg.add(var.set_data_static(data))
    return var
