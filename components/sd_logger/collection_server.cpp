#include "collection_server.h"

#include "status_json.h"

#ifdef USE_SD_LOGGER_COLLECTION_SERVER

#include "sd_logger.h"
#include "sd_diagnostics.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

// `serve_chunk_()` has to measure the httpd-owned task from inside its handler. Require the
// FreeRTOS facilities that make both the measurement and a detected overflow actionable: IDF's
// method-2 check verifies canary bytes at each context switch and invokes its panic hook on damage.
// There is no per-httpd-task switch for either setting; these apply to the task httpd_start() owns.
#if INCLUDE_uxTaskGetStackHighWaterMark != 1
#error "sd_logger collection server requires uxTaskGetStackHighWaterMark"
#endif
#if configCHECK_FOR_STACK_OVERFLOW < 2
#error "sd_logger collection server requires FreeRTOS method-2 stack-overflow canaries"
#endif

namespace esphome {
namespace sd_logger {

static const char *const TAG = "sd_logger.collect";

static const char *const ROUTE_INDEX = "/sdlog/index";
static const char *const ROUTE_STATUS = "/sdlog/status";
static const char *const ROUTE_FSDEBUG = "/sdlog/fsdebug*";
static const char *const ROUTE_RAW = "/sdlog/raw";
static const char *const ROUTE_CHAIN = "/sdlog/chain";
static const char *const ROUTE_SPI_TRACE = "/sdlog/spitrace";
static const char *const STATUS_BAD_REQUEST = "400"
                                              " Bad Request";
static const char *const PREFIX_CHUNK = "/sdlog/f/";
static const char *const PREFIX_DONE = "/sdlog/done/";

namespace {

/// `clear_serving()` on **every** exit path — success, client hang-up, send failure, card error,
/// handler abort. The cost of missing one is stated in `collection_policy.h` and is worse than it
/// first reads: a flag left set on a CONFIRMED chunk does not leak one file, it makes
/// `next_victim()` return nothing at all ("the free list is not empty, only busy"), so retention
/// stops entirely and the card fills. Hence RAII rather than cleanup threaded through returns.
class ServingGuard {
 public:
  ServingGuard(SdLogger *parent, uint32_t seq) : parent_(parent), seq_(seq) {}
  ~ServingGuard() { this->parent_->collection_end_serve(this->seq_); }
  ServingGuard(const ServingGuard &) = delete;
  ServingGuard &operator=(const ServingGuard &) = delete;

 private:
  SdLogger *parent_;
  uint32_t seq_;
};

/// The same argument for the read fd. A leaked descriptor on FATFS is a file the writer cannot
/// rename and a VFS slot nothing gets back.
class FdGuard {
 public:
  explicit FdGuard(int fd) : fd_(fd) {}
  ~FdGuard() {
    if (this->fd_ >= 0)
      ::close(this->fd_);
  }
  FdGuard(const FdGuard &) = delete;
  FdGuard &operator=(const FdGuard &) = delete;

 private:
  int fd_;
};

class RawReadGuard {
 public:
  explicit RawReadGuard(SdLogger *parent) : parent_(parent) {}
  ~RawReadGuard() { this->parent_->collection_end_raw_read(); }
  RawReadGuard(const RawReadGuard &) = delete;
  RawReadGuard &operator=(const RawReadGuard &) = delete;

 private:
  SdLogger *parent_;
};

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/// `<prefix><name>` off `req->uri`, percent-decoded into `out` (SD_LOG_NAME_LEN bytes) and parsed.
///
/// The decode buffer is bounded *before* anything is decoded into it, and the only shape that
/// survives `parse_chunk_name()` is a fixed 8.3 `L#######.LOG` / `L#######.UPL` — which is what
/// makes `..`, `/`, embedded NULs and every other traversal attempt a 404 for free, without this
/// file ever having to reason about paths. No untrusted string reaches `open()`.
///
/// `req->uri` is the raw URI *including* any query string and IDF exposes no public unescape
/// helper, so the query is truncated here rather than left for `parse_chunk_name()` to trip over.
/// The client never sends one; a browser does.
bool decode_chunk_name(const char *uri, const char *prefix, char *out, uint32_t *seq_out) {
  const size_t plen = std::strlen(prefix);
  if (uri == nullptr || std::strncmp(uri, prefix, plen) != 0)
    return false;
  const char *src = uri + plen;
  size_t n = 0;
  while (*src != '\0' && *src != '?' && *src != '#') {
    if (n + 1 >= SD_LOG_NAME_LEN)
      return false;  // longer than any chunk name can be; stop before writing past the buffer
    char c = *src;
    if (c == '%') {
      const int hi = hex_value(src[1]);
      // Short-circuited on purpose: src[2] is only read once src[1] proved it is not the NUL.
      const int lo = hi < 0 ? -1 : hex_value(src[2]);
      if (lo < 0)
        return false;
      c = static_cast<char>((hi << 4) | lo);
      src += 2;
    }
    out[n++] = c;
    src++;
  }
  out[n] = '\0';
  if (n != SD_LOG_NAME_LEN - 1)
    return false;
  ChunkState state = ChunkState::NONE;
  // The state the *name* implies is deliberately discarded: it is not authoritative. `find(seq)` on
  // the index is, and a name alone can never say OPEN because the open file is also `.LOG`.
  return parse_chunk_name(out, seq_out, &state);
}

/// A small JSON body with a real `Content-Length`, which is what `httpd_resp_send()` gives.
/// `status` must outlive the call — `httpd_resp_set_status()` stores the pointer — so every caller
/// passes a literal.
esp_err_t reply_json(httpd_req_t *req, const char *status, const char *body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN) == ESP_OK ? ESP_OK : ESP_FAIL;
}

/// The two forms the collector can emit, and only those (§3.5):
///   `bytes=0-<count-1>`  the identity head window, `count` is min(local, 256) so it can be small;
///   `bytes=<start>-`     the resume, always open-ended.
/// Anything else — suffix ranges, multi-range, other units, garbage — is reported as "no range",
/// and the caller then answers 200 with the whole file. That is the one way a server is allowed to
/// ignore a Range (§7), and it is strictly better than the 400 the fake sends: the client treats
/// any non-416 4xx as fatal for the chunk, while a missing 206 merely costs it a restart.
/// **Untested against the client, which cannot reach this path.**
bool parse_range(const char *header, uint32_t *start_out, uint32_t *end_out, bool *has_end_out) {
  static const char PREFIX[] = "bytes=";
  while (*header == ' ')
    header++;
  if (std::strncmp(header, PREFIX, sizeof(PREFIX) - 1) != 0)
    return false;
  const char *p = header + sizeof(PREFIX) - 1;
  if (*p < '0' || *p > '9')
    return false;
  uint64_t start = 0;
  for (; *p >= '0' && *p <= '9'; p++) {
    start = start * 10 + static_cast<uint64_t>(*p - '0');
    if (start > 0xFFFFFFFFull)
      return false;
  }
  if (*p != '-')
    return false;
  p++;
  uint64_t end = 0;
  bool has_end = false;
  for (; *p >= '0' && *p <= '9'; p++) {
    has_end = true;
    end = end * 10 + static_cast<uint64_t>(*p - '0');
    if (end > 0xFFFFFFFFull)
      end = 0xFFFFFFFFull;  // clamped, not refused: the end is clamped to EOF a few lines later
  }
  while (*p == ' ')
    p++;
  if (*p != '\0')
    return false;  // a comma means a multi-range request, which is not implemented
  if (has_end && end < start)
    return false;
  *start_out = static_cast<uint32_t>(start);
  *end_out = static_cast<uint32_t>(end);
  *has_end_out = has_end;
  return true;
}

}  // namespace

// -------------------------------------------------------------------------------- lifecycle

bool CollectionServer::start(uint16_t port) {
  if (this->handle_.load(std::memory_order_acquire) != nullptr)
    return true;

  this->block_ = new (std::nothrow) char[SD_LOG_SERVE_BLOCK];
  if (this->block_ == nullptr) {
    ESP_LOGE(TAG, "serve buffer alloc (%u B) failed — collection server not started",
             static_cast<unsigned>(SD_LOG_SERVE_BLOCK));
    return false;
  }

  // §2.4: half the collector's dedup key, and its fallback is the URL hostname, which is not stable
  // across DHCP leases and mDNS spellings. `App.get_name()` is the node name — stable across
  // reboots, already unique on the network, and it costs no new config key. §11.2 leaves the source
  // unsettled and floats a `collection.device_name:`; this is the choice, and the consequence is
  // written down: renaming the node changes the archive directory the collector writes into.
  const std::string &name = App.get_name();
  size_t out = 0;
  for (size_t i = 0; i < name.size() && out + 1 < sizeof(this->device_); i++) {
    const char c = name[i];
    // JSON-safe by construction rather than by trusting the node name's validator: printable ASCII
    // only, and the two characters that would end the string early are dropped rather than escaped,
    // because a device name is a key and a key with a backslash in it is a bug either way.
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
      continue;
    this->device_[out++] = c;
  }
  this->device_[out] = '\0';
  if (name.size() + 1 > sizeof(this->device_)) {
    // Said out loud rather than truncated in silence: this string is half the key the collector
    // dedups on and names the directory it archives into, so a quietly shortened one splits an
    // archive in two the day someone lengthens the node name past this buffer.
    ESP_LOGW(TAG, "device name '%s' does not fit %u B and was cut to '%s'", name.c_str(),
             static_cast<unsigned>(sizeof(this->device_) - 1), this->device_);
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  // The control socket is a UDP port the server talks to itself on. Defaulting it would collide with
  // esphome's own web_server on a config that runs both, which V25 cannot see because it only
  // compares the *server* ports. So it is derived from ours — but `port + 1` alone is wrong at two
  // values, and both of them fail *silently*:
  //
  //   * `port: 65535` (which `cv.port` accepts) makes the cast 0. `cs_create_ctrl_sock(0)` binds an
  //     ephemeral UDP port and **succeeds**, so the server starts and looks healthy — until
  //     `httpd_stop()` sends its shutdown message to 127.0.0.1:0, nothing receives it, and the IDF
  //     spins `while (status != THREAD_STOPPED) sleep(100)` forever. `stop()` is the first thing
  //     `SdLogger::on_shutdown()` does, on the main loop, so OTA and every requested reboot wedge;
  //   * `port: 32767` makes it 32768, which is `ESP_HTTPD_DEF_CTRL_PORT` — the value esphome's own
  //     `web_server_idf` leaves untouched. One of the two `httpd_start()`s then fails to bind its
  //     control socket, and which one loses depends on setup order.
  //
  // Both are answered here rather than in the schema on purpose. A validator would have to know
  // whether esphome's web_server is in the config and which control port *it* ended up with — state
  // V25 does not have and would have to guess at — and rejecting `port: 65535` would break a config
  // that is legal today over an implementation detail of a UDP socket the user never asked for. The
  // firmware knows the one thing that matters: this number has to be non-zero, and not the default.
  uint16_t ctrl_port = static_cast<uint16_t>(port + 1);
  if (ctrl_port == 0 || ctrl_port == ESP_HTTPD_DEF_CTRL_PORT) {
    // Downwards instead. `cv.port` refuses 0, so `port - 1` is a valid port whenever this fires
    // (only at 65535 and 32767), and it cannot itself be 0 or 32768.
    ctrl_port = static_cast<uint16_t>(port - 1);
    ESP_LOGW(TAG, "collection port %u would put the control socket on %u — using %u instead", port,
             static_cast<unsigned>(static_cast<uint16_t>(port + 1)), ctrl_port);
  }
  config.ctrl_port = ctrl_port;
  // The collector is strictly sequential and sends `Connection: close` on every request, so three
  // sockets is two more than it needs — and each one costs heap. `lru_purge_enable` is what keeps
  // an abandoned socket (§10.5: the identity window hangs up mid-response) from wedging the server
  // until its timeout expires.
  config.max_open_sockets = 3;
  config.lru_purge_enable = true;
  config.max_uri_handlers = 8;
  // How long a *single* blocked `send()` stalls the httpd task — a per-syscall `SO_SNDTIMEO`, not a
  // budget for the request. It matters here because that task is the one holding a read fd on the
  // card, so this is also the floor for `unmount_card_()`'s drain bound (SD_LOG_SEND_WAIT_S, read
  // from collection_server.h at that end).
  //
  // Deliberately **not** justified against the client's 10 s timeout, which is a per-`recv` socket
  // timeout of its own and not a per-request one either; the two never had the relationship the
  // earlier note claimed. What two seconds actually buys is a bounded worst-case teardown, and what
  // it costs is real: any single send that blocks longer aborts the transfer, so a congested link
  // resumes from offset instead of finishing. That trade is deliberate — a chunk that resumes is
  // free, a card that cannot be unmounted for 5 s while the writer waits is not — but it is a trade,
  // and 2 is the lower bound this component should ever use, not a target to shave.
  config.send_wait_timeout = static_cast<uint16_t>(SD_LOG_SEND_WAIT_S);
  // The same, in the other direction, and lowered from the IDF's 5 for the same reason rather than
  // left at a default whose only argument is that it is the default. Every one of these routes is a
  // request with no body worth waiting on — the confirm sends `Content-Length: 0` — so a receive
  // that blocks is a peer that went away mid-header, and holding the single httpd task on it for
  // five seconds delays every other request behind it.
  config.recv_wait_timeout = static_cast<uint16_t>(SD_LOG_SEND_WAIT_S);
  // The handler reads the card and formats JSON on this stack. The 8 KiB budget is independently
  // measured by the completed-response high-water mark below; the staging block stays on the heap.
  config.stack_size = SD_LOG_HTTPD_STACK_SIZE;
  // Below the writer task (6), far below `lin_uart_evt` (18) and the WiFi tasks (~23). Serving must
  // never preempt logging or bus timing — §6's whole rule is that logging wins.
  //
  // **Set, not assumed.** This is the same value as the IDF default (`tskIDLE_PRIORITY + 5`), and
  // asserting that in a comment is what the previous version did — which leaves the one relationship
  // this component actually depends on, `httpd < writer`, resting on a constant in someone else's
  // header. An IDF bump that raised the default to 7 would silently invert it, and the symptom
  // would be a collection run that steals SPI time from the writer under load: dropped records on a
  // board nobody was collecting from at the time the ring filled.
  config.task_priority = tskIDLE_PRIORITY + 5;
  config.uri_match_fn = httpd_uri_match_wildcard;

  httpd_handle_t handle = nullptr;
  const esp_err_t err = httpd_start(&handle, &config);
  if (err != ESP_OK) {
    delete[] this->block_;
    this->block_ = nullptr;
    ESP_LOGE(TAG, "httpd_start on port %u failed (%s) — chunks can only be collected by hand", port,
             esp_err_to_name(err));
    return false;
  }
  this->handle_.store(handle, std::memory_order_release);
  this->port_ = port;

  // Wildcard patterns for the two routes that carry a name; `httpd_uri_match_wildcard` still
  // compares the fixed routes exactly, so there is no overlap to order.
  // Only the four fields that always exist are named: `httpd_uri_t` grows `is_websocket` and
  // friends only under CONFIG_HTTPD_WS_SUPPORT, which esphome leaves off, and naming them would
  // make this file depend on an sdkconfig option it has no business having an opinion about.
  // Everything omitted is value-initialised.
  const httpd_uri_t routes[] = {
      {.uri = ROUTE_INDEX, .method = HTTP_GET, .handler = &CollectionServer::index_route_, .user_ctx = this},
      {.uri = ROUTE_STATUS, .method = HTTP_GET, .handler = &CollectionServer::status_route_, .user_ctx = this},
      {.uri = ROUTE_FSDEBUG, .method = HTTP_GET, .handler = &CollectionServer::fsdebug_route_, .user_ctx = this},
      {.uri = ROUTE_RAW, .method = HTTP_GET, .handler = &CollectionServer::raw_route_, .user_ctx = this},
      {.uri = ROUTE_CHAIN, .method = HTTP_GET, .handler = &CollectionServer::chain_route_, .user_ctx = this},
      {.uri = ROUTE_SPI_TRACE, .method = HTTP_GET, .handler = &CollectionServer::spitrace_route_, .user_ctx = this},
      {.uri = "/sdlog/f/*", .method = HTTP_GET, .handler = &CollectionServer::chunk_route_, .user_ctx = this},
      {.uri = "/sdlog/done/*", .method = HTTP_POST, .handler = &CollectionServer::done_route_, .user_ctx = this},
  };
  for (const httpd_uri_t &route : routes) {
    const esp_err_t reg = httpd_register_uri_handler(handle, &route);
    if (reg != ESP_OK) {
      ESP_LOGE(TAG, "registering %s failed (%s)", route.uri, esp_err_to_name(reg));
      this->stop();
      return false;
    }
  }

  ESP_LOGI(TAG, "collection server on port %u as '%s' (GET /sdlog/index)", port, this->device_);
  return true;
}

void CollectionServer::stop() {
  // Claim the handle: whoever exchanges the non-null value owns the teardown. `on_shutdown()` (main
  // loop) and `emergency_close_()` (writer task) can both arrive here, and a plain null check would
  // let both call `httpd_stop()` on the same handle and both `delete[]` the same block.
  httpd_handle_t handle = this->handle_.exchange(nullptr, std::memory_order_acq_rel);
  if (handle == nullptr)
    return;
  // Blocks until the httpd task is gone, so no handler can still be holding a serving flag or a
  // read fd when the writer starts its own shutdown.
  httpd_stop(handle);
  // Only now, and only on this path: a handler may have been mid-`::read()` into it until
  // `httpd_stop()` returned, and freeing it from the losing caller would be a use-after-free in the
  // one case this whole function is about.
  delete[] this->block_;
  this->block_ = nullptr;
}

// -------------------------------------------------------------------------------- trampolines

esp_err_t CollectionServer::index_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_index_(req);
}
esp_err_t CollectionServer::chunk_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_chunk_(req);
}
esp_err_t CollectionServer::done_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_done_(req);
}
esp_err_t CollectionServer::status_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_status_(req);
}
esp_err_t CollectionServer::fsdebug_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_fsdebug_(req);
}
esp_err_t CollectionServer::raw_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_raw_(req);
}
esp_err_t CollectionServer::chain_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_chain_(req);
}
esp_err_t CollectionServer::spitrace_route_(httpd_req_t *req) {
  return static_cast<CollectionServer *>(req->user_ctx)->serve_spitrace_(req);
}

// -------------------------------------------------------------------------------- helpers

bool CollectionServer::send_all_(httpd_req_t *req, const char *buf, size_t len) {
  while (len > 0) {
    const int sent = httpd_send(req, buf, len);
    if (sent <= 0)
      return false;
    buf += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

void CollectionServer::backpressure_() {
  for (uint8_t i = 0; i < SD_LOG_BACKPRESSURE_MAX_SLEEPS; i++) {
    // Sleeping only helps while somebody is emptying the ring. The writer drains it into the open
    // file and stops the moment `file_ok_()` goes false — `enter_failed_()` sets `mounted_` false in
    // the same breath — and producers are gated on `mounted_` too, so from then on the fill is a
    // frozen number, not a queue that is about to move. Waiting on it would pay the full
    // 8 x 5 ms per block for the rest of the run, for a ring nothing is draining: a transfer
    // throttled to a quarter of its rate in the exact situation where finishing it matters most,
    // because the chunks still on that card may be all that is left of the run.
    if (!this->parent_->is_mounted())
      return;
    if (this->parent_->ring_fill_percent() < SD_LOG_BACKPRESSURE_PERCENT)
      return;
    vTaskDelay(pdMS_TO_TICKS(SD_LOG_BACKPRESSURE_SLEEP_MS));
  }
}

// -------------------------------------------------------------------------------- GET /sdlog/index

esp_err_t CollectionServer::serve_index_(httpd_req_t *req) {
  // Chunked, not buffered, and that is not an optimisation. V26 put the default index at 256
  // entries with a schema ceiling of 2048; at ~40 B of JSON an entry that is 10-80 KB against a
  // §5a budget of ~10 KB on a chip with ~100-200 KB free. `index()` on the client reads to EOF and
  // never looks at `Content-Length`, so `Transfer-Encoding: chunked` is contract-legal here — and a
  // cut mid-index surfaces as `IncompleteRead` or a JSON parse error, both of which it *retries*.
  httpd_resp_set_type(req, "application/json");
  // One small stack buffer, reused per entry. Every write through it is checked for truncation:
  // snprintf returns the length it *would* have needed, and handing that to send_chunk would read
  // past the buffer — an entry is ~52 B against 96, so this can only fire if someone widens a
  // field, which is exactly when a silent overrun would be hardest to find.
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "{\"device\":\"%s\",\"chunks\":[", this->device_);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(buf))
    return ESP_FAIL;
  if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
    return ESP_FAIL;

  bool first = true;
  // `count()` is re-read every iteration on purpose: the lock is dropped between entries so a
  // 2048-entry index cannot hold the writer off for the length of a car-WiFi round trip, and
  // `discard()` compacts the array under us. The two ways that can go wrong are both harmless — an
  // entry emitted twice is collapsed by the client's dedup, and one skipped appears in the next
  // index. Holding the lock across the whole walk would be the actual bug.
  for (uint16_t i = 0; i < this->parent_->collection_count(); i++) {
    ChunkEntry entry{};
    if (!this->parent_->collection_entry_at(i, &entry))
      break;
    // SEALED only (§2.3). The OPEN file is the one the writer holds an fd to and CONFIRMED chunks
    // have already been collected; listing either is how a puller ends up confirming a live file.
    if (entry.state != ChunkState::SEALED)
      continue;
    char name[SD_LOG_NAME_LEN];
    format_chunk_name(name, entry.seq, ChunkState::SEALED);
    // `bytes` earns its place three times over (§2.6): without it the client cannot pre-check a
    // wrong-length archived copy, cannot catch an over-long partial before the 416, and — the
    // expensive one — cannot take the "a previous run got every byte and died before the rename"
    // shortcut, so it re-reads the whole chunk over the air.
    //
    // Emitted verbatim, saturation included: a `bytes` of 4294967295 can only come from the boot
    // scan `stat()`ing a foreign 4 GiB+ file that happens to be named like a chunk, and
    // over-reporting a stranger's size is the deliberate direction (see `ChunkEntry`).
    //
    // No `first_t_us`/`last_t_us`: `ChunkEntry` has no timestamp to report and reading each file's
    // `#sdlog` header at index time would put card I/O on the httpd path for a display nicety
    // (§11.1, option 1). The keys are optional — the client's `_opt_int(None)` accepts their
    // absence and only `sdlog_collect.py index` notices, printing `span=-`.
    n = snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"seq\":%" PRIu32 ",\"bytes\":%" PRIu32 "}", first ? "" : ",",
                 name, entry.seq, entry.bytes);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf))
      return ESP_FAIL;
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
      return ESP_FAIL;
    first = false;
  }

  if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK)
    return ESP_FAIL;
  return httpd_resp_send_chunk(req, nullptr, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------- GET /sdlog/f/<name>

esp_err_t CollectionServer::serve_chunk_(httpd_req_t *req) {
  char name[SD_LOG_NAME_LEN];
  uint32_t seq = 0;
  if (!decode_chunk_name(req->uri, PREFIX_CHUNK, name, &seq))
    return reply_json(req, "404 Not Found", "{\"error\":\"no such chunk\"}");

  uint32_t indexed_bytes = 0;
  ChunkState state = ChunkState::NONE;
  // One locked transaction: the state is read and the serving flag set together, because between a
  // separate read and a separate mark, retention has a window in which it deletes the file about to
  // be served. `mark_serving()` is the one index mutation the httpd task cannot defer into the
  // writer's intent queue for exactly that reason (§8a).
  if (!this->parent_->collection_begin_serve(seq, &indexed_bytes, &state)) {
    // OPEN is checked before existence, like the fake does: a stale index can name the open file,
    // and 409 is what tells the client "not this one, keep going". Confirming the open file would
    // make the live file deletable.
    if (state == ChunkState::OPEN)
      return reply_json(req, "409 Conflict", "{\"error\":\"open\"}");
    // CONFIRMED, discarded by retention, never tracked because the index was full, or simply not a
    // chunk. All 404: the client reports the chunk failed and the run continues.
    return reply_json(req, "404 Not Found", "{\"error\":\"no such chunk\"}");
  }
  ServingGuard serving(this->parent_, seq);

  // The on-card name comes from the *granted* state, not from what was asked for: a SEALED chunk is
  // always `.LOG`, whatever spelling reached the URL.
  char on_card[SD_LOG_NAME_LEN];
  format_chunk_name(on_card, seq, ChunkState::SEALED);
  char path[128];
  const int path_len = snprintf(path, sizeof(path), "%s/%s", this->parent_->mount_point().c_str(), on_card);
  if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path))
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"path\"}");

  // Read-only `open()`/`read()`/`close()` from the httpd task is what §8 explicitly permits; every
  // mutation (rename, unlink) stays on the writer. The invariant that makes this survivable is
  // file-level, not lock-level: the writer only ever appends to the OPEN file and only SEALED
  // chunks are servable, so the two never touch the same file.
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) {
    // 500, not 404: the index says this chunk exists, so a card that will not open it is a
    // transient fault and 5xx is the only status the client retries. A 404 here would tell the
    // operator "wrong firmware".
    ESP_LOGW(TAG, "open %s failed (errno %d)", path, errno);
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"card\"}");
  }
  FdGuard fd_guard(fd);

  struct stat st;
  if (::fstat(fd, &st) != 0 || st.st_size < 0)
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"card\"}");
  // The size on the card, not the size in the index. They agree — `seal()` records the final size
  // and refuses a second seal precisely so the number the collector was given cannot be rewritten —
  // but `Content-Length` is a promise about the bytes this response will actually produce, so it is
  // taken from the thing that produces them. `off_t` is 32-bit here and no file this component
  // writes reaches 2 GiB; a stranger that large is refused rather than served with a wrapped length.
  if (st.st_size > 0x7FFFFFFF)
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"too large\"}");
  const uint32_t total = static_cast<uint32_t>(st.st_size);
  // An empty file is not a chunk, and it is answered here — before the Range block — because every
  // answer available further down is wrong for it. `want_start >= total` is `0 >= 0`, so a zero-byte
  // chunk 416s **every** Range including the client's identity probe, which reads as "your partial
  // is longer than what I hold": the client drops its partial, restarts, gets an empty 200, and
  // verifies a `.part` that was never created. Answering 200 with `Content-Length: 0` instead is no
  // better — it is a promise of a complete chunk with no `#sdlog` header in it, which no reader and
  // no `verify_chunk()` can accept.
  //
  // Not unreachable, which is why this is a branch and not a comment. `close_file_()` cannot produce
  // one — it writes `#close`, flushes and only then seals — but the *boot scan* can index one, and
  // the file it indexes is the ordinary residue of a reset between `open(O_CREAT)` in
  // `open_next_file_()` and the first flush that puts the header on the card. That window is exactly
  // the one this component's recovery ladder exists for. 404 rather than 5xx: the client treats 4xx
  // as final for the chunk and goes on to the next, which is right for a file no retry can fix, and
  // retention still reclaims it as a (0 KB) discard.
  if (total == 0) {
    ESP_LOGW(TAG, "%s is 0 B on the card — not a chunk, refusing to serve it", on_card);
    return reply_json(req, "404 Not Found", "{\"error\":\"no such chunk\"}");
  }
  if (total != indexed_bytes) {
    // Not fatal — what is served is what is on the card, and it is served honestly — but the index
    // is what `/sdlog/index` advertised as `bytes`, and `seal()` refuses a second seal precisely so
    // that number cannot drift. If this ever prints, one of those two invariants is broken and the
    // client's cheap length pre-checks are running against a number that is not the file's.
    ESP_LOGW(TAG, "%s: index says %" PRIu32 " B, the card says %" PRIu32 " B", on_card, indexed_bytes, total);
  }

  uint32_t start = 0;
  uint32_t end = total > 0 ? total - 1 : 0;
  bool partial = false;
  char range[64];
  if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
    uint32_t want_start = 0;
    uint32_t want_end = 0;
    bool has_end = false;
    if (parse_range(range, &want_start, &want_end, &has_end)) {
      if (want_start >= total) {
        // 416, and only for this. The client special-cases it: it drops its local partial ("it is
        // longer than the chunk offered") and retries inside the same run. Answering 416 for a
        // range whose *end* runs past EOF would instead turn `_prove_local`'s correct mismatch
        // verdict into a fatal 4xx for the chunk — hence the clamp below rather than a second 416.
        char content_range[48];
        snprintf(content_range, sizeof(content_range), "bytes */%" PRIu32, total);
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        httpd_resp_set_type(req, "text/csv");
        httpd_resp_set_hdr(req, "Content-Range", content_range);
        return httpd_resp_send(req, nullptr, 0) == ESP_OK ? ESP_OK : ESP_FAIL;
      }
      start = want_start;
      if (has_end && want_end < end)
        end = want_end;
      partial = true;
    }
    // else: unparseable. Fall through to the full 200 — see `parse_range()` for why that is the
    // safe way to ignore a Range and 400 is not.
  }

  const uint32_t body = total == 0 ? 0 : end - start + 1;

  // Headers written by hand, and this is the one place the IDF's own helpers cannot be used.
  // `httpd_resp_send()` sets `Content-Length` but wants the whole body in RAM — impossible for a
  // 4 MB chunk on a chip with no PSRAM — and `httpd_resp_send_chunk()` streams but emits
  // `Transfer-Encoding: chunked` and no `Content-Length` at all, which would delete the client's
  // primary cut detection (`got < promised`). So: the status line and headers go out raw and the
  // body follows in SD_LOG_SERVE_BLOCK pieces.
  //
  // **`Content-Length` on a 206 is the length of THIS body, not of the file.** Getting that wrong
  // is the single easiest way to break every resume there will ever be: the client raises
  // `_Interrupted` on `got < promised`, so an inflated length makes each successful resume look
  // like a cut connection, forever, with nothing anywhere saying so.
  char head[192];
  int head_len;
  if (partial) {
    head_len = snprintf(head, sizeof(head),
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Type: text/csv\r\n"
                        "Content-Range: bytes %" PRIu32 "-%" PRIu32 "/%" PRIu32 "\r\n"
                        "Content-Length: %" PRIu32 "\r\n"
                        "\r\n",
                        start, end, total, body);
  } else {
    head_len = snprintf(head, sizeof(head),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/csv\r\n"
                        "Content-Length: %" PRIu32 "\r\n"
                        "\r\n",
                        body);
  }
  if (head_len <= 0 || static_cast<size_t>(head_len) >= sizeof(head))
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"headers\"}");
  if (!this->send_all_(req, head, static_cast<size_t>(head_len)))
    return ESP_FAIL;  // the peer is already gone; nothing else to say to it

  if (body == 0)
    return ESP_OK;
  if (start != 0 && ::lseek(fd, static_cast<off_t>(start), SEEK_SET) != static_cast<off_t>(start)) {
    // The 206 promised bytes from `start` and the card cannot produce them. The headers are on the
    // wire, so there is no status left to send: close hard and let the client's short-body check do
    // its job. Never send from byte 0 after promising 206 — that splices two files into a hybrid
    // that parses clean and verifies clean, the one corruption no later check catches.
    ESP_LOGW(TAG, "seek %s to %" PRIu32 " failed (errno %d)", on_card, start, errno);
    return ESP_FAIL;
  }

  uint32_t remaining = body;
  bool logged_64k_read = false;
  while (remaining > 0) {
    // Between blocks, not inside them: the card read is the part that competes with the writer for
    // SPI2, so the sleep has to happen before it (§6). Bounded, so a busy ring throttles the
    // transfer instead of stalling it past the client's 10 s socket timeout.
    this->backpressure_();
    const uint32_t read_offset = start + (body - remaining);
    const size_t want = remaining < SD_LOG_SERVE_BLOCK ? remaining : SD_LOG_SERVE_BLOCK;
    // Timed like the writer's write(): when the writer is spinning out a card stall it holds the
    // FATFS volume mutex, and this read is where the httpd task inherits that wait. A warning here
    // that pairs with a "write() held" / "writer pass gap" line on the same timestamp is the
    // signature that separates mutex convoy from a slow card read of its own.
    const int64_t t0 = esp_timer_get_time();
    const ssize_t got = ::read(fd, this->block_, want);
    // Snapshot the one known failure boundary before anything can hand this buffer to the socket.
    // A whole-file transfer reaches it exactly once with the 4 KiB serve block. This is deliberately
    // a 16-byte sample rather than a hot-path checksum or per-read log, so it does not perturb the
    // writer/reader timing that reproduces the issue.
    if (!logged_64k_read && read_offset == 65536) {
      char first_bytes[16 * 2 + 1] = {};
      const size_t sample_len = got <= 0 ? 0 : (static_cast<size_t>(got) < 16 ? static_cast<size_t>(got) : 16);
      for (size_t i = 0; i < sample_len; i++)
        snprintf(first_bytes + i * 2, 3, "%02X", static_cast<unsigned>(static_cast<uint8_t>(this->block_[i])));
      ESP_LOGI(TAG, "read boundary: offset %" PRIu32 ", requested %u B, returned %d B, first %u B %s", read_offset,
               static_cast<unsigned>(want), static_cast<int>(got), static_cast<unsigned>(sample_len), first_bytes);
      logged_64k_read = true;
    }
    const int64_t held_us = esp_timer_get_time() - t0;
    if (held_us > SD_LOG_READ_WARN_US) {
      ESP_LOGW(TAG, "read %s held %" PRId64 " ms (%u B)", on_card, held_us / 1000, static_cast<unsigned>(want));
    }
    if (got <= 0) {
      // A short read on a file whose length we just measured means the card went away underneath
      // us (§11.6). Same reasoning as the failed seek: headers are committed, so close hard.
      ESP_LOGW(TAG, "read %s failed at %" PRIu32 " B remaining (errno %d)", on_card, remaining, errno);
      return ESP_FAIL;
    }
    if (!this->send_all_(req, this->block_, static_cast<size_t>(got))) {
      // The collector read its identity window and hung up, or the car drove away. Expected, not an
      // error: stop reading the card, drop the flag (the guard), close the fd (the guard), and let
      // the core reap the socket. Logged at DEBUG because it happens on every abandoned transfer.
      ESP_LOGD(TAG, "peer closed while sending %s, %" PRIu32 " B left", on_card, remaining);
      return ESP_FAIL;
    }
    remaining -= static_cast<uint32_t>(got);
  }
  // ESP-IDF reports this in bytes (not FreeRTOS words), and it is the minimum since the httpd task
  // started. Query it only after a completed response so ordinary client disconnects do not
  // produce misleading sizing data. The historic minimum includes the deepest f_read() path that
  // crossed a FATFS cluster boundary during this and earlier completed transfers.
  const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(TAG, "served %s: %" PRIu32 " B; httpd stack minimum free %u / %u B", on_card, body,
           static_cast<unsigned>(stack_free), static_cast<unsigned>(SD_LOG_HTTPD_STACK_SIZE));
  return ESP_OK;
}

// ------------------------------------------------------------------------- POST /sdlog/done/<name>

esp_err_t CollectionServer::serve_done_(httpd_req_t *req) {
  // The request carries `Content-Length: 0` and a `Content-Type`. Nothing to read, and anything
  // that did arrive is purged by `httpd_req_delete()` after this returns.
  char name[SD_LOG_NAME_LEN];
  uint32_t seq = 0;
  if (!decode_chunk_name(req->uri, PREFIX_DONE, name, &seq))
    return reply_json(req, "404 Not Found", "{\"error\":\"no such chunk\"}");

  // Answered from the index, by seq, never from the filesystem. After the first confirm the writer
  // renames `.LOG` -> `.UPL`, so a handler that `stat()`ed the name would 404 the retry — and the
  // retry is exactly the case where the first 200 was lost on the wire. This is the single most
  // likely bug in this file, and `CollectionPolicy::confirm()` already encodes the right answer.
  switch (this->parent_->collection_confirm(seq)) {
    case SdLogger::ConfirmResult::CONFIRMED: {
      // **Exactly 200.** The client's confirm loop is `if response.status == 200`, not "2xx means
      // yes": a 204 — the tempting choice for an empty ack — is reported as `confirm: HTTP 204` and
      // the chunk is left uncollected forever. The body is never read.
      char body[48];
      snprintf(body, sizeof(body), "{\"confirmed\":\"%s\"}", name);
      return reply_json(req, "200 OK", body);
    }
    case SdLogger::ConfirmResult::IS_OPEN:
      return reply_json(req, "409 Conflict", "{\"error\":\"open\"}");
    case SdLogger::ConfirmResult::BUSY:
      // The writer's intent queue is full, which on a healthy board means it has not run for a
      // while. 503 is retryable and the confirm is idempotent, so a retry costs nothing.
      return reply_json(req, "503 Service Unavailable", "{\"error\":\"busy\"}");
    case SdLogger::ConfirmResult::NOT_TRACKED:
    default:
      // Unpinned by the contract — the fake can never reach it (§11.5). 404 for both readings:
      // retention discarded the chunk while the transfer was in flight, or the index was full when
      // the file appeared and the firmware cannot rename what it is not tracking. Neither can lose
      // data: the client reports the chunk failed while holding good verified bytes, and the chunk
      // is not in the next index either, so it is never re-fetched.
      return reply_json(req, "404 Not Found", "{\"error\":\"no such chunk\"}");
  }
}

// ------------------------------------------------------------------------------ GET /sdlog/status

esp_err_t CollectionServer::serve_status_(httpd_req_t *req) {
  // The one route no client reads — `sdlog_collect.py` contains no `/sdlog/status` at all — so this
  // is the only place the firmware is free. It answers the operator's question with the established
  // eight keys, then the card-state and writer counters a host needs to distinguish recovery from
  // an unmounted card or a failed fill query.
  uint32_t sealed = 0;
  uint32_t confirmed = 0;
  for (uint16_t i = 0; i < this->parent_->collection_count(); i++) {
    ChunkEntry entry{};
    if (!this->parent_->collection_entry_at(i, &entry))
      break;
    if (entry.state == ChunkState::SEALED)
      sealed++;
    else if (entry.state == ChunkState::CONFIRMED)
      confirmed++;
  }
  uint32_t oldest = 0;
  // 517 rendered bytes worst case: the established fields plus the capacity verdict and five ten-digit loss-pipeline
  // counters. Keep headroom so an additive status field cannot silently turn this route into 500.
  char body[560];
  const StatusFields fields{
      this->device_,
      sealed,
      confirmed,
      this->parent_->collection_card_percent(),
      this->parent_->collection_discarded_chunks(),
      this->parent_->collection_discarded_bytes(),
      this->parent_->collection_oldest_uncollected(&oldest),
      oldest,
      this->parent_->get_index_refused(),
      this->parent_->is_mounted(),
      this->parent_->is_degraded(),
      this->parent_->capacity_status(),
      this->parent_->get_records_written(),
      this->parent_->get_dropped_records(),
      this->parent_->get_card_dropped_records(),
      this->parent_->get_write_lost_records(),
      this->parent_->get_tap_shutdown_lost_records(),
      this->parent_->get_tap_accepted_records(),
      this->parent_->get_tap_drained_records(),
      this->parent_->get_tap_record_ring_accepted_records(),
  };
  const int n = format_status_json(body, sizeof(body), fields);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(body))
    return reply_json(req, "500 Internal Server Error", "{\"error\":\"status\"}");
  return reply_json(req, "200 OK", body);
}

// ----------------------------------------------------------------------------- GET /sdlog/fsdebug

esp_err_t CollectionServer::serve_fsdebug_(httpd_req_t *req) {
  char query[SD_LOG_NAME_LEN + 6];  // "name=" plus an 8.3 chunk name and NUL
  char name[SD_LOG_NAME_LEN];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
    return reply_json(req, "404 Not Found", "{\"error\":\"name is required\"}");

  uint32_t seq = 0;
  ChunkState ignored_state = ChunkState::NONE;
  if (!parse_chunk_name(name, &seq, &ignored_state))
    return reply_json(req, "404 Not Found", "{\"error\":\"invalid chunk name\"}");

  const SdLogger::FsDebugResult result = this->parent_->collection_fs_debug(name, this->block_, SD_LOG_SERVE_BLOCK);
  if (result == SdLogger::FsDebugResult::NOT_INDEXED)
    return reply_json(req, "404 Not Found", this->block_);
  if (result != SdLogger::FsDebugResult::OK)
    return reply_json(req, "503 Service Unavailable", this->block_);
  return reply_json(req, "200 OK", this->block_);
}

// ----------------------------------------------------------------------------- GET /sdlog/raw

esp_err_t CollectionServer::serve_raw_(httpd_req_t *req) {
  char query[64];
  RawSectorRequest request{};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK || !parse_raw_sector_query(query, &request))
    return reply_json(req, STATUS_BAD_REQUEST, "{\"error\":\"lba uint32 and count 1..128 are required\"}");

  char error[96] = {};
  const SdLogger::RawReadResult begin =
      this->parent_->collection_begin_raw_read(request.lba, request.count, error, sizeof(error));
  if (begin == SdLogger::RawReadResult::INVALID)
    return reply_json(req, STATUS_BAD_REQUEST, "{\"error\":\"raw LBA range is outside the card\"}");
  if (begin != SdLogger::RawReadResult::OK) {
    snprintf(this->block_, SD_LOG_SERVE_BLOCK, "{\"error\":\"%s\"}", error);
    return reply_json(req, "503 Service Unavailable", this->block_);
  }
  RawReadGuard guard(this->parent_);

  httpd_resp_set_type(req, "application/octet-stream");
  for (uint32_t i = 0; i < request.count; i++) {
    if (!this->parent_->collection_raw_read_sector(request.lba + i, reinterpret_cast<uint8_t *>(this->block_), error,
                                                   sizeof(error)))
      return ESP_FAIL;
    if (httpd_resp_send_chunk(req, this->block_, SD_LOGGER_CARD_SECTOR_BYTES) != ESP_OK)
      return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// ----------------------------------------------------------------------------- GET /sdlog/chain

esp_err_t CollectionServer::serve_chain_(httpd_req_t *req) {
  char query[80];
  ChainPageRequest page{};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK || !parse_chain_page_query(query, &page))
    return reply_json(req, STATUS_BAD_REQUEST, "{\"error\":\"name is required; from defaults to 0; count is 1..20\"}");

  uint32_t seq = 0;
  ChunkState ignored_state = ChunkState::NONE;
  if (!parse_chunk_name(page.name, &seq, &ignored_state))
    return reply_json(req, STATUS_BAD_REQUEST, "{\"error\":\"invalid chunk name\"}");

  const SdLogger::FsDebugResult result =
      this->parent_->collection_chain_debug(page.name, page.from, page.count, this->block_, SD_LOG_SERVE_BLOCK);
  if (result == SdLogger::FsDebugResult::NOT_INDEXED)
    return reply_json(req, "404 Not Found", this->block_);
  if (result != SdLogger::FsDebugResult::OK)
    return reply_json(req, "503 Service Unavailable", this->block_);
  return reply_json(req, "200 OK", this->block_);
}

// ----------------------------------------------------------------------------- GET /sdlog/spitrace

esp_err_t CollectionServer::serve_spitrace_(httpd_req_t *req) {
  bool clear = false;
  char query[16];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char value[2];
    if (httpd_query_key_value(query, "clear", value, sizeof(value)) == ESP_OK) {
      if (std::strcmp(value, "1") != 0)
        return reply_json(req, STATUS_BAD_REQUEST, "{\"error\":\"clear must be 1\"}");
      clear = true;
    }
  }

  SdSpiTraceRing &trace = this->parent_->collection_sdspi_trace();
  const uint32_t write_start = trace.write_index();
  const uint32_t oldest = trace.oldest(write_start);
  httpd_resp_set_type(req, "text/csv");
  int n =
      snprintf(this->block_, SD_LOG_SERVE_BLOCK,
               "#write_index_start,%" PRIu32 ",dropped,%" PRIu32 "\nseq,us,task,opcode,arg,blklen,datalen,flags,err\n",
               write_start, trace.dropped());
  if (n < 0 || static_cast<size_t>(n) >= SD_LOG_SERVE_BLOCK || httpd_resp_send_chunk(req, this->block_, n) != ESP_OK)
    return ESP_FAIL;

  for (uint32_t seq = oldest; seq != write_start; seq++) {
    const SdSpiTraceEntry entry = trace.read(seq);
    char task[sizeof(entry.task)];
    std::memcpy(task, entry.task, sizeof(task));
    task[sizeof(task) - 1] = '\0';
    for (size_t i = 0; task[i] != '\0'; i++) {
      if (task[i] == ',' || task[i] == '\r' || task[i] == '\n' || task[i] == '"')
        task[i] = '_';
    }
    n = snprintf(this->block_, SD_LOG_SERVE_BLOCK,
                 "%" PRIu32 ",%" PRId64 ",%s,%" PRId32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRId32 "\n",
                 entry.seq, entry.us, task, entry.opcode, entry.arg, entry.blklen, entry.datalen, entry.flags,
                 entry.err);
    if (n < 0 || static_cast<size_t>(n) >= SD_LOG_SERVE_BLOCK || httpd_resp_send_chunk(req, this->block_, n) != ESP_OK)
      return ESP_FAIL;
  }
  const uint32_t write_end = trace.write_index();
  n = snprintf(this->block_, SD_LOG_SERVE_BLOCK, "#write_index_end,%" PRIu32 ",dropped,%" PRIu32 "\n", write_end,
               trace.dropped());
  if (n < 0 || static_cast<size_t>(n) >= SD_LOG_SERVE_BLOCK || httpd_resp_send_chunk(req, this->block_, n) != ESP_OK)
    return ESP_FAIL;
  if (clear)
    trace.clear();
  return httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace sd_logger
}  // namespace esphome

#endif  // USE_SD_LOGGER_COLLECTION_SERVER
