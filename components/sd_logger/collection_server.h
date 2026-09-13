#pragma once

/// M6 Phase B — the on-device collection surface (docs/sdlog-collection-design.md §5).
///
/// A private `esp_http_server` instance on its own port, deliberately **not** esphome's
/// `web_server` (§5a): `web_server_base`'s handler API is the Arduino path, what the IDF path
/// exposes to external components is not contractual, and coupling to it would put this component
/// one esphome refactor away from needing a core edit.
///
/// Four routes, and the shape of every one of them is fixed by a client that already exists and is
/// already tested — `script/sdlog_collect.py` and the `FakeDevice`/`_Handler` pair in
/// `tests/sd_logger/test_collect.py`, whose own docstring calls itself "the contract". That is the
/// hazard this file is written against: those 66 tests stay green whatever the firmware does, so a
/// deviation here is invisible everywhere except on a board nobody can collect from.
///
///     GET  /sdlog/index      SEALED chunks only, as JSON. Streamed (chunked), never buffered.
///     GET  /sdlog/f/<name>   the bytes. `Range` honoured; 409 for the OPEN file; 404 otherwise.
///     POST /sdlog/done/<n>   the confirm. **Exactly 200**, answered from the index by seq.
///     GET  /sdlog/status     counters. The one route no client reads.
///
/// The five ways to break the collector without breaking anything visible, all of them settled in
/// the code below and each worth re-reading before editing it:
///
///   1. `Content-Length` on a 206 is the length of *that body*, not of the file. The client raises
///      on `got < promised`, so an inflated one makes every successful resume look like a cut
///      connection, permanently. That is also why the chunk body does not use
///      `httpd_resp_send_chunk()` — see `serve_chunk_()`.
///   2. A successful confirm must be exactly `200`. The client tests `status == 200`, not 2xx; a
///      `204` is reported as an error and the chunk is never collected.
///   3. The confirm is answered from the **index by seq**, never from the filesystem: the first
///      confirm renames `.LOG` -> `.UPL`, so a handler that `stat()`s would 404 the retry — which
///      is exactly the case where the first 200 was lost on the wire.
///   4. `Range` may be ignored **only** by answering 200 with the whole file from byte 0. A 206 is
///      a promise that the body starts at the requested offset; answering 206 and sending from 0
///      splices two files into a hybrid that parses clean and verifies clean.
///   5. The index is streamed. At V26's default of 256 entries it is ~25 KB of JSON against a ~10 KB
///      heap budget, and the schema allows 2048.
///
/// Everything here runs on the httpd task, which per §8 may **read** the card and must not mutate
/// it: the confirm posts an intent that `SdLogger::drain_confirms_()` executes on the writer task.
/// Every read of the chunk index goes through the `SdLogger::collection_*()` accessors, which hold
/// the index mutex (§8a) — `CollectionPolicy` itself is a passive data structure that knows nothing
/// about tasks, and `discard()` memmoves its array out from under an unlocked reader.

#include "esphome/core/defines.h"

#ifdef USE_SD_LOGGER_COLLECTION_SERVER

#include <atomic>
#include <cstdint>

#include <esp_http_server.h>

namespace esphome {
namespace sd_logger {

class SdLogger;

/// Card -> socket staging buffer, one heap allocation for the life of the server (§5c/§9.6).
/// Deliberately not a stack local: the httpd task's stack is a few KB and a 4 KB frame in a handler
/// that also calls into FATFS is how this crashes on the first large chunk.
static const size_t SD_LOG_SERVE_BLOCK = 4096;

/// The IDF HTTP server task must retain enough headroom for FATFS to follow a cluster link while a
/// chunk handler is live. A 5 KiB stack happened to cover reads within the first cluster, but the
/// deeper read path at the 64 KiB cluster boundary overwrote the handler's live state and made the
/// next socket writes expose unrelated memory. Keep the 4 KiB card/socket buffer on the heap. This
/// 8 KiB recovery allocation is deliberately provisional: `serve_chunk_()` reports the task's
/// measured minimum free stack after each completed response, which is the evidence used to size it.
static const uint32_t SD_LOG_HTTPD_STACK_SIZE = 8192;

/// `SO_SNDTIMEO` on the server's sockets, in whole seconds — `httpd_config_t` takes no finer unit.
/// It is how long a blocked `httpd_send()` can stall the httpd task, and the httpd task is the one
/// holding a read fd on the card, so it is also the *floor* for `unmount_card_()`'s drain bound in
/// sd_logger.cpp. Named here, and read there, so the two cannot drift apart: a drain shorter than
/// this gives up by construction on every stalled socket, which is how the original 500 ms bound
/// read as "the transfer is wedged" on a case that was merely slow.
static const uint32_t SD_LOG_SEND_WAIT_S = 2;

/// Ring fill above which a transfer sleeps before the next card read (§6). "Logging always wins":
/// a collection run must never be able to turn into `dropped_records`.
static const uint8_t SD_LOG_BACKPRESSURE_PERCENT = 50;
/// One sleep, and how many of them a single block may pay. The bound is what keeps backpressure
/// from becoming a cut connection: the client's socket timeout is 10 s, and a gap longer than that
/// between socket writes is indistinguishable from a car that drove away (§10.4). 8 x 5 ms is two
/// orders of magnitude inside it, and the throughput it gives up is throughput the writer needs.
static const uint32_t SD_LOG_BACKPRESSURE_SLEEP_MS = 5;
static const uint8_t SD_LOG_BACKPRESSURE_MAX_SLEEPS = 8;

/// A single card read held longer than this gets a warning with its duration — the same 50 ms
/// threshold the writer uses for write()/fsync (STALL_WARN_US in sd_logger.cpp), so one grep
/// collects both sides of a stall. On the read side the usual cause is not the read itself but the
/// FATFS volume mutex: the writer holds it while sdspi busy-polls a card stall, and this is where
/// the httpd task inherits that wait.
static const int64_t SD_LOG_READ_WARN_US = 50000;

class CollectionServer {
 public:
  explicit CollectionServer(SdLogger *parent) : parent_(parent) {}

  /// Bind and start. `false` leaves nothing running and nothing allocated; the caller says so once
  /// and the component goes on logging, because a card that fills is better than a card that stops.
  bool start(uint16_t port);
  /// Idempotent. Blocks until the httpd task has finished any handler in flight, which is what
  /// makes it safe to call before the writer's own shutdown.
  ///
  /// **Reachable from two tasks**: the main loop (`SdLogger::on_shutdown()`) and the writer task
  /// (`emergency_close_()`, on the `dying_` path a VCC sag or `request_emergency_close()` starts).
  /// Those can overlap — a shutdown during a sag — so the handle is claimed with an atomic exchange
  /// and only the winner calls `httpd_stop()` and frees the block. The loser returns immediately,
  /// which means it returns while a handler may still be running; both callers survive that, and it
  /// is strictly better than the double `httpd_stop()` / double `delete[]` the alternative gives.
  void stop();

  bool running() const { return this->handle_.load(std::memory_order_acquire) != nullptr; }
  uint16_t port() const { return this->port_; }

 protected:
  // The four routes. `user_ctx` carries the instance, so these stay ordinary members.
  static esp_err_t index_route_(httpd_req_t *req);
  static esp_err_t chunk_route_(httpd_req_t *req);
  static esp_err_t done_route_(httpd_req_t *req);
  static esp_err_t status_route_(httpd_req_t *req);

  esp_err_t serve_index_(httpd_req_t *req);
  esp_err_t serve_chunk_(httpd_req_t *req);
  esp_err_t serve_done_(httpd_req_t *req);
  esp_err_t serve_status_(httpd_req_t *req);

  /// `httpd_send()` returns a short count like `send()` does, so every raw write loops. `false`
  /// means the peer is gone — the normal end of an abandoned transfer (§10.5), not an error.
  bool send_all_(httpd_req_t *req, const char *buf, size_t len);
  /// §6: hold the card still while the ring drains. Bounded — see the constants above.
  void backpressure_();

  SdLogger *parent_;
  /// Atomic because `stop()` can be entered from the main loop and the writer task at once; see
  /// there. `httpd_handle_t` is a `void *`, so this is a lock-free word on every target.
  std::atomic<httpd_handle_t> handle_{nullptr};
  char *block_{nullptr};
  uint16_t port_{0};
  /// Half the collector's dedup key (§2.4), so it has to be stable across reboots and DHCP leases.
  /// Rendered once at start() rather than per request: it never changes, and JSON-escaping it on
  /// every index request would be work done for a string that came from the node name.
  char device_[40]{};
};

}  // namespace sd_logger
}  // namespace esphome

#endif  // USE_SD_LOGGER_COLLECTION_SERVER
