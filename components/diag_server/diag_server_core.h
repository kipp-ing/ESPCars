#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace esphome::diag_server {

constexpr uint16_t ANY_SESSION = 0x100;
constexpr uint16_t NO_ENTRY = UINT16_MAX;
constexpr uint8_t DEFAULT_SESSION = 0x01;

struct Entry {
  uint16_t request_offset;
  uint16_t reply_offset;
  uint16_t request_size;
  uint16_t reply_size;
  uint16_t session;
  bool prefix;
};
static_assert(sizeof(Entry) == 12);  // Included in the codegen flash budget.

struct Table {
  const uint8_t *bytes;
  const Entry *entries;
  uint16_t size;
};

enum class Rule : uint8_t {
  IGNORED,
  BUSY,
  DROP,
  ENTRY,
  PRESENT,
  SUPPRESS,
  SESSION,
  WRONG_SESSION,
  OUT_OF_RANGE,
  UNSUPPORTED,
};

struct Decision {
  Rule rule;
  uint16_t entry{NO_ENTRY};
  uint16_t reply_size{0};
  uint32_t delay_ms{0};
  bool pending{false};
};

struct Counters {
  uint32_t requests{0};
  uint32_t replies{0};
  uint32_t misses{0};
};

struct Reply {
  uint16_t size{0};
  uint16_t session{ANY_SESSION};
  bool pending{false};
  bool miss{false};
};

class ServerCore {
 public:
  ServerCore(Table table, uint32_t s3_ms) : table_(table), s3_ms_(s3_ms) {}

  void set_enabled(bool enabled) {
    if (!enabled)
      this->held_size_ = 0;
    this->enabled_ = enabled;
  }
  bool enabled() const { return this->enabled_; }
  void set_delay(uint32_t delay_ms) { this->delay_ms_ = delay_ms; }
  uint32_t delay() const { return this->delay_ms_; }
  void set_pending_78(bool pending) { this->pending_78_ = pending; }
  bool pending_78() const { return this->pending_78_; }
  void set_drop_every(uint32_t every) {
    if (this->drop_every_ != every)
      this->drop_position_ = 0;
    this->drop_every_ = every;
  }
  uint32_t drop_every() const { return this->drop_every_; }
  uint8_t session() const { return this->session_; }
  const Counters &counters() const { return this->counters_; }

  // The caller supplies a buffer at least as long as the largest table or built-in reply.
  // The request is only borrowed for this call. The same reply buffer stays valid until poll()
  // returns the final reply, which the caller must consume before the next request.
  Decision on_request(uint32_t now, const uint8_t *request, uint16_t size, bool transport_busy, uint8_t *buffer) {
    this->expire_session(now);
    if (!this->enabled_ || size == 0)
      return {Rule::IGNORED};

    this->last_request_ms_ = now;
    this->counters_.requests++;
    const bool drop = this->drop_every_ != 0 && ++this->drop_position_ == this->drop_every_;
    if (drop)
      this->drop_position_ = 0;
    if (transport_busy || this->held_size_ != 0) {
      this->counters_.misses++;
      return {Rule::BUSY};
    }
    if (drop) {
      this->counters_.misses++;
      return {Rule::DROP};
    }

    auto decision = this->match_(request, size);
    if (decision.rule == Rule::SUPPRESS)
      return decision;

    const uint8_t sid = request[0];
    this->held_session_ = ANY_SESSION;
    this->held_miss_ = false;
    switch (decision.rule) {
      case Rule::ENTRY: {
        const auto &entry = this->table_.entries[decision.entry];
        std::memcpy(buffer, this->table_.bytes + entry.reply_offset, entry.reply_size);
        decision.reply_size = entry.reply_size;
        if (sid == 0x10 && size == 2 && buffer[0] == 0x50)
          this->held_session_ = request[1];
        break;
      }
      case Rule::PRESENT:
        buffer[0] = 0x7E;
        buffer[1] = 0x00;
        decision.reply_size = 2;
        break;
      case Rule::SESSION: {
        const uint8_t reply[]{0x50, request[1], 0x00, 0x32, 0x01, 0xF4};
        std::memcpy(buffer, reply, sizeof(reply));
        decision.reply_size = sizeof(reply);
        this->held_session_ = request[1];
        break;
      }
      case Rule::WRONG_SESSION:
      case Rule::OUT_OF_RANGE:
      case Rule::UNSUPPORTED:
        buffer[0] = 0x7F;
        buffer[1] = sid;
        buffer[2] = decision.rule == Rule::WRONG_SESSION ? 0x7F : decision.rule == Rule::OUT_OF_RANGE ? 0x31 : 0x11;
        decision.reply_size = 3;
        this->held_miss_ = true;
        break;
      default:
        return decision;
    }

    this->held_size_ = decision.reply_size;
    this->held_sid_ = sid;
    this->pending_due_ = this->pending_78_;
    this->held_delay_ms_ = this->pending_due_ && this->delay_ms_ == 0 ? 100 : this->delay_ms_;
    this->held_since_ms_ = now;
    decision.pending = this->pending_due_;
    decision.delay_ms = this->held_delay_ms_;
    return decision;
  }

  void expire_session(uint32_t now) {
    if (this->session_ != DEFAULT_SESSION && now - this->last_request_ms_ >= this->s3_ms_)
      this->session_ = DEFAULT_SESSION;
  }

  // A pending NRC uses the caller's three-byte scratch buffer. The final reply is in the
  // buffer passed to on_request(). At most one reply is held across loop iterations.
  Reply poll(uint32_t now, bool transport_busy, uint8_t (&pending)[3]) {
    this->expire_session(now);
    if (this->held_size_ == 0 || transport_busy)
      return {};
    if (this->pending_due_) {
      this->pending_due_ = false;
      pending[0] = 0x7F;
      pending[1] = this->held_sid_;
      pending[2] = 0x78;
      this->held_since_ms_ = now;
      return {3, ANY_SESSION, true};
    }
    if (now - this->held_since_ms_ < this->held_delay_ms_)
      return {};
    const Reply reply{this->held_size_, this->held_session_, false, this->held_miss_};
    this->held_size_ = 0;
    return reply;
  }

  void reply_sent(uint32_t now, Reply reply) {
    this->counters_.replies++;
    if (reply.miss)
      this->counters_.misses++;
    if (reply.pending)
      this->held_since_ms_ = now;
    if (reply.session != ANY_SESSION)
      this->session_ = static_cast<uint8_t>(reply.session);
  }

 private:
  Decision for_entry_(uint16_t index) const {
    const auto session = this->table_.entries[index].session;
    return {session == ANY_SESSION || session == this->session_ ? Rule::ENTRY : Rule::WRONG_SESSION, index};
  }

  Decision match_(const uint8_t *request, uint16_t size) const {
    uint16_t prefix = NO_ENTRY;
    uint16_t prefix_size = 0;
    bool known_sid = false;
    for (uint16_t i = 0; i < this->table_.size; i++) {
      const auto &entry = this->table_.entries[i];
      const auto *key = this->table_.bytes + entry.request_offset;
      if (key[0] != request[0])
        continue;
      known_sid = true;
      if (entry.request_size > size || (!entry.prefix && entry.request_size != size) ||
          std::memcmp(key, request, entry.request_size) != 0)
        continue;
      if (!entry.prefix)
        return this->for_entry_(i);
      if (entry.request_size > prefix_size) {
        prefix = i;
        prefix_size = entry.request_size;
      }
    }

    // Only an exact table entry overrides the two built-in TesterPresent forms.
    if (size == 2 && request[0] == 0x3E) {
      if (request[1] == 0x00)
        return {Rule::PRESENT};
      if (request[1] == 0x80)
        return {Rule::SUPPRESS};
    }
    if (prefix != NO_ENTRY)
      return this->for_entry_(prefix);
    if (size == 2 && request[0] == 0x10)
      return {Rule::SESSION};
    return {known_sid ? Rule::OUT_OF_RANGE : Rule::UNSUPPORTED};
  }

  Table table_;
  uint32_t s3_ms_;
  Counters counters_{};
  uint32_t last_request_ms_{0};
  uint32_t delay_ms_{0};
  uint32_t drop_every_{0};
  uint32_t drop_position_{0};
  uint32_t held_since_ms_{0};
  uint32_t held_delay_ms_{0};
  uint16_t held_size_{0};
  uint16_t held_session_{ANY_SESSION};
  uint8_t held_sid_{0};
  uint8_t session_{DEFAULT_SESSION};
  bool enabled_{false};
  bool pending_78_{false};
  bool pending_due_{false};
  bool held_miss_{false};
};

}  // namespace esphome::diag_server
