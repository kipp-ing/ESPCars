# sd_logger M6 Phase B — the wire contract the firmware must satisfy

Extracted 2026-07-28 by reading code, not by paraphrasing the design.

**Why this document exists.** The host half of Phase B is built and green:
`script/sdlog_collect.py` (940 lines) and `tests/sd_logger/test_collect.py`
(1517 lines, **66 tests, all passing**). `test_collect.py` contains a
`FakeDevice`/`_Handler` pair that implements all four endpoints, and its own
docstring calls it what it is:

> `class _Handler(BaseHTTPRequestHandler):`
> `    """The §5b route table. Deliberately literal — it is the contract."""`

If the firmware deviates from what follows, those 66 tests stay green while
proving nothing. That is the failure mode this document is written against.

**Authority order used here** (highest first):
1. `tests/sd_logger/test_collect.py` — `_Handler` (L145–276), `FakeDevice`
   (L290–426), and the six `test_the_fake_device_*` cases (L478–596) that pin the
   fake's own behaviour.
2. `script/sdlog_collect.py` — what the client sends, what it retries, what it
   treats as fatal.
3. `docs/sdlog-collection-design.md` §5b/§6/§7/§8.
4. `components/sd_logger/collection_policy.h` — the class the server is built on.

Where 1 and 3 disagree, §11 ("Open questions") says so rather than picking a
winner silently.

**Freshness warning.** A concurrent session edited `collection_policy.h`,
`sd_logger.{h,cpp}`, `__init__.py` and the design doc *while this document was
being written* (V26 `collection: max_chunks:`, `SD_LOG_MAX_CHUNKS` 64 → 256,
`ChunkEntry::bytes` 64-bit → 32-bit saturating, an `index_refused=` counter).
Everything below has been re-verified against the working tree as of that edit.
**`tests/sd_logger/test_collect.py` and `script/sdlog_collect.py` — the two
sources of authority for the wire contract — were not touched**, and the 66 tests
are green. Re-check `git diff` before implementing if more time has passed.

**Freshness warning, 2026-07-29 — two statements below are now false.** The
firmware was built and then flashed (`18c6d02`), and this document was written
before either:

- **§9.4 "No primitive exists"** for ring-fill backpressure. It exists:
  `CollectionServer::backpressure_()` sleeps `SD_LOG_BACKPRESSURE_SLEEP_MS` (5 ms)
  between blocks above `SD_LOG_BACKPRESSURE_PERCENT` (50 %) ring fill, at most
  `SD_LOG_BACKPRESSURE_MAX_SLEEPS` (8) times — a 40 ms per-block ceiling — and
  returns at once if the card unmounts. The §6 acceptance criterion it serves is
  still unmeasured.
- **§11.8 "Nothing here has been on hardware"**. It has, on Mr. Orange
  2026-07-29: index, status, a 32 MB chunk at 415 KB/s, `sdlog.py check` clean on
  the result, 409/416/206, a verified 206 body offset, and confirm idempotency
  across the rename. See M6 load run §2 (2026-07-29, git history) for the table and — more
  important — for what that run still does **not** cover, which is everything
  under load.

Everything else stands. The rest of §9.4 and §11.8 (the reasoning, the owed
measurements) is still accurate; only the "does not exist" and "never run"
framing has expired.

**Constants**, from `sdlog_collect.py` L162–163, re-exported by the test suite as
`HEAD` and `SEAM`:

```python
HEAD_WINDOW = 256
SEAM_WINDOW = 256
BLOCK       = 64 * 1024      # the client's socket read size
DEFAULT_TIMEOUT = 10.0       # seconds, per request
DEFAULT_RETRIES = 4          # attempts per chunk per run, NOT retries after the first
```

---

## 1. Transport-level facts

These were captured by pointing the real client at a socket sniffer, not guessed.
Every request the collector can emit, verbatim:

```
GET /sdlog/index HTTP/1.1
Accept-Encoding: identity
Host: <host>:<port>
User-Agent: Python-urllib/3.13
Connection: close

GET /sdlog/f/L0000431.LOG HTTP/1.1          <- first fetch: NO Range header
Accept-Encoding: identity
Host: <host>:<port>
User-Agent: Python-urllib/3.13
Connection: close

GET /sdlog/f/L0000431.LOG HTTP/1.1          <- identity head window
...
Range: bytes=0-255
Connection: close

GET /sdlog/f/L0000431.LOG HTTP/1.1          <- resume, always open-ended
...
Range: bytes=144-
Connection: close

POST /sdlog/done/L0000431.LOG HTTP/1.1
Accept-Encoding: identity
Content-Type: application/x-www-form-urlencoded
Content-Length: 0
Host: <host>:<port>
User-Agent: Python-urllib/3.13
Connection: close
```

Consequences the firmware must honour:

- **HTTP/1.1, `Connection: close` on every request.** There is no keep-alive to
  optimise for; each request is one TCP connection, torn down by the client.
  `max_open_sockets` can be small (2–3) — but see §10.5 on the abandoned socket.
- **The POST carries a zero-length body with a `Content-Type`.** The handler must
  not choke on `Content-Type: application/x-www-form-urlencoded` and must not
  require a body. The fake reads and discards it:
  `length = int(self.headers.get("Content-Length") or 0); if length: self.rfile.read(length)`.
- **No query strings, ever.** No `Authorization`, no cookies, no `Accept` that
  matters. `Accept-Encoding: identity` — never compress a response.
- **Never more than one request in flight.** `Collector` is single-threaded and
  strictly sequential (`collect()` L772–805).
- **Every request has a 10 s socket timeout** (`DEFAULT_TIMEOUT`). A gap of more
  than 10 s between socket writes is indistinguishable from a cut connection.
  This bounds §6 backpressure — see §10.4.
- **Max URI length is trivial**: `/sdlog/f/L0000431.LOG` is 21 characters.
  `CONFIG_HTTPD_MAX_URI_LEN` (default 512) is not a constraint.

---

## 2. `GET /sdlog/index`

### 2.1 Path and method

Exact string match on `/sdlog/index`. The fake dispatches on
`if self.path == "/sdlog/index"` — no trailing slash, no query, no
case-insensitivity. GET only.

### 2.2 Success response

Transcribed from `_Handler._serve_index` / `_Handler._json` (L158–196):

```
200 OK
Content-Type: application/json
Content-Length: <len(body)>

{"device": "mr-orange", "chunks": [ ... ]}
```

Each entry of `chunks`, transcribed from `FakeDevice.index_entries()` (L358–372):

| key | JSON type | source in `collection_policy.h` | required? |
|---|---|---|---|
| `name` | string | `format_chunk_name(out, seq, ChunkState::SEALED)` → `L0000431.LOG` | **yes** — an entry without a usable `name` is dropped and reported |
| `seq` | number (uint32) | `ChunkEntry::seq` | no — `null`/absent falls back to the digits in `name` |
| `bytes` | number (uint32) | `ChunkEntry::bytes` — **32-bit and saturating** since the 2026-07-28 edit; `chunk_bytes()` clamps at `0xFFFFFFFF` rather than truncating | no — but see §2.6, omitting it degrades three behaviours |
| `first_t_us` | number (uint64) | **does not exist** — see §11.1 | no — only affects `sdlog_collect.py index` display |
| `last_t_us` | number (uint64) | **does not exist** — see §11.1 | no |

Unknown extra keys are ignored by the client. The parser is `_chunk_of()`
(L818–836).

One consequence of the 32-bit `bytes`: a *saturated* entry (`4294967295`) can only
come from the boot scan `stat()`ing a foreign 4 GiB+ file that merely happens to
be named `L#######.LOG`. Emitting it verbatim is correct — the client's
`_already_have` length check will refuse to confirm anything under that name, and
`_prove_local`'s `local > chunk.size` guard stays sane. Do not special-case it;
over-reporting a stranger's size is the deliberate direction (see the
`ChunkEntry` docstring).

### 2.3 What is listed

**SEALED only.** From `collection_policy.h`:

```cpp
/// Listed by `GET /sdlog/index` and readable by `GET /sdlog/f/<name>`: SEALED only. The OPEN file
/// is not servable at any point and CONFIRMED chunks are not listed again.
bool is_servable(uint32_t seq) const {
  const ChunkEntry *entry = this->find(seq);
  return entry != nullptr && entry->state == ChunkState::SEALED;
}
```

The handler walks `at(0)…at(count()-1)` and emits an entry for every
`ChunkState::SEALED`. `at()` returns entries in **insertion order, not seq
order** — that is fine and deliberate:

> `"""Insertion order, deliberately not sorted: the collector owns the`
> `oldest-first ordering, so the device must not do it for free."""`
> — `FakeDevice.sealed()`, L350–352

The client re-sorts: `chunks.sort(key=lambda chunk: (chunk.seq, chunk.name))`
(`index()` L471). Pinned by `test_chunks_are_collected_oldest_first`, which feeds
the device seqs in the order 433, 431, 432 and asserts the collector confirms
431, 432, 433.

A name listed twice is collapsed to one chunk by the client
(`test_a_chunk_listed_twice_is_collected_once`) — but duplicate emission is a
firmware bug regardless; `CollectionPolicy::add()` already refuses a duplicate
seq.

### 2.4 The `device` key

Half the dedup key. `index()` L450–452:

```python
named = payload.get("device")
if named and not self._device_pinned:
    self.device = str(named)
```

Falsy or absent → the client falls back to the URL hostname
(`_name()` L406–407), which is **not stable** across DHCP leases and mDNS
spellings. The firmware must emit a stable name; `App.get_name()` is the obvious
source. This is a genuinely new requirement — nothing in Phase A carries a device
name (see §11.2).

### 2.5 Errors the client can distinguish

`_retryable()` (L175–178) is the client's whole decision function:

```python
_RETRYABLE_STATUS = frozenset({408, 429})
def _retryable(status: int) -> bool:
    return status >= 500 or status in _RETRYABLE_STATUS
```

| response | client behaviour | pinned by |
|---|---|---|
| 5xx / 408 / 429 | retried, up to `retries` attempts, exponential backoff | `test_a_busy_index_is_retried_not_abandoned` (`index_faults=[500,503]` → 3 index requests, run succeeds) |
| 5xx every time | `_Unreachable`, `<index>` failure, **exit 2** | `test_an_index_that_keeps_stumbling_is_reported_as_unreachable` |
| any other 4xx | **not retried** — one request, `_Unreachable`, exit 2 | `test_an_index_that_answers_4xx_is_not_retried` (404 → exactly 1 request) |
| valid JSON that is not an object | not retried, `"the index is not a JSON object but a list"`, exit 2 | `test_an_index_that_is_not_an_object_says_so` |
| invalid JSON / transport error | **retried** (`json.JSONDecodeError` is a `ValueError`, caught at L436) | — |
| `chunks` missing or not a list | silently treated as `[]` — a clean, empty run | `index()` L454–456 |

So **500 is the right answer for a transient failure** (out of heap, card busy,
card unmounted) and 503 is equally fine. A 404 on this route means "wrong
firmware / wrong port" to the operator, so never use 4xx for a transient
condition.

### 2.6 Cost of omitting `bytes`

`chunk.size == 0` is the "unknown" sentinel (`_int_or(entry.get("bytes"), 0)`).
Three behaviours degrade, none unsafely:

- `_already_have()` L551: the cheap length pre-check `local != chunk.size` is
  skipped, so a wrong-length archived copy is only caught by the head window.
- `_prove_local()` L529: the `local > chunk.size` guard is skipped, so an
  over-long partial is only caught by the 416 (§3.6).
- `_fetch()` L602: the "a previous run got every byte and died before the rename"
  shortcut needs `chunk.size`; without it the client re-reads the chunk.
  Pinned by `test_a_complete_partial_is_renamed_without_refetching_the_chunk`,
  which asserts `outcome.bytes_wire == HEAD` — i.e. only 256 bytes crossed the
  air. **Emit `bytes`.** `ChunkEntry::bytes` already holds the exact sealed size.

---

## 3. `GET /sdlog/f/<name>`

The load-bearing endpoint. Everything about resume safety lives here.

### 3.1 Path, method, and `<name>` parsing

Prefix match: `self.path.startswith("/sdlog/f/")`, then
`unquote(self.path[len("/sdlog/f/"):])`.

The client builds the path as `f"/sdlog/f/{quote(chunk.name)}"` with
`urllib.parse.quote` at its default `safe='/'`. For real names
(`L0000431.LOG`) quoting is the identity function, so **percent-decoding is
optional in practice** — but the firmware should still decode, and must bound the
decode buffer before decoding.

Validation, in order:

1. Reject anything whose decoded length is not 12 characters. `SD_LOG_NAME_LEN`
   is 13 including the NUL (`log_format.h` L596).
2. `parse_chunk_name(name, &seq, &state)` — `collection_policy.h` L151. It accepts
   **exactly** `L#######.LOG` (→ SEALED) and `L#######.UPL` (→ CONFIRMED),
   uppercase, NUL-terminated at index 12, and nothing else. It is its own bounds
   check ("a short name hits its NUL here, which is not a digit"). It rejects
   `.CSV` deliberately — an M1-era file is not a chunk.
3. Anything `parse_chunk_name()` rejects → **404**. This covers `..`, `/`, empty,
   NUL bytes, and traversal attempts for free — the parser only accepts a
   fixed 8.3 shape, so no untrusted string ever reaches `open()`.

Note the state `parse_chunk_name()` returns is the state the *name* implies. It
is **not** authoritative: `find(seq)` on the index is. A name alone can never say
OPEN, because the open file is also `.LOG`.

### 3.2 The state → status table

The fake's order of checks (`_serve_chunk` L198–207) is normative — OPEN is
tested **before** existence:

```python
# The open file is never servable — it is the one file the writer holds.
if name == dev.open_file:
    self._json(409, {"error": "open"})
    return
data = dev.chunks.get(name)
if data is None or name in dev.confirmed:
    self._json(404, {"error": "no such chunk"})
    return
```

| index state for `seq` | status | pinned by |
|---|---|---|
| `ChunkState::OPEN` | **409** | `test_the_fake_device_never_lists_the_open_file_and_409s_it`, `test_a_409_on_the_open_file_does_not_stop_the_run` |
| `ChunkState::SEALED` | 200 or 206 | the happy path |
| `ChunkState::CONFIRMED` | **404** | `test_the_fake_device_confirms_idempotently_and_can_refuse` (after a confirm, GET → 404) |
| not tracked (never existed, discarded by retention, index was full) | **404** | — |
| name unparseable | **404** | — |
| SEALED but `open()`/`read()` on the card fails | **500** (retryable) — *chosen, not modelled by the fake* | see §11.6 |

409 must be reachable: a stale index can name the open file, and the client is
required to survive it. `test_a_409_on_the_open_file_does_not_stop_the_run`
asserts the run collects 431 and 433, reports 432 as failed, and — the point —
`device.confirms(open_name) == []`. **Confirming the open file would make the
live file deletable.**

### 3.3 Success response, no Range

```
200 OK
Content-Type: text/csv
Content-Length: <full file size>

<the whole file>
```

`Content-Type` is `text/csv` in the fake and **nothing in the client reads it**.
Any sane value works; use `text/csv` for consistency with the fake.

### 3.4 Success response, Range honoured

```
206 Partial Content
Content-Type: text/csv
Content-Range: bytes <start>-<end>/<total>
Content-Length: <end - start + 1>          <- the length of THIS body, not the file
```

From `_serve_chunk` L227–235:

```python
body = data[start : end + 1]
self.send_response(206 if honour else 200)
self.send_header("Content-Type", "text/csv")
if honour:
    self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
# Always the honest length for what is being sent.
self.send_header("Content-Length", str(len(body)))
```

**The single easiest way to break every resume** is to set `Content-Length` to
the file size on a 206. The client computes
`if promised is not None and got < promised: raise _Interrupted` (`_download`
L718), so an inflated `Content-Length` makes every successful resume look like a
cut connection, forever.

`Content-Range` is asserted by `test_the_fake_device_serves_a_range_as_206_and_cuts_on_demand`:

```python
assert tail.headers["Content-Range"] == f"bytes {len(data) - 10}-{len(data) - 1}/{len(data)}"
```

…but **the client never reads it.** Grep `sdlog_collect.py`: only
`response.status` and `Content-Length` are consulted. Emit it anyway (RFC 7233,
and the fake pins the spelling), but no client behaviour depends on it.

**`Accept-Ranges` is never sent by the fake and never read by the client.** It is
optional. Do not spend heap on it.

### 3.5 The two Range forms the client sends

Only these two ever occur:

- `Range: bytes=0-<count-1>` — the identity head window. `count` is
  `min(local_size, HEAD_WINDOW)` (`_prove_local` L531), so it is **usually 255
  but can be smaller** when the local partial is shorter than 256 bytes. Bounded
  ranges of arbitrary small size must work.
- `Range: bytes=<start>-` — open-ended, the resume. `start = offset - min(offset, 256)`
  (`_download` L664–668).

The fake's grammar (L213): `re.fullmatch(r"bytes=(\d+)-(\d*)", rng.strip())`.
Single range only. No suffix ranges (`bytes=-500`), no multi-range, no units
other than `bytes`. **The firmware need only implement these two forms.**

End-of-range clamping is required, and 416 must **not** fire for it:

```python
if match.group(2):
    end = min(end, int(match.group(2)))     # end clamps to EOF; no 416
if start >= len(data):
    <416>
```

This matters: `_prove_local()` asks for `bytes=0-255` against a file that may be
shorter than 256 bytes, and relies on getting a short 206 back so it can conclude
`"the device holds only {len(head)} B under this name"` → `_Mismatch`. Answering
416 there would turn a correct mismatch verdict into a fatal 4xx for the chunk.

### 3.6 416

```
416 Range Not Satisfiable
Content-Range: bytes */<total>
Content-Length: 0
```

Triggered by `start >= len(data)` only. The client special-cases it
(`_fetch` L613–622):

```python
if err.code == 416:
    # We asked from beyond the end of what the device holds, so
    # what we hold is not a prefix of it.
    _unlink(partial)
    transfer.restarted = True
    reason = "discarded the partial: it is longer than the chunk offered"
    continue
```

It discards the local partial and **retries within the same run**. Pinned by
`test_a_partial_longer_than_the_chunk_offered_is_dropped`, which sets
`report_bytes = False` so the 416 is the *only* signal available. Implement it.

### 3.7 400 on a malformed Range

The fake answers `400 {"error": "bad range ..."}` for a Range it cannot parse
(L214–216). **No test exercises this path, and no client code path can reach
it** — the client only ever emits the two forms above. The firmware is therefore
free here, and 400 is the *worse* choice: the client treats any non-416 4xx as
fatal for that chunk (`_fetch` L623–627). Recommended: on an unparseable Range,
**ignore it and serve 200 with the whole file** — a behaviour the client
explicitly tolerates (§7). Either way, mark it untested.

### 3.8 The cut connection — what a car driving away looks like

There is no status code for it. The fake writes fewer bytes than the promised
`Content-Length`, flushes, and sets `close_connection = True` (L238–246). The
client detects it purely by counting:

```python
if promised is not None and got < promised:
    # No status code says this happened. The car drove out of range.
    raise _Interrupted(f"body ended after {start + got} of {start + promised} bytes")
```

The firmware does not have to *produce* this deliberately — but it must make it
detectable, which is exactly what an honest `Content-Length` does, and it must
survive its own send failing mid-body (§10.5).

---

## 4. `POST /sdlog/done/<name>`

### 4.1 Path, method, body

Prefix match `/sdlog/done/`, then `unquote(...)`. POST only — a GET on this path
falls through the fake's `do_GET` to `404 {"error": "no such route"}`.

The request carries `Content-Length: 0` and
`Content-Type: application/x-www-form-urlencoded`. Read and discard.

`<name>` is parsed and validated exactly as in §3.1 — `parse_chunk_name()`, then
`find(seq)`.

### 4.2 Success response

```
200 OK
Content-Type: application/json
Content-Length: <n>

{"confirmed": "L0000431.LOG"}
```

**The status must be exactly 200.** The client's confirm loop (`_confirm`
L727–749) is not "2xx means yes":

```python
if response.status == 200:
    return None
if _retryable(response.status):
    reason = f"HTTP {response.status}"
    continue
return f"confirm: HTTP {response.status}"
```

A `204 No Content` — the tempting choice for an empty ack — is reported as
`confirm: HTTP 204` and the chunk is left uncollected forever. The response body
is never read; only the status matters.

### 4.3 The state → status table

Fake order of checks (`do_POST` L255–276): route, then `on_confirm` hook, then
OPEN, then the injected status, then existence.

| index state for `seq` | `CollectionPolicy::confirm(seq)` returns | status | pinned by |
|---|---|---|---|
| `ChunkState::OPEN` | `false` | **409** | fake L264–266; `test_confirm_reports_true_only_when_the_device_said_so` uses 409 and asserts `confirm(...) is False` |
| `ChunkState::SEALED` | `true` → becomes CONFIRMED, writer renames `.LOG`→`.UPL` | **200** | the happy path |
| `ChunkState::CONFIRMED` (already) | `true`, no-op | **200** | `assert confirm(name).status == 200  # idempotent` |
| not tracked | `false` | **404** — *chosen, not modelled* | see §11.5 |
| name unparseable | — | **404** | fake: `{"error": "no such chunk"}` |

The idempotency row is the trap. After the first confirm, the writer renames the
file, so `L0000431.LOG` **no longer exists on the card**. A handler that answers
from `stat()` would 404 the retry — and the client's retry is exactly the case
where the first 200 was lost on the wire. **Answer from the index, by seq, never
from the filesystem.** `CollectionPolicy::confirm()` already encodes this:

```cpp
bool confirm(uint32_t seq) {
  ChunkEntry *entry = this->find_mut_(seq);
  if (entry == nullptr) return false;
  if (entry->state == ChunkState::CONFIRMED) return true;   // idempotent
  if (entry->state != ChunkState::SEALED) return false;      // OPEN
  entry->state = ChunkState::CONFIRMED;
  return true;
}
```

Note also that `L0000431.UPL` parses to seq 431 / CONFIRMED, so a client that
somehow asked for the `.UPL` spelling resolves to the same entry and gets 200.

### 4.4 Errors

| response | client behaviour | pinned by |
|---|---|---|
| 5xx / 408 / 429 | retried up to `retries` times; the confirm is idempotent so duplicates are free | `test_a_confirm_that_stumbles_is_retried_and_a_refusal_is_not` — `confirm_status=500` → exactly 3 POSTs |
| any other 4xx | **one attempt only**, chunk reported failed, retried next run | same test — `confirm_status=403` → exactly 1 more POST, `"403" in reason` |
| transport error | retried | `_confirm` L746–748 |

A failed confirm is never treated as sent. `test_a_failed_confirm_leaves_the_chunk_for_the_next_run`
asserts the archive keeps the bytes, the device still lists the chunk, and the
next run confirms it.

### 4.5 A 200 whose rename does not stick is legal

`FakeDevice.confirm_sticks = False` models "the ECU answered 200 and then lost
power before the rename". The client handles it by re-collecting the chunk next
run, paying one 256-byte identity window and no chunk bytes
(`test_a_re_served_chunk_is_neither_stored_nor_fetched_twice`: `second.bytes_fetched == 0`,
`second.bytes_wire == HEAD`, and `len(device.confirms(name)) == 2`).

This is what licenses an **asynchronous** confirm (handler validates, posts an
intent to the writer, answers 200 before the rename is durable) — see §9.2. It
costs one re-serve on an unlucky power cut; it never loses data.

---

## 5. `GET /sdlog/status`

**The client never calls this endpoint.** Grep confirms: `sdlog_collect.py`
contains no `/sdlog/status`. It exists in the fake and is unconstrained by any
collector behaviour, so this is the one route where the firmware is free.

The fake's payload (`FakeDevice.status_payload()` L374–382):

```python
{
    "device": self.device,          # str
    "sealed": len(self.sealed()),   # int  — SEALED chunks currently listed
    "confirmed": len(self.confirmed),  # int
    "card_percent": 41,             # int  — card fill
    "discarded_chunks": 0,          # int
    "discarded_bytes": 0,           # int
}
```

Response shape: `200`, `Content-Type: application/json`, `Content-Length` set.

Design §5b asks for one field the fake does not model — **"oldest un-collected
seq"** — and `collection_policy.h` provides exactly that accessor:

```cpp
/// Oldest SEALED seq — the "oldest un-collected" of `GET /sdlog/status`.
bool oldest_uncollected(uint32_t *seq_out) const;
```

Recommended payload: the fake's six keys, plus `"oldest_uncollected": <seq|null>`
(design §5b), plus `"index_refused": <n>` (see §11.5 — without it, an index that
overflowed is indistinguishable from a device with nothing to give).

Sources: `count()`+`at()` folds for `sealed`/`confirmed`, `discarded_chunks()` /
`discarded_bytes()` for the totals (`discarded_bytes()` returns `uint64_t` on
purpose — it sums across a whole run, unlike the now-32-bit per-entry
`ChunkEntry::bytes`), `SdLogger::get_index_refused()` for the refusals, and a
writer-published card fill for `card_percent` (**not** a direct call to
`card_fill_percent_()` from the httpd task — that is an SD access; see §9.4).

---

## 6. Idempotency and ordering

**The one rule the whole design rests on** (design §3, restated in both tool
docstrings and in `collection_policy.h` L20):

```
serve bytes  ->  puller verifies locally  ->  puller confirms  ->  device renames to .UPL
```

Device-side obligations that follow:

1. **Never rename before the confirm arrives.** A crash inside the window
   re-serves the chunk, which is correct and cheap. The inverse order loses data
   permanently.
2. **`POST /sdlog/done` is idempotent and must stay 200 forever** for a chunk that
   is tracked and not OPEN (§4.3).
3. **`GET /sdlog/f/<name>` is idempotent and side-effect-free** except for the
   serving flag. Serving a chunk twice costs one retry; the client's dedup on
   `(device, seq)` handles it.
4. **Index → GET consistency.** A chunk the index listed must, at the moment of
   the GET, either serve or produce a clean 4xx. Never 200 stale or different
   bytes under a listed name: the client's resume is a byte offset into a file it
   assumes is unchanged. This is why `CollectionPolicy::seal()` refuses a second
   seal —

   > `/// OPEN -> SEALED, recording the final size. `false` for an unknown seq or a chunk that is not`
   > `/// OPEN — a second seal must not rewrite the size of a file the collector may already be reading,`
   > `/// because that number is the `Content-Length` it was given.`

5. **Ordering is the client's job, not the device's.** Emit the index in whatever
   order `at()` walks. Do not sort.
6. **A confirm may legally arrive while `serving` is still set.** The policy says
   so explicitly:

   > `/// A transfer is starting. `false` for an unknown seq and for the OPEN chunk, which is never`
   > `/// served in the first place. A CONFIRMED chunk can be marked: the fixed order is`
   > `/// serve -> confirm -> rename, so the confirm lands while the handler is still finishing.`
   > `///`
   > `/// The serving flag is deliberately left alone: the flag, not the state, is what guards deletion,`
   > `/// and only the handler's exit path knows the transfer is over.`

---

## 7. May the server ignore `Range`? — the exact answer

**Yes, but only in one specific way, and the rule is keyed on the status code,
not on the headers.**

`_download` L672–682:

```python
mode = "ab"
if offset and response.status != 206:
    # The device ignored the Range and is resending the whole chunk.
    # Splicing that onto the partial would corrupt it silently, so the
    # partial is abandoned instead — by the truncating 'wb' open,
    # which still only happens once a first byte has arrived.
    mode = "wb"
    overlap = 0
    start = offset = 0
elif offset:
    transfer.resumed = True
```

Therefore:

- **A server that does not implement `Range` may answer `200` and send the
  complete file from byte 0.** The client notices the missing 206, discards its
  partial, and restarts. Cost: bandwidth. Correctness: intact.
  Pinned by `test_a_device_that_ignores_range_restarts_instead_of_splicing`,
  whose docstring is the requirement in one line: *"the missing 206 means
  restart"*.
- **`206` is a promise that the body begins at exactly the requested `start`
  offset.** A server that answers 206 and then sends from byte 0 corrupts the
  archive silently — the client appends, lands on the promised length, and the
  result parses clean. This is the one corruption no later check catches.
- **The `wb` truncation happens lazily**, only after the first body byte arrives
  (`handle` is opened inside the read loop, L706–708). So a device that is both
  range-blind *and* flaky cannot erase a good partial by refusing before it sends
  anything. The same test asserts `part.read_bytes() == data[:400]` after a
  range-blind, always-cut run.
- **The head window is not status-checked at all.** `_probe_head` ignores
  `response.status` and only enforces
  `owed = count if promised is None else min(promised, count)` (L514). A server
  that ignores the *end* of `bytes=0-255` and starts sending 4 MB costs one
  abandoned socket, nothing more.

**Recommendation:** implement `Range` properly. The design calls it "the retry
story", and without it a 4 MB chunk over car WiFi restarts from zero on every
drop. But a Phase B v1 that ships `Range`-less is *safe*, and that is a
deliberate property of the client, not an accident.

---

## 8. The client's fatal-vs-retry decision function, in one table

| where | condition | client does |
|---|---|---|
| index | 5xx, 408, 429 | retry (up to `retries`) |
| index | other 4xx | **fatal**, exit 2, one request |
| index | not a JSON object | **fatal**, exit 2, one request |
| index | bad JSON, transport error | retry |
| index entry | no usable `name`, not an object, unsafe name | skip that entry, report, run continues |
| chunk GET | 416 | drop partial, retry in-run |
| chunk GET | other 4xx (incl. 409, 404, 400) | **fatal for that chunk**, run continues, nothing confirmed |
| chunk GET | 5xx | retry in-run |
| chunk GET | short body vs `Content-Length` | retry in-run, partial kept |
| chunk GET | complete but fails `sdlog.py` parse | **partial deleted**, no retry, chunk left for a later run |
| confirm | 200 | success |
| confirm | 5xx, 408, 429 | retry |
| confirm | any other status incl. non-200 2xx | **fatal for that chunk**, run continues |

Exit statuses: `0` everything collected and confirmed, `1` at least one chunk was
not, `2` the device could not be reached at all.

---

## 9. What the firmware must do that the fake device does not model

The fake is a Python `ThreadingHTTPServer` with a dict of `bytes`. It has no SPI
bus, no FATFS, no tasks, no card. Everything below is real and unmodelled.

### 9.1 One SPI bus, one FATFS lock (design §2, §6)

The writer and the serving handler share SPI2 and the FATFS lock. The invariant
that makes this survivable is *file-level*, not lock-level:

> **The writer only ever appends to the open file. The collector only ever reads
> sealed files. They never touch the same file.**

`is_servable()` is the enforcement point — SEALED only, so the OPEN file's fd is
never touched by the handler.

**Owed:** read the body in 4–8 KB blocks straight into the socket (§5c), never
resident. Never hold the FATFS lock across a socket write. Bound how long a
single `read()` can stall the writer.

### 9.2 One mutator — the writer task (design §8)

> All filesystem mutations (open, append, rotate, rename, unlink) belong to the
> **writer task**. The httpd task only opens files read-only and posts intents
> (`confirm <name>`) into a small queue the writer drains.

And `sd_logger.h` L383–388 is stricter still — it forbids even *reading*:

```cpp
// Chunk lifecycle, rotation bounds and retention (design §3/§4/§7). The *index* belongs to the
// writer task, which is the only mutator of the card (§8): the boot scan fills it, close_file_()
// seals, retention_pass_() discards. Nothing outside that task may read it either, which is why
// `dump_config` and the stats line report configured values only — those are written by codegen
// before any task exists and never change again, while count() and the discard totals move under
// a reader that holds no lock.
```

**Owed, and this is the largest single piece of Phase B that does not exist yet:**

- Read-only `open()`/`read()`/`close()` of a SEALED chunk from the httpd task is
  explicitly permitted by §8. Do that directly.
- `rename()` (confirm) and `unlink()` (retention) stay on the writer. The handler
  posts an intent — a FreeRTOS queue of `uint32_t seq` drained next to
  `retention_pass_()` in the writer loop (`sd_logger.cpp` ~L1137).
- **`CollectionPolicy` itself has no concurrency story.** `find()`, `at()`,
  `count()`, `is_servable()` are plain `const` reads of `chunks_[]`/`count_`, and
  `discard()` *memmoves the array* to close a hole (L484–487). A handler
  iterating `at()` for the index while the writer discards is a genuine data
  race that can hand out a torn entry.
- `mark_serving()`, `clear_serving()` and `confirm()` are **mutators** called from
  the httpd side. `mark_serving()` cannot be a deferred intent: it must take
  effect *before the first block is read*, or retention has a window in which it
  deletes the file being served.

  Options, none of them in the tree today:
  1. A mutex (`SemaphoreHandle_t`, **not** a `portMUX` spinlock) around every
     `CollectionPolicy` access from both tasks. Simplest and correct. Note the
     critical sections are no longer short: `find()`, `oldest_()` and
     `next_victim()` are linear scans, and V26 raised `SD_LOG_MAX_CHUNKS` to a
     default of 256 with a schema ceiling of 2048. A spinlock held across a
     2048-entry scan with interrupts disabled is unacceptable next to the TWAI ISR
     discipline `can_gateway` enforces; a blocking mutex is fine because neither
     task is the record producer.
  2. A writer-published immutable snapshot of the index (seqlock, the shape
     `SnapshotRing` already uses in `gateway_core.h`) for the *read* paths, plus a
     synchronous request/ack queue for `mark_serving`. More machinery, less
     locking.
  3. Route everything through the writer with a synchronous ack (queue + task
     notification with a timeout). Most faithful to §8, and gives the handler a
     natural 500-on-timeout.

  **Recommend option 1 for Phase B v1.** It is the one that cannot be subtly
  wrong, and `sd_logger.h`'s "nothing outside that task may read it" comment
  becomes an accurate description of a *lock-free* access — which it currently is
  — rather than of the new design.

### 9.3 The serving guard against retention (design §7, last paragraph)

> The exception is the pressure case where retention deletes SEALED chunks that
> were never collected; that path needs a "currently serving" guard so it skips a
> file with a transfer in flight.

The primitives exist and are host-tested:

```cpp
bool mark_serving(uint32_t seq);   // false for unknown seq and for the OPEN chunk
void clear_serving(uint32_t seq);  // idempotent, tolerates an untracked seq
bool is_serving(uint32_t seq) const;
```

`next_victim()` skips serving chunks, and `discard()` refuses one again at the
task boundary:

```cpp
/// `false`, and nothing changes, for an unknown seq, for the OPEN chunk and for a chunk marked
/// serving. `next_victim()` already refuses those; this refuses them again because the caller in
/// between is a task boundary (§8), and the flag can be set after the victim was chosen.
```

**Owed:** `mark_serving(seq)` before the first `read()`, and `clear_serving(seq)`
on **every** exit path — success, client hang-up, send failure, card error,
handler abort, WiFi teardown. The cost of getting this wrong is stated in the
header:

> `/// The transfer finished, failed, or the connection dropped. Must be called on every exit path:`
> `/// a flag left set pins the oldest chunk and retention walks past it forever.`

Use an RAII guard, not `goto`-threaded cleanup. Note the identity window
(`bytes=0-255`) is also a transfer and must mark/clear too — it is the request
most likely to be abandoned mid-response.

Also note `next_victim()`'s deliberate refusal to spend a SEALED chunk while a
CONFIRMED one is merely busy:

```cpp
if (this->oldest_(ChunkState::CONFIRMED, /*skip_serving=*/false) != nullptr)
  return nullptr;  // the free list is not empty, only busy — wait for it rather than lose data.
```

So a stuck `serving` flag on a CONFIRMED chunk does not just leak one file — it
**stops retention entirely**, and the card fills. Same bug, worse blast radius.

### 9.4 Ring-fill backpressure (design §6)

> Between blocks, the serving handler checks record-ring fill. Above ~50 %, it
> sleeps a few ms before the next read.
> …
> A collection run must never be able to turn into `dropped_records`.

**No primitive exists.** `sd_logger.h` L313–318 has the raw state:

```cpp
LogRecord *ring_{nullptr};
uint32_t buffer_depth_{2048};
uint32_t ring_mask_{0};
volatile uint32_t head_{0};  // producer index
volatile uint32_t tail_{0};  // consumer index
portMUX_TYPE ring_mux_ = portMUX_INITIALIZER_UNLOCKED;
```

**Owed:** a public accessor, e.g.
`uint8_t ring_fill_percent() const { return (uint8_t)(((head_ - tail_) & ring_mask_) * 100u / buffer_depth_); }`.
Two `volatile` reads without the mux is acceptable *for a backpressure
heuristic* — a torn read misestimates fill by one pass and the next block
corrects it — but say so in a comment, because every other cross-task read in
this component is either atomic or mux-protected, and an unexplained exception
reads as a bug.

The acceptance criterion is already written: `dropped_records == 0` while a
transfer is in flight at production rate (~148 KB/s). That is a bench
measurement, not a unit test.

**Interaction with the client's 10 s timeout:** see §10.4.

### 9.5 The device name

`FakeDevice` is constructed with `"mr-orange"`. The firmware has no equivalent —
`sd_logger.h` carries no name field. Use `App.get_name()`. It is half the dedup
key (§2.4) and must be stable across reboots and DHCP leases.

### 9.6 Heap and task budget

§5a budgets "roughly 10 KB heap plus a task stack" on a chip §9.3 calls
heap-bound, with 100–200 KB free after WiFi, IDF, esphome, `can_gateway` and
`linbus`. The fake has a whole Python interpreter. Concretely:

- `esp_http_server` `config.stack_size` defaults to 4096. A 4–8 KB read buffer
  must **not** be a stack local. Use a static buffer or one heap allocation at
  server start.
- `max_open_sockets` default 7 — each socket costs heap. The client is strictly
  sequential and sends `Connection: close`, so 2–3 is enough; enable
  `lru_purge_enable` so an abandoned socket cannot wedge the server.
- **The index JSON no longer fits in RAM under any circumstances.** V26 moved
  `SD_LOG_MAX_CHUNKS` from 64 to a default of **256**, configurable to **2048**.
  At roughly 100 bytes of JSON per entry that is ~25 KB at the default and
  ~200 KB at the ceiling, against a §5a budget of ~10 KB and ~100–200 KB free
  heap. Buffering the index would have been merely wasteful at 64 entries; at 256
  it is a guaranteed allocation failure. It **must** be streamed (§10.2).
- The httpd task priority sits below WiFi (~23) and, at IDF's default
  `tskIDLE_PRIORITY + 5`, below `lin_uart_evt` at 18. That is the right ordering
  and should stay that way. Design §9.1's LIN-jitter measurement is still owed
  and is a bench item, not a code item.

---

## 10. Real-ESP32 constraints where the fake's behaviour cannot simply be copied

### 10.1 `Content-Length` on a streamed body — the significant one

§5b says "`Content-Length` set". §5c says reads are streamed, 4–8 KB at a time,
never resident. On ESP-IDF's `esp_http_server` those two pull in opposite
directions:

- `httpd_resp_send(req, buf, len)` sets `Content-Length` but wants the whole body
  in RAM — impossible for a 4 MB chunk.
- `httpd_resp_send_chunk()` streams, but emits `Transfer-Encoding: chunked` and
  **no `Content-Length`**.
- The escape hatch is writing the status line and headers yourself and pushing the
  body with `httpd_socket_send()` / `httpd_req_to_sockfd()`.

**I am flagging the exact IDF API surface here as recall, not as verified against
the pinned toolchain** (`requirements_test.txt` pins only `esphome>=2026.7.0`;
the IDF version comes from esphome's platform pin). Whoever implements this must
check it against the pinned IDF before choosing.

What the client does if `Content-Length` is absent, traced precisely:

- `_download` L670–671: `promised = None`, so the `got < promised` short-body
  check at L718 is **dead**. The primary cut-detection signal is gone.
- A cut is still caught, one level down: chunked framing means `http.client`
  raises `IncompleteRead`, a subclass of `HTTPException`, caught at L631 →
  attempt fails → partial kept → resume works.
- `_probe_head` L514: `owed = count if promised is None else min(promised, count)`
  → `owed = 256`, unchanged.

So **chunked encoding is contract-legal and survivable** — but it removes a
detection layer and violates §5b's letter. Recommendation: `Content-Length` for
the chunk body (raw-socket path), chunked for the index (§10.2). Whichever is
chosen, the 206 case still needs `Content-Length` = length of *this* body if it is
sent at all (§3.4).

### 10.2 The index may — and must — be chunked

`index()` does `json.loads(response.read().decode("utf-8"))` — it reads to EOF and
never inspects `Content-Length`. Chunked encoding is fine, and after V26 it is the
only option that fits (§9.6): emit `{"device":...,"chunks":[`, then one
`httpd_resp_send_chunk()` per entry from a small stack format buffer, then `]}`.
A cut mid-index surfaces as `IncompleteRead` or a JSON parse error, both of which
the client **retries** (§2.5), so a partially-sent index is safe.

Take the lock (§9.2) per entry or per small batch, not across the whole walk — a
2048-entry index streamed over car WiFi could otherwise hold the writer off for
seconds. Re-checking `count()` each iteration is required if the lock is dropped,
since `discard()` compacts the array under you.

### 10.3 Percent-decoding and query stripping

`req->uri` in `esp_http_server` is the raw URI **including any query string**, and
IDF exposes no public URI-unescape helper (the file-serving example ships its
own). The client sends no query strings, but a stray `?` from a browser must not
reach `parse_chunk_name()`. Truncate at `?` first, then decode into a 16-byte
buffer, then parse. Wildcard routing needs
`config.uri_match_fn = httpd_uri_match_wildcard` and patterns `/sdlog/f/*` and
`/sdlog/done/*`.

### 10.4 Backpressure sleeps vs. the client's 10 s timeout

§6's "sleep a few ms above ~50 % ring fill" is safe. An *unbounded* backpressure
stall is not: the client's socket timeout is 10 s (`DEFAULT_TIMEOUT`), and a gap
longer than that between socket writes is reported as a cut, which costs a retry
and — over a long enough stall — the whole run. Bound the cumulative per-block
delay (a few hundred ms at most, target well under 2 s) and let throughput drop
rather than the connection.

### 10.5 The abandoned socket

`_probe_head` reads exactly 256 bytes and then leaves the `with` block, closing
the connection. If the server ignored the range end, it is midway through sending
megabytes into a socket nobody will read. The fake explicitly declares this
expected:

```python
def handle_error(self, request, client_address) -> None:
    # The collector reads an identity window and hangs up on the rest. That
    # is a broken pipe here and is expected, not a fixture fault.
    if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
        return
```

On the device this is `ESP_ERR_HTTPD_RESP_SEND` / `ECONNRESET` from
`httpd_resp_send_chunk()`. It must be treated as a **normal end of transfer**:
stop reading the card, `clear_serving()`, close the fd, return. It must not log at
error level (it will happen on every dedup skip and every resume) and must not
leave the serving flag set. Honouring the range end makes it rare; handling it
correctly makes it harmless.

`send_wait_timeout` (default 5 s) governs how long a blocked send stalls the httpd
task. On car WiFi that is a real stall of the task that also holds the FATFS lock
if you structured it badly — hence §9.1's "never hold the lock across a socket
write".

---

## 11. Open questions the contract does not settle

Flagged honestly. Guesses are labelled.

### 11.1 `first_t_us` / `last_t_us` have no backing store

The fake emits both. `ChunkEntry` has four fields — `seq`, `bytes`, `state`,
`serving` — and none of them is a timestamp. Three ways out:

- **Omit the keys.** Contract-legal: `_opt_int(None)` → `None`, and the only
  consequence is `sdlog_collect.py index` printing `span=-` instead of
  `span=0.0s`. No `Collector` behaviour depends on them. *(The two tests that
  assert on these fields — `test_the_fake_device_serves_the_index_contract` and
  `test_index_lists_and_confirms_nothing` — test the **fake**, not the firmware.
  Do not read them as firmware requirements.)*
- Add two `uint64_t` to `ChunkEntry` (+16 B × 64 = 1 KB static RAM), set on
  `add()`/`seal()` from the writer's existing timebase. The honest fix.
- Read them out of each file's `#sdlog` header at index time — an SD read per
  chunk per index request. Rejected: it puts card I/O on the httpd path for a
  display nicety.

**Unresolved.** Recommend option 1 for v1, option 2 if the operator experience
matters.

### 11.2 The device name has no configured source

Design and fake both assume a stable device name; nothing in the schema or
`sd_logger.h` provides one. `App.get_name()` is the obvious answer but it is a
*choice*, and it silently changes the archive layout if a user renames the node.
Worth an explicit decision, possibly a `collection.device_name:` key. Not
settled.

### 11.3 `mark_serving` vs. §8's one-mutator rule

§8 says the httpd task "only opens files read-only and posts intents". But the
serving guard requires a mutation (`mark_serving`) that must be **effective before
the first byte is read**, which an intent queue drained at the writer's pace
cannot guarantee. The design does not acknowledge the tension; `collection_policy.h`
implies a shared-object model (it has `mark_serving` as a plain method and warns
that "the caller in between is a task boundary"). See §9.2 for the three options.
**This is a design decision Phase B has to make and the design doc does not.**

### 11.4 Renaming a file that a handler holds an open read fd to

`confirm()` deliberately leaves the serving flag alone, so the writer can rename
`.LOG`→`.UPL` while the httpd task still has the file open. On POSIX that is
fine. On ESP-IDF's FATFS (`CONFIG_FATFS_LFN_NONE`, `f_rename` under the VFS lock),
whether an open `FIL` survives a rename of its directory entry is **not verified
here — this is a guess that it does**, since the `FIL` holds cluster chain state
rather than a directory reference. In the client's actual flow the confirm always
follows a completed GET, so the race needs a stale `serving` flag or a second
puller to occur at all. Verify, or defer the rename until `is_serving(seq)` is
false.

### 11.5 What to answer for a confirm of an untracked seq

`CollectionPolicy::confirm()` returns `false` for a seq that is not tracked, and
explains why the entry is not resurrected:

> `/// and for a seq that is not tracked (retention may have discarded it while the transfer was in`
> `/// flight — the answer is "gone", not a resurrected entry naming a file whose rename would then`
> `/// fail on every pass).`

The fake never reaches this state (its `chunks` dict never loses entries), so the
status code is **unpinned**. Two defensible answers:

- **404** — "there is nothing here to confirm". The client reports the chunk as
  failed even though it holds good verified bytes. Harmless: the chunk is not in
  the next index either, so it is never re-fetched.
- **200** — "you may consider it deletable; it already is". Arguably more honest,
  and produces a clean run report.

Also unpinned: the same question when the index was **full** so a real, servable
file was never tracked. There the file exists but `find()` says nothing — and 404
is clearly right, because the firmware cannot rename what it is not tracking.
**Recommend 404 for both**, for consistency, and note that neither choice can lose
data.

This case is not hypothetical. V26 raised `SD_LOG_MAX_CHUNKS` from 64 to 256
precisely because a 12-minute bench soak on 2026-07-28 found the index **full at
the first of 29 rotations** — every chunk of that run untracked, so
`/sdlog/index` would have listed nothing at all. An untracked chunk is invisible
to all four endpoints, and the only witness is the new `index_refused=` counter in
the periodic stats line (`SdLogger::get_index_refused()`). Phase B should surface
that counter in `GET /sdlog/status` too: a device serving an empty index because
its index overflowed looks exactly like a device with nothing to give.

### 11.6 500 on a card that failed mid-transfer

Unmodelled by the fake. If `enter_failed_()` fires while a chunk is being served,
the handler has an open fd on a dead card. 500 is the right status (retryable by
the client), but the response headers may already be on the wire with a
`Content-Length` the body will never reach — at which point the only honest move
is to close the connection and let the client's short-body detection do its job.
**Decision needed:** best-effort 500 before the first byte, hard close after.

### 11.7 `Content-Length` vs. streaming on the pinned IDF

See §10.1. The API choice is unresolved and the exact `esp_http_server` surface
should be verified against the pinned toolchain before the implementation commits
to raw `httpd_socket_send()`.

### 11.8 Nothing here has been on hardware

Per M6 Phase A §1 (2026-07-28, git history): *"Nothing has been on hardware. No board was
flashed this session."* Rotation at 1–4 MB has never fired on a board (design §4),
`#gap` has never been emitted on a card, and no v1 file has ever been read back.
Phase B sits on top of all three. The handover's §3 ("read the card back before
flashing anything") is still item 1 and is **not** discharged by this document.

### 11.9 Auth, TLS, and the collection trigger

Design §9.3 and §10.1/§10.4: no decision on auth, TLS heap (~40–50 KB per mbedTLS
connection may not fit), or whether collection is continuous or gated on vehicle
idle. The client sends no credentials and has no `--token` flag, so **Phase B v1
is plain HTTP with no auth** by construction. Any later auth is a client change
too.

---

## 12. How to prove the firmware against this contract

The 66 existing tests prove the *client*. To prove the *device*, point the real
collector at a real board:

```bash
.venv/bin/python script/sdlog_collect.py index http://mr-orange.local:8080
.venv/bin/python script/sdlog_collect.py pull  http://mr-orange.local:8080 --into /tmp/arch
```

Minimum bench checklist, each item mapping to a section above:

1. `index` lists SEALED only; the OPEN file never appears (§2.3).
2. A `pull` on three chunks: `stored == confirmed`, `bytes_wire == bytes_fetched`
   (no identity windows on a clean run) (§2, §4).
3. A second `pull` immediately after: zero chunks listed, exit 0 (§4.3).
4. Kill WiFi mid-chunk; re-run. `outcome.resumed == [name]` — proves 206,
   `Content-Range` and the honest 206 `Content-Length` (§3.4).
5. `curl -i` the open file's name directly → 409 (§3.2). Requires knowing the open
   seq from the log.
6. `curl -i -r 99999999- <chunk>` → 416 with `Content-Range: bytes */<size>` (§3.6).
7. Confirm the same chunk twice by hand → 200 both times, second one *after* the
   rename (§4.3). This is the single most likely firmware bug.
8. `dropped_records == 0` on the stats line while a pull runs at production rate
   (§9.4) — the §6 assertion, and the only one the bench can make.
9. After a pull that is killed mid-chunk, confirm retention still runs: no chunk
   is pinned `serving` forever (§9.3). Watch the `retention:` stats line.
