// The two tasks that share sd_logger's chunk index: the writer task, which owns the card
// (docs/sdlog-collection-design.md §8), and the collection server's httpd task, which M6 Phase B
// added on top of it (§8a). Everything in this file needs a *second thread* to say anything at all.
//
// CLAUDE.md's definition of done, rule 5: "Concurrency contracts (seqlocks, the slot tracker's
// driver-FIFO mirror) get a case with a real thread, not a reasoned argument: that is how the
// `SnapshotRing` reader fence was found." The collection server is the second component to add a
// task to a data structure that had exactly one mutator, and all four hazards it introduced are
// invisible to a single-threaded case, to `esphome compile`, and to a bench run — a card that
// unmounts under an open read fd, a retention pass that unlinks a chunk a transfer is reading, a
// confirm intent dropped between two producers. None of them leave anything behind but a truncated
// download or a file nobody lists again.
//
// **What is real code here and what is a model.** Two cases drive the real `CollectionPolicy` from
// components/sd_logger/collection_policy.h under two real threads and assert over its actual
// behaviour: the retention case and the index-walk case. The other two model an IDF-bound contract,
// because the code that carries it — `esp_vfs_fat_sdcard_unmount()`, the FreeRTOS queue behind
// `confirm_q_`, and `esp_http_server` — cannot be linked on a laptop. Those cases reproduce the
// *lock scoping and the state machine* of sd_logger.cpp verbatim and assert the invariant over that;
// they are an honest model of the contract, and they do not exercise the firmware. Each says so in
// its own comment, and each names the one-line mutation that makes it go red, so the next reader can
// re-verify that it can fail rather than trusting that it once did.
//
// Sanitizers: ASan + UBSan, like the rest of this target, and deliberately not TSAN — the same
// reason test_snapshot.cpp gives. The models therefore *detect* a dangling access (a generation
// stamp that moved under the reader) rather than performing one: a real use-after-free would abort
// the whole binary inside ASan instead of naming the case that failed.

#include "harness.h"
#include "collection_policy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

using esphome::sd_logger::ChunkEntry;
using esphome::sd_logger::ChunkState;
using esphome::sd_logger::CollectionPolicy;

namespace {

/// 4 MB — the chunk size §4 targets. Only the bookkeeping matters here; nothing writes bytes.
static const uint64_t CHUNK = 4ull * 1024 * 1024;

/// "No sequence number": the initial value of every seq the confirm drain peeks into, so a peek that
/// silently failed could not be mistaken for chunk 0.
static const uint32_t NO_SEQ = 0xFFFFFFFFu;

volatile uint32_t g_spin_sink = 0;

/// A short busy delay standing in for card I/O or a socket write.
///
/// Spun rather than slept, and that is not a style choice: the windows these cases race are a few
/// microseconds wide, while a `sleep_for(20us)` costs ~100 µs of scheduler on a laptop — the
/// iteration counts below would then take minutes instead of milliseconds, and every thread would
/// spend the run parked rather than interleaved. Spinning also keeps both threads on-CPU, which is
/// what actually makes the two-instruction windows collide.
void spin(uint32_t rounds) {
  for (uint32_t i = 0; i < rounds; i++)
    g_spin_sink = g_spin_sink + 1;
}

// =================================================================================================
// 1. The serving gate closes before the unmount drain (BUG 1)
// =================================================================================================

/// Stand-in for the mounted card's VFS context. `esp_vfs_fat_sdcard_unmount()` frees the object that
/// owns every open `FIL`, so a read fd that outlives the unmount is a use-after-free on its next
/// block — and the run that gets there is exactly the interesting one, a card that failed
/// mid-transfer.
struct MountedCard {
  uint32_t magic;
};

/// The shared state of `unmount_card_()` (sd_logger.cpp) versus `collection_begin_serve()` +
/// `serve_chunk_()` (collection_server.cpp), with the lock scoping copied and nothing else.
struct MountRig {
  std::mutex index_mux;  ///< == SdLogger::index_mux_, taken by IndexLock
  CollectionPolicy policy;
  /// == the fix's `SdLogger::serving_open_`. Guarded by `index_mux`, never atomic: the whole point
  /// is that it is read in the *same* critical section as the `find()`/`mark_serving()` that grants
  /// a transfer, so a grant cannot slip between the check and the mark.
  bool serving_open{true};
  /// The mounted card's identity, and the thing the model checks instead of dereferencing freed
  /// memory. 0 means "nothing is mounted". Monotone, so a remount that the allocator happens to
  /// place at the old address is still recognised as a different card.
  std::atomic<uint64_t> live_gen{1};
  std::atomic<MountedCard *> card{nullptr};
  std::atomic<uint32_t> in_flight{0};  ///< == SdLogger::transfers_in_flight_
  std::atomic<bool> stop{false};

  std::atomic<uint32_t> grants{0};
  std::atomic<uint32_t> refused_by_gate{0};
  std::atomic<uint32_t> open_after_free{0};
  std::atomic<uint32_t> read_after_free{0};
  std::atomic<uint32_t> unmounts{0};
  std::atomic<uint32_t> drain_timeouts{0};
};

/// The httpd task: `collection_begin_serve()` -> `::open()` -> read blocks -> `~ServingGuard`.
void mount_rig_httpd(MountRig &rig, uint32_t seq) {
  while (!rig.stop.load(std::memory_order_relaxed)) {
    bool granted = false;
    {
      std::lock_guard<std::mutex> lock(rig.index_mux);
      // THE GATE. Removing this one line is the bug: the drain below can then read zero in-flight
      // transfers and start tearing the card down while this handler is on its way to ::open().
      if (rig.serving_open) {
        const ChunkEntry *entry = rig.policy.find(seq);
        granted = entry != nullptr && entry->state == ChunkState::SEALED;
        if (granted) {
          rig.policy.mark_serving(seq);
          rig.in_flight.fetch_add(1, std::memory_order_release);
        }
      } else {
        rig.refused_by_gate.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!granted) {
      spin(200);
      continue;
    }
    rig.grants.fetch_add(1, std::memory_order_relaxed);
    // --- everything below runs with no lock held, exactly like serve_chunk_() ---
    // const int fd = ::open(path, O_RDONLY): the fd's FIL lives inside the context mounted *now*.
    const uint64_t opened_against = rig.live_gen.load(std::memory_order_acquire);
    if (opened_against == 0)
      rig.open_after_free.fetch_add(1, std::memory_order_relaxed);
    spin(400);  // ::read(fd, block_, want) + httpd_resp_send_chunk(), several times over
    if (opened_against != 0 && rig.live_gen.load(std::memory_order_acquire) != opened_against)
      rig.read_after_free.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(rig.index_mux);
      rig.policy.clear_serving(seq);
    }
    rig.in_flight.fetch_sub(1, std::memory_order_release);
  }
}

/// The writer task: `unmount_card_()` and the `mount_card_()` that follows a recovery.
///
/// Runs `cycles` unmounts, and keeps going until the httpd side has actually been granted
/// `min_grants` transfers *and* been refused `min_refusals` times by the gate — a bound in
/// iterations alone is a bound on *this* thread's progress, and on a loaded box (measured: 32
/// concurrent copies of this binary on a 14-core host) the handler can be starved through all of
/// them, leaving the case green over a run that raced nothing.
void mount_rig_writer(MountRig &rig, uint32_t seq, uint32_t cycles, uint32_t min_grants, uint32_t min_refusals) {
  // Only so a starved handler cannot hang the suite: reaching it is a broken environment, and the
  // non-vacuity checks in the case then say so rather than the run hanging.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  for (uint32_t cycle = 0; cycle < cycles || ((rig.grants.load(std::memory_order_relaxed) < min_grants ||
                                               rig.refused_by_gate.load(std::memory_order_relaxed) < min_refusals) &&
                                              std::chrono::steady_clock::now() < deadline);
       cycle++) {
    // Close the gate FIRST, under the lock, and only then drain. The order is the whole fix: a
    // drain that reads zero is only meaningful if nothing can be granted afterwards.
    {
      std::lock_guard<std::mutex> lock(rig.index_mux);
      rig.serving_open = false;
    }
    // The bounded drain of sd_logger.cpp, at its real 500 ms bound and yielding rather than spinning.
    // A transfer here is microseconds long, so the bound expiring means the handler thread was
    // descheduled for half a second — and a timeout is the one case where the firmware tears down
    // anyway, which would make the assertions below inconclusive rather than wrong. Bounded in wall
    // clock rather than in iterations for that reason: a spin count measures this thread's progress,
    // not the other one's.
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (rig.in_flight.load(std::memory_order_acquire) != 0) {
      if (std::chrono::steady_clock::now() > drain_deadline) {
        rig.drain_timeouts.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      std::this_thread::yield();
    }
    // The teardown is not instantaneous: close(fd_), drop_unflushed_gap_(), block_.reset() and then
    // esp_vfs_fat_sdcard_unmount()'s own f_mount(NULL) plus host deinit all run first. A handler
    // that passed the drain check opens its fd inside exactly this window.
    spin(3000);
    rig.live_gen.store(0, std::memory_order_release);
    delete rig.card.exchange(nullptr, std::memory_order_acq_rel);
    rig.unmounts.fetch_add(1, std::memory_order_relaxed);
    spin(500);
    // mount_card_() + scan_next_seq_(): the card is back and the index is rebuilt from it. The gate
    // reopens last, in the same critical section that publishes the rebuilt index.
    rig.card.store(new MountedCard{0xC0FFEEu}, std::memory_order_release);
    rig.live_gen.store(static_cast<uint64_t>(cycle) + 2u, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(rig.index_mux);
      rig.policy.clear();
      rig.policy.add(seq, CHUNK, ChunkState::OPEN);
      rig.policy.seal(seq, CHUNK);
      rig.serving_open = true;
    }
    spin(6000);  // the card is up and serving; this is where transfers get granted
  }
  rig.stop.store(true, std::memory_order_relaxed);
}

// =================================================================================================
// 3. The confirm queue drops nothing (BUG 4)
// =================================================================================================

/// Stand-in for `confirm_q_`: `xQueueSend()`, `xQueuePeek()`, `xQueueReceive()` and
/// `xQueueSendToFront()` at a fixed depth with a zero timeout, which is how sd_logger.cpp uses them.
/// The IDF queue cannot be linked on the host; what matters for the contract is reproduced exactly —
/// the depth is finite, the calls never block, and a *second producer* can fill the queue between
/// the consumer's receive and its next send.
class FakeQueue {
 public:
  explicit FakeQueue(size_t depth) : depth_(depth) {}

  /// xQueueSend(q, &seq, 0) — to the back, `false` when full.
  bool send(uint32_t seq) {
    std::lock_guard<std::mutex> lock(this->mux_);
    if (this->items_.size() >= this->depth_)
      return false;
    this->items_.push_back(seq);
    return true;
  }

  /// xQueueSendToFront(q, &seq, 0) — to the head, `false` when full. Kept because its failure mode is
  /// the bug: the requeue this drain no longer performs treated it as "cannot happen — a slot was
  /// just freed by the receive above", which stops being true the moment a second task produces into
  /// the same queue.
  bool send_to_front(uint32_t seq) {
    std::lock_guard<std::mutex> lock(this->mux_);
    if (this->items_.size() >= this->depth_)
      return false;
    this->items_.push_front(seq);
    return true;
  }

  /// xQueuePeek(q, &seq, 0) — reads the head and leaves it there.
  bool peek(uint32_t *seq_out) const {
    std::lock_guard<std::mutex> lock(this->mux_);
    if (this->items_.empty())
      return false;
    *seq_out = this->items_.front();
    return true;
  }

  /// xQueueReceive(q, &seq, 0).
  bool receive(uint32_t *seq_out) {
    std::lock_guard<std::mutex> lock(this->mux_);
    if (this->items_.empty())
      return false;
    *seq_out = this->items_.front();
    this->items_.pop_front();
    return true;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(this->mux_);
    return this->items_.empty();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(this->mux_);
    return this->items_.size();
  }

 private:
  mutable std::mutex mux_;
  std::deque<uint32_t> items_;
  size_t depth_;
};

/// The on-card state of one chunk file, which is what a lost confirm intent contradicts.
enum : uint8_t {
  FILE_NONE = 0,
  FILE_LOG = 1,  ///< L#######.LOG — sealed, not yet renamed
  FILE_UPL = 2,  ///< L#######.UPL — the confirm's rename landed
};

/// Chunks per round. Below `SD_LOG_MAX_CHUNKS` so `add()` never refuses — a refused add would make
/// the round quietly smaller instead of failing, and the capacity behaviour is pinned elsewhere
/// (test_collection_capacity.cpp).
static const uint32_t CONFIRM_CHUNKS = SD_LOG_MAX_CHUNKS > 240 ? 240u : static_cast<uint32_t>(SD_LOG_MAX_CHUNKS - 1);

struct ConfirmRig {
  std::mutex index_mux;
  CollectionPolicy policy;
  FakeQueue queue{4};  ///< == SD_LOGGER_CONFIRM_QUEUE_DEPTH
  std::atomic<uint8_t> files[CONFIRM_CHUNKS + 1];

  std::atomic<uint32_t> accepted{0};   ///< intents the handler took responsibility for
  std::atomic<uint32_t> renamed{0};    ///< intents the writer carried out
  std::atomic<uint32_t> busy{0};       ///< queue full: a 503, no index change, the puller retries
  std::atomic<uint32_t> deferrals{0};  ///< the writer postponed a confirm because a transfer held it
};

/// A puller: `GET /sdlog/f/<name>` and then `POST /sdlog/done/<name>`, on the httpd task.
///
/// Each puller owns a disjoint half of the sequence space. Two pullers confirming the *same* chunk
/// would collide on `ChunkEntry::serving`, which is a flag and not a refcount — a real limitation of
/// the component, but not the one this case is about, and letting it in would make the deferral
/// counter meaningless.
void confirm_rig_puller(ConfirmRig &rig, uint32_t phase) {
  for (uint32_t seq = 1 + phase; seq <= CONFIRM_CHUNKS; seq += 2) {
    // collection_begin_serve(): refused while the flag is set, because the writer reserves with that
    // same flag across the rename below. A 404 is what the client sees; here it is a retry.
    for (;;) {
      bool granted = false;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        const ChunkEntry *entry = rig.policy.find(seq);
        if (entry != nullptr && entry->state != ChunkState::OPEN && !entry->serving)
          granted = rig.policy.mark_serving(seq);
      }
      if (granted)
        break;
      std::this_thread::yield();
    }
    spin(100);  // the transfer itself
    // collection_confirm(): the intent is queued *before* the state flips (sd_logger.cpp), because
    // an index that says CONFIRMED with no rename queued leaves a `.LOG` file that nothing lists,
    // serves or reclaims. That order is also why a dropped intent is the failure it is.
    for (;;) {
      bool posted = false;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        const ChunkEntry *entry = rig.policy.find(seq);
        if (entry == nullptr || entry->state == ChunkState::OPEN)
          break;
        if (entry->state == ChunkState::CONFIRMED) {
          posted = true;  // idempotent: the rename is queued or already done
        } else if (rig.queue.send(seq)) {
          rig.policy.confirm(seq);
          rig.accepted.fetch_add(1, std::memory_order_relaxed);
          posted = true;
        } else {
          rig.busy.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (posted)
        break;
      std::this_thread::yield();
    }
    spin(200);  // the handler is still finishing while the writer looks at the queue
    {           // ~ServingGuard -> collection_end_serve()
      std::lock_guard<std::mutex> lock(rig.index_mux);
      rig.policy.clear_serving(seq);
    }
  }
}

/// The writer task's `drain_confirms_()`, in the **peek, decide, then consume** shape sd_logger.cpp
/// carries: the intent stays in the queue until this task has committed to executing it, so the
/// deferral needs no re-queue at all. Safe because the writer is the queue's only consumer —
/// producers only ever append, so nothing can take the head between the peek and the receive.
///
/// The shape it replaces received first and pushed the intent back with `xQueueSendToFront()`, on
/// the reasoning that the receive had just freed a slot. That slot is not the writer's: the httpd
/// task produces into the same queue, and a full queue turns the requeue into a *dropped intent* —
/// an index entry left CONFIRMED over a file still named `.LOG`, which under
/// `collection: {enabled: false, serve: true}` is never listed, served or reclaimed again.
void confirm_rig_writer(ConfirmRig &rig, uint32_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  for (;;) {
    if (rig.accepted.load(std::memory_order_relaxed) == expected && rig.queue.empty())
      return;
    if (std::chrono::steady_clock::now() > deadline)
      return;  // a dropped intent shows up here, as a round that never finishes draining
    uint32_t seq = NO_SEQ;
    while (rig.queue.peek(&seq)) {  // one drain_confirms_() pass
      bool reserved = false;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        if (rig.policy.is_serving(seq)) {
          // Not while a handler still holds a read fd on the file: whether an open FATFS `FIL`
          // survives a rename of its directory entry is not verified on this platform. The intent is
          // still at the head of the queue; the rest waits with it rather than being reordered.
          rig.deferrals.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        // Claim it with the flag `collection_begin_serve()` refuses on, and hold that across the
        // rename below — the index lock is given back for the card I/O.
        reserved = rig.policy.mark_serving(seq);
      }
      uint32_t taken = NO_SEQ;
      if (!rig.queue.receive(&taken)) {  // cannot fail: single consumer. Bounded anyway.
        if (reserved) {
          std::lock_guard<std::mutex> lock(rig.index_mux);
          rig.policy.clear_serving(seq);
        }
        break;
      }
      rig.files[seq].store(FILE_UPL, std::memory_order_release);  // ::rename(.LOG -> .UPL)
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        if (reserved)
          rig.policy.clear_serving(seq);
        rig.policy.confirm(seq);  // idempotent; the handler already did it
      }
      rig.renamed.fetch_add(1, std::memory_order_relaxed);
    }
    spin(100);  // the writer pass is 20 ms apart on the device
  }
}

}  // namespace

// -------------------------------------------------------------------------------------------------
// 1. BUG 1 — nothing may be granted after the drain reads zero
// -------------------------------------------------------------------------------------------------

TEST(sdlog_serving_gate_refuses_every_transfer_while_the_card_is_down) {
  // The single-threaded half of the contract, so the threaded case below cannot pass by never
  // granting anything at all: the gate must refuse a chunk that is otherwise perfectly servable.
  MountRig rig;
  rig.policy.configure(50);
  rig.policy.add(7, CHUNK, ChunkState::OPEN);
  rig.policy.seal(7, CHUNK);

  const ChunkEntry *entry = rig.policy.find(7);
  CHECK(entry != nullptr);
  CHECK_EQ(entry->state, ChunkState::SEALED);  // servable by every rule except the gate

  rig.serving_open = false;
  bool granted = false;
  if (rig.serving_open) {
    granted = rig.policy.is_servable(7);
    if (granted)
      rig.policy.mark_serving(7);
  }
  CHECK_EQ(granted, false);
  CHECK_EQ(rig.policy.is_serving(7), false);  // and no flag was left behind to pin the chunk

  rig.serving_open = true;
  CHECK_EQ(rig.policy.is_servable(7), true);
}

TEST(sdlog_no_transfer_is_granted_after_the_unmount_drain) {
  // BUG 1, as a real race. `unmount_card_()` drains `transfers_in_flight_` to zero and then frees the
  // VFS context — but the drain reading zero says nothing about the *next* transfer, and the teardown
  // between that read and the free is milliseconds of card and host work. A handler that passed
  // `collection_begin_serve()` in that window opens an fd against a context that is about to be freed
  // and reads from it afterwards. The reviewer's throwaway measured 243 such reads and 413 such opens
  // over 2372 unmounts with the drain-wait timeout at ZERO — i.e. entirely inside the "clean" path.
  //
  // MODEL, not firmware: `esp_vfs_fat_sdcard_unmount()` and `esp_http_server` cannot be linked on the
  // host. What is reproduced verbatim is the lock scoping and the order of operations of
  // `unmount_card_()` and `collection_begin_serve()`/`serve_chunk_()`; what is asserted is the
  // invariant the fix owes: with the gate closed under `index_mux_` *before* the drain, no transfer
  // is granted afterwards, so no access can land on the freed context.
  //
  // Break it by deleting the `if (rig.serving_open)` check in mount_rig_httpd(): 5215 fds opened on
  // an unmounted card and 657 reads whose card was freed under them, measured over 898 unmounts.
  MountRig rig;
  const uint32_t seq = 433;
  rig.card.store(new MountedCard{0xC0FFEEu});
  rig.policy.configure(50);
  rig.policy.add(seq, CHUNK, ChunkState::OPEN);
  rig.policy.seal(seq, CHUNK);

  std::thread writer(mount_rig_writer, std::ref(rig), seq, /*cycles=*/600u, /*min_grants=*/200u,
                     /*min_refusals=*/20u);
  std::thread httpd(mount_rig_httpd, std::ref(rig), seq);
  writer.join();
  httpd.join();
  delete rig.card.exchange(nullptr);

  CHECK_EQ_MSG(rig.open_after_free.load(), 0u, "a transfer opened its fd on an unmounted card");
  CHECK_EQ_MSG(rig.read_after_free.load(), 0u, "the card a transfer's fd lived in was freed under it");
  // A drain timeout is the one exposure the firmware documents and accepts (it tears down anyway), so
  // it would make the two counters above inconclusive rather than reassuring. It must not be how this
  // run stayed clean.
  CHECK_EQ_MSG(rig.drain_timeouts.load(), 0u, "the drain bound expired — the run proves nothing");
  // Non-vacuity: the race needs both sides to have actually run. The writer keeps unmounting until
  // the handler has been granted its 200, so falling short here means the handler thread never ran
  // for 8 seconds — a broken host, not a passing test.
  CHECK_MSG(rig.grants.load() >= 200u, "the handler was never granted a transfer");
  CHECK_MSG(rig.refused_by_gate.load() >= 20u, "the gate never refused anything — the window never opened");
  CHECK_MSG(rig.unmounts.load() >= 600u, "the writer did not complete its unmount cycles");
  std::printf("           (%u unmounts, %u transfers granted, %u refused by the gate, %u after-free accesses)\n",
              rig.unmounts.load(), rig.grants.load(), rig.refused_by_gate.load(),
              rig.open_after_free.load() + rig.read_after_free.load());
}

// -------------------------------------------------------------------------------------------------
// 2. BUG 2 — retention must not unlink a chunk a reader is holding
// -------------------------------------------------------------------------------------------------

TEST(sdlog_retention_never_unlinks_a_chunk_a_reader_holds) {
  // BUG 2, over the REAL `CollectionPolicy`. `retention_pass_()` chose its victim under the index
  // lock, gave the lock back, and unlinked the file outside it — so `mark_serving()` could land in
  // the window and the unlink took a file a transfer had open. The reviewer's throwaway measured
  // 10 292 unlinks under a live reader and 10 323 discards refused *after* the file was already
  // gone, each of which leaves an index entry naming a deleted file: served forever as a 500,
  // chosen as the victim on every later pass, and never reclaimed.
  //
  // Asserted as the two OBSERVABLE invariants rather than as a mechanism:
  //
  //   (a) no unlink happens while a reader holds that chunk;
  //   (b) `discard()` is never refused after its file was unlinked.
  //
  // The transaction below reserves with the existing `serving` flag — which is what sd_logger.cpp
  // does, and the variant that costs no byte in the 12-byte `ChunkEntry` — but the assertions do not
  // depend on that choice: with a separate `condemned` flag the grant condition reads
  // `!entry->condemned` and nothing else in this case changes. What it pins about the real header
  // either way: `next_victim()` skips a chunk marked serving and `discard()` refuses one. Remove
  // either and this case goes red.
  //
  // Break it by deleting the `rig.policy.mark_serving(seq)` reservation from the writer's critical
  // section: 340 unlinks under a live reader and 995 reads of an already-unlinked file, measured.
  struct FakeFile {
    std::atomic<uint32_t> readers{0};
    std::atomic<bool> gone{false};
  };
  struct RetentionRig {
    std::mutex index_mux;
    CollectionPolicy policy;
    FakeFile files[8];
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> serves{0};
    std::atomic<uint32_t> unlinks{0};
    std::atomic<uint32_t> unlink_under_reader{0};
    std::atomic<uint32_t> read_after_unlink{0};
    std::atomic<uint32_t> refused_discards{0};
  };
  static const uint32_t SEQ_LO = 1;
  static const uint32_t SEQ_HI = 6;

  RetentionRig rig;
  rig.policy.configure(50);
  rig.policy.configure_rotation(static_cast<uint32_t>(CHUNK), 60);
  for (uint32_t seq = SEQ_LO; seq <= SEQ_HI; seq++) {
    rig.policy.add(seq, CHUNK, ChunkState::OPEN);
    rig.policy.seal(seq, CHUNK);
  }

  // The httpd task: collection_begin_serve() -> ::open() -> read blocks -> collection_end_serve().
  std::thread httpd([&rig]() {
    while (!rig.stop.load(std::memory_order_relaxed)) {
      for (uint32_t seq = SEQ_LO; seq <= SEQ_HI; seq++) {
        bool granted = false;
        {
          std::lock_guard<std::mutex> lock(rig.index_mux);
          const ChunkEntry *entry = rig.policy.find(seq);
          // `!entry->serving` is the reservation half: a chunk retention has claimed is not servable.
          granted = entry != nullptr && entry->state == ChunkState::SEALED && !entry->serving;
          if (granted)
            rig.policy.mark_serving(seq);
        }
        if (!granted)
          continue;
        rig.serves.fetch_add(1, std::memory_order_relaxed);
        rig.files[seq].readers.fetch_add(1, std::memory_order_acq_rel);
        if (rig.files[seq].gone.load(std::memory_order_acquire))
          rig.read_after_unlink.fetch_add(1, std::memory_order_relaxed);  // opened an unlinked file
        spin(300);                                                        // ::read() + httpd_send()
        if (rig.files[seq].gone.load(std::memory_order_acquire))
          rig.read_after_unlink.fetch_add(1, std::memory_order_relaxed);  // unlinked while we held it
        rig.files[seq].readers.fetch_sub(1, std::memory_order_acq_rel);
        {
          std::lock_guard<std::mutex> lock(rig.index_mux);
          rig.policy.clear_serving(seq);
        }
      }
    }
  });

  // The writer task: retention_pass_(), with choose-and-reserve as ONE critical section.
  //
  // 4000 unlinks, and then as many more as it takes for the httpd side to have been granted 200
  // transfers: a pass count bounds this thread's progress, not the other one's, and a starved
  // handler would otherwise leave the case green over a run with nothing to race. The wall-clock
  // deadline is only so a handler that never runs at all cannot hang the suite.
  std::thread writer([&rig]() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (uint32_t pass = 0; pass < 4000 || (rig.serves.load(std::memory_order_relaxed) < 200u &&
                                            std::chrono::steady_clock::now() < deadline);
         pass++) {
      uint32_t seq = 0;
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        const ChunkEntry *victim = rig.policy.next_victim(/*fill_percent=*/100);
        if (victim != nullptr) {
          seq = victim->seq;
          have = true;
          // The reservation, in the same critical section that chose the victim. Without it the
          // unlink below races every `collection_begin_serve()` on the httpd task.
          rig.policy.mark_serving(seq);
        }
      }
      if (!have) {
        std::this_thread::yield();
        continue;
      }
      // The unlink is card I/O and runs with the lock given back — that has not changed, and does
      // not need to: the entry is reserved.
      if (rig.files[seq].readers.load(std::memory_order_acquire) != 0)
        rig.unlink_under_reader.fetch_add(1, std::memory_order_relaxed);
      rig.files[seq].gone.store(true, std::memory_order_release);
      rig.unlinks.fetch_add(1, std::memory_order_relaxed);
      spin(200);  // the SPI unlink itself
      if (rig.files[seq].readers.load(std::memory_order_acquire) != 0)
        rig.unlink_under_reader.fetch_add(1, std::memory_order_relaxed);
      bool discarded = false;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        rig.policy.clear_serving(seq);  // release the reservation, then drop the entry
        discarded = rig.policy.discard(seq);
        if (discarded) {
          // The writer re-seals a fresh chunk in that slot so the run keeps producing work.
          rig.files[seq].gone.store(false, std::memory_order_release);
          rig.policy.add(seq, CHUNK, ChunkState::OPEN);
          rig.policy.seal(seq, CHUNK);
        }
      }
      if (!discarded)
        rig.refused_discards.fetch_add(1, std::memory_order_relaxed);
    }
    rig.stop.store(true, std::memory_order_relaxed);
  });

  writer.join();
  httpd.join();

  CHECK_EQ_MSG(rig.unlink_under_reader.load(), 0u, "retention unlinked a chunk a transfer had open");
  CHECK_EQ_MSG(rig.read_after_unlink.load(), 0u, "a transfer read a file retention had already unlinked");
  CHECK_EQ_MSG(rig.refused_discards.load(), 0u, "discard() was refused after the file was already gone");
  CHECK_MSG(rig.unlinks.load() >= 4000u, "the writer did not complete its retention passes");
  CHECK_MSG(rig.serves.load() >= 200u, "the httpd side was never granted a transfer");
  std::printf("           (%u unlinks against %u transfers, 0 unlinked under a reader, 0 refused discards)\n",
              rig.unlinks.load(), rig.serves.load());
}

// -------------------------------------------------------------------------------------------------
// 3. BUG 4 — an accepted confirm intent is never dropped
// -------------------------------------------------------------------------------------------------

TEST(sdlog_confirm_queue_models_the_freertos_semantics) {
  // The model the case below rests on. A queue that silently grew, or whose send-to-front could not
  // fail, would make that case pass for the wrong reason — the dropped intent it is about is
  // *exactly* a `xQueueSendToFront()` that returned false on a full queue.
  FakeQueue queue(4);
  CHECK_EQ(queue.empty(), true);
  for (uint32_t seq = 1; seq <= 4; seq++)
    CHECK_EQ_MSG(queue.send(seq), true, "a send below the depth was refused");
  CHECK_EQ_MSG(queue.send(5), false, "the queue accepted a fifth item at depth 4");
  CHECK_EQ_MSG(queue.send_to_front(5), false, "send-to-front succeeded on a full queue");
  CHECK_EQ(queue.size(), 4u);

  uint32_t got = 0;
  // Peek reads the head without consuming it — the whole basis of the drain's "peek, decide, then
  // consume". A peek that consumed, or one that reported a different item than the next receive,
  // would make the case below meaningless.
  CHECK_EQ(queue.peek(&got), true);
  CHECK_EQ(got, 1u);
  CHECK_EQ_MSG(queue.size(), 4u, "peek consumed the head");
  got = 0;
  CHECK_EQ(queue.receive(&got), true);
  CHECK_EQ(got, 1u);                        // FIFO, and the same item the peek reported
  CHECK_EQ(queue.send_to_front(99), true);  // the freed slot takes a head insert
  CHECK_EQ(queue.receive(&got), true);
  CHECK_EQ_MSG(got, 99u, "send-to-front did not jump the queue");
  for (uint32_t expect = 2; expect <= 4; expect++) {
    CHECK_EQ(queue.receive(&got), true);
    CHECK_EQ(got, expect);
  }
  CHECK_EQ(queue.receive(&got), false);
  CHECK_EQ(queue.peek(&got), false);
  CHECK_EQ(queue.empty(), true);
}

TEST(sdlog_confirm_intents_survive_a_second_producer) {
  // BUG 4. `drain_confirms_()` receives a seq, finds the chunk still being served, and pushes the
  // intent back with `xQueueSendToFront()` — "cannot happen" on a full queue, because the receive
  // just freed a slot. It is not the writer's slot: the httpd task produces into the same queue and
  // can refill it in between, and the send then fails. The intent is gone, while the index entry
  // went CONFIRMED at accept time (that order is deliberate and correct). The result is a CONFIRMED
  // entry over a file still named `.LOG`, which retention would eventually reclaim — except under
  // `collection: {enabled: false, serve: true}`, where retention is off and the file is never
  // listed, served or reclaimed again.
  //
  // MODEL, not firmware: FreeRTOS queues do not link on the host. The queue semantics are pinned by
  // the case above, the index is the real `CollectionPolicy`, and the drain reproduces the shape
  // `drain_confirms_()` ships — peek, decide under the index lock, then consume, so a deferral never
  // has to put anything back. What is asserted is the invariant rather than that shape: every
  // accepted intent produces exactly one rename, and no entry is left CONFIRMED over a `.LOG` file.
  //
  // Break it by going back to receive-then-requeue in confirm_rig_writer() — take the head with
  // `receive()` before the `is_serving()` check and push it back with `rig.queue.send_to_front(seq)`
  // on the deferral path, ignoring the result: `renamed` falls short of `accepted` and the
  // CONFIRMED-over-`.LOG` count goes non-zero (measured: 25 of 1440 intents dropped).
  static const uint32_t ROUNDS = 6;
  uint32_t total_accepted = 0;
  uint32_t total_renamed = 0;
  uint32_t total_deferrals = 0;
  uint32_t total_busy = 0;
  uint32_t confirmed_over_log = 0;
  uint32_t sealed_without_file = 0;

  for (uint32_t round = 0; round < ROUNDS; round++) {
    ConfirmRig rig;
    rig.policy.configure(50);
    for (uint32_t seq = 0; seq <= CONFIRM_CHUNKS; seq++)
      rig.files[seq].store(FILE_NONE);
    for (uint32_t seq = 1; seq <= CONFIRM_CHUNKS; seq++) {
      rig.policy.add(seq, CHUNK, ChunkState::OPEN);
      rig.policy.seal(seq, CHUNK);
      rig.files[seq].store(FILE_LOG);
    }

    std::thread puller_a(confirm_rig_puller, std::ref(rig), 0u);
    std::thread puller_b(confirm_rig_puller, std::ref(rig), 1u);
    std::thread writer(confirm_rig_writer, std::ref(rig), CONFIRM_CHUNKS);
    puller_a.join();
    puller_b.join();
    writer.join();

    total_accepted += rig.accepted.load();
    total_renamed += rig.renamed.load();
    total_deferrals += rig.deferrals.load();
    total_busy += rig.busy.load();
    // The index and the card must agree once everything has settled: CONFIRMED means the rename
    // happened, SEALED means it did not. A dropped intent is exactly the first of these going wrong.
    for (uint16_t i = 0; i < rig.policy.count(); i++) {
      const ChunkEntry *entry = rig.policy.at(i);
      if (entry == nullptr)
        continue;
      const uint8_t on_card = rig.files[entry->seq].load();
      if (entry->state == ChunkState::CONFIRMED && on_card != FILE_UPL)
        confirmed_over_log++;
      if (entry->state == ChunkState::SEALED && on_card != FILE_LOG)
        sealed_without_file++;
    }
  }

  CHECK_EQ_MSG(total_accepted, ROUNDS * CONFIRM_CHUNKS, "a puller never got its intent accepted");
  CHECK_EQ_MSG(total_renamed, total_accepted, "an accepted confirm intent was dropped: no rename ever happened");
  CHECK_EQ_MSG(confirmed_over_log, 0u, "an index entry is CONFIRMED while its file is still .LOG");
  CHECK_EQ_MSG(sealed_without_file, 0u, "a SEALED entry names a file that was already renamed");
  // Non-vacuity: the deferral path is the one the bug lives on. A run that never deferred anything
  // never reached the requeue at all and proves nothing.
  CHECK_MSG(total_deferrals > 0u, "the writer never deferred a confirm — the race window never opened");
  std::printf("           (%u intents accepted, %u renamed, %u deferred, %u refused while the queue was full)\n",
              total_accepted, total_renamed, total_deferrals, total_busy);
}

// -------------------------------------------------------------------------------------------------
// 4. The index walk stays coherent (serve_index_)
// -------------------------------------------------------------------------------------------------

TEST(sdlog_index_walk_stays_coherent_under_concurrent_mutation) {
  // `serve_index_()` walks the index one entry at a time, re-reading `count()` and taking the index
  // lock per entry, and gives the lock back to write each line onto the socket. That is deliberate —
  // the alternative is holding `index_mux_` across a socket write, which stalls the writer task for
  // as long as the client's TCP window takes — and it means the walk can *skip* an entry when
  // `discard()` memmoves the array under it. Skips are harmless: the puller polls the index again.
  //
  // Tears are not, and neither are duplicates: a listing line that pairs one chunk's name with
  // another's length sends the puller to fetch a byte count that does not exist, and it looks like a
  // truncated download rather than an index bug. The reviewer measured 1828 concurrent walks with 0
  // torn entries and 0 duplicate seqs; this pins that result over the REAL `CollectionPolicy`,
  // because the property is exactly the kind that regresses silently when someone widens or narrows
  // one of those per-entry lock scopes.
  //
  // Break it by formatting the listing line field by field off the live entry instead of off the
  // locked copy — `copy.seq = entry->seq; spin(100); copy.bytes = entry->bytes;` with the lock given
  // back, which is what `serve_index_()` becomes if someone keeps the pointer: 41 027 torn entries
  // out of 168 787 listed. Merely moving the whole 12-byte `copy = *entry` outside the lock is *not*
  // enough to see it, which is worth knowing before trusting a green run on a narrower mutation.
  //
  // The ordering counter has its own trigger, and it is a real one: replacing `discard()`'s
  // hole-closing memmove with the swap-the-last-entry-in that collection_policy.h explicitly rejects
  // makes the walk list a chunk it had already walked past (verified at 20 000 out of 20 000 walks
  // against a patched copy of the header). The duplicate counter never fired under any mutation
  // tried — no operation on this index moves an entry to a *higher* index — so it is a cheap
  // standing invariant rather than a demonstrated one.
  struct WalkRig {
    std::mutex index_mux;
    CollectionPolicy policy;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> adds{0};
    std::atomic<uint32_t> discards{0};
  };
  static const uint32_t WALKS = 20000;
  /// Safety cap on the mutator, and not an arbitrary one: the fixture below encodes `bytes` as
  /// `seq * 1000`, so a seq past ~4.29 million makes `chunk_bytes()` saturate and every entry then
  /// reads as torn — a false positive that says nothing about the walk. (The first draft had no cap,
  /// ran 10 million mutations in four seconds, walked straight past `SD_LOG_SEQ_MAX` where `add()`
  /// starts refusing, and reported 15012 torn entries out of 16049.)
  static const uint32_t MUTATION_CAP = 2000000;

  WalkRig rig;
  rig.policy.configure(50);

  // The writer task: add() appends, discard() memmoves the array to close the hole. Sequence numbers
  // are never reused, which is what makes a duplicate within one walk a real defect rather than a
  // chunk that legitimately came back.
  std::thread writer([&rig]() {
    uint32_t next = 100;
    {
      // Seeded from this thread so the index is populated before the walker's first pass. Walking an
      // empty index is instantaneous and 2000 of those finish before the mutator is even scheduled —
      // which is how the first draft of this case passed while measuring nothing at all.
      std::lock_guard<std::mutex> lock(rig.index_mux);
      for (uint32_t i = 0; i < 9; i++, next++) {
        if (rig.policy.add(next, static_cast<uint64_t>(next) * 1000u, ChunkState::SEALED))
          rig.adds.fetch_add(1, std::memory_order_relaxed);
      }
    }
    for (uint32_t mutation = 0; mutation < MUTATION_CAP && !rig.stop.load(std::memory_order_relaxed); mutation++) {
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        if (rig.policy.count() > 8) {
          const ChunkEntry *oldest = rig.policy.at(0);
          if (oldest != nullptr && rig.policy.discard(oldest->seq))
            rig.discards.fetch_add(1, std::memory_order_relaxed);
        } else if (rig.policy.add(next, static_cast<uint64_t>(next) * 1000u, ChunkState::SEALED)) {
          rig.adds.fetch_add(1, std::memory_order_relaxed);
          next++;
        }
      }
      spin(150);  // the writer task mutates the index a few times per 20 ms pass, not per nanosecond
    }
  });

  uint32_t torn = 0;
  uint32_t duplicates = 0;
  uint32_t out_of_order = 0;
  uint64_t entries_listed = 0;
  uint32_t walks = 0;
  while (!rig.adds.load(std::memory_order_relaxed))
    std::this_thread::yield();  // the seed above; a walk before it would list nothing
  // Both bounds: enough walks, and enough of the array shifts that make the walk interesting. A
  // walker that outran the mutator would report "0 torn" over an index nothing was moving.
  for (; walks < WALKS || rig.discards.load(std::memory_order_relaxed) < 200u; walks++) {
    uint32_t last_seq = 0;
    for (uint16_t i = 0;; i++) {
      uint16_t n = 0;
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        n = rig.policy.count();
      }
      if (i >= n)
        break;
      ChunkEntry copy{};
      {
        std::lock_guard<std::mutex> lock(rig.index_mux);
        const ChunkEntry *entry = rig.policy.at(i);
        if (entry == nullptr)
          break;
        // A copy, not the pointer: `discard()` memmoves the array the moment this lock is given back.
        copy = *entry;
      }
      // The fixture keeps bytes == seq * 1000, so a name from one chunk carrying another's length is
      // detectable from the listed entry alone.
      if (copy.bytes != copy.seq * 1000u)
        torn++;
      // Seqs only ever increase and the index keeps insertion order, so a walk must see them
      // strictly increasing. Equal is a duplicate; smaller is a reordering.
      if (copy.seq == last_seq)
        duplicates++;
      else if (copy.seq < last_seq)
        out_of_order++;
      last_seq = copy.seq;
      entries_listed++;
      spin(150);  // httpd_resp_send_chunk() onto the socket, with the lock given back
    }
  }
  rig.stop.store(true, std::memory_order_relaxed);
  writer.join();

  CHECK_EQ_MSG(torn, 0u, "the index walk listed an entry mixing two chunks");
  CHECK_EQ_MSG(duplicates, 0u, "the index walk listed the same chunk twice");
  CHECK_EQ_MSG(out_of_order, 0u, "the index walk listed a chunk it had already walked past");
  CHECK_MSG(entries_listed > 1000u, "the walk never listed anything");
  CHECK_MSG(rig.adds.load() > 100u, "the mutator never grew the index");
  CHECK_MSG(rig.discards.load() > 100u, "the mutator never shifted the array — the hazard never arose");
  CHECK_MSG(rig.adds.load() + rig.discards.load() < MUTATION_CAP,
            "the mutator hit its safety cap: seq * 1000 is about to saturate and every entry would read as torn");
  std::printf("           (%u walks, %llu entries listed against %u adds / %u discards, 0 torn, 0 duplicates)\n", walks,
              static_cast<unsigned long long>(entries_listed), rig.adds.load(), rig.discards.load());
}
