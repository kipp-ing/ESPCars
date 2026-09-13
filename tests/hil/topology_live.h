// Live CAN topology catcher — identity by MAC, reported the moment it changes.
//
// Bench diagnostics for tests/hil/. Not a component, not shipped: it transmits
// unconditionally, which is wrong on a vehicle bus (CLAUDE.md).
//
// WHY THIS EXISTS. The previous catcher mapped board name -> beacon id through a
// fixed table. Every new board needed an entry, a board missing from the table
// was invisible, and a stale table lied confidently. Here each node beacons its
// own **MAC**, so a board nobody has ever configured still identifies itself
// correctly the first time it speaks — and it is the same identifier
// script/hil/ports.py uses over USB, so the bench names boards identically over
// both transports.
//
// The id is derived from the MAC and therefore carries identity by itself:
//
//     can_id = 0x100 | (mac[5] << 2) | (bus & 0x03)      -> 0x100-0x4FF
//
// Stable (a board always beacons on the same id) and collision-free for
// distinct final MAC bytes, which holds on this bench: 0x50 green, 0xE0 orange,
// 0xF0 blue, 0xC0 purple. Two nodes sharing an id would corrupt each other's
// payloads — arbitration separates ids, not data — so the receiver compares the
// full 6-byte MAC and warns if one id ever arrives carrying two different MACs.
// That warning is the thing that turns a silent collision into a visible one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
#include "esp_mac.h"
}

using esphome::millis;

namespace topo {

static const char *const TAG = "topo";

// A partner is "gone" after this long without a beacon. 3 s is five missed
// beacons at 1.5 Hz: long enough that losing arbitration under a 91 % load
// never flickers a healthy link out, short enough to feel live while a hand is
// still on the connector.
static const uint32_t TTL_MS = 3000;
static const uint8_t MAX_PEERS = 8;
static const uint8_t MAX_PORTS = 4;

struct Peer {
  uint8_t mac[6];
  uint8_t bus;
  uint32_t can_id;
  uint32_t seen;  ///< millis of last beacon; 0 = free slot
};

struct Port {
  const char *name;
  uint8_t bus;  ///< this port's own bus index, reported so a host need not guess
  Peer peers[MAX_PEERS];
};

inline Port g_ports[MAX_PORTS];
inline uint8_t g_port_count = 0;

/// Ports are registered on first use and matched by name, so a config adds a
/// port simply by naming it. The bus index is carried explicitly rather than
/// derived from the name: a host that has to infer "seg2 means bus 1" from a
/// trailing digit breaks the moment someone names a port sensibly.
inline Port *port_for(const char *name, uint8_t bus) {
  for (uint8_t i = 0; i < g_port_count; i++)
    if (std::strcmp(g_ports[i].name, name) == 0)
      return &g_ports[i];
  if (g_port_count >= MAX_PORTS)
    return nullptr;
  Port *p = &g_ports[g_port_count++];
  p->name = name;
  p->bus = bus;
  std::memset(p->peers, 0, sizeof(p->peers));
  return p;
}

inline void own_mac(uint8_t out[6]) { esp_read_mac(out, ESP_MAC_WIFI_STA); }

inline uint32_t beacon_id(const uint8_t mac[6], uint8_t bus) {
  return 0x100u | (static_cast<uint32_t>(mac[5]) << 2) | (bus & 0x03u);
}

/// Build this node's beacon payload: full MAC, bus index, rolling counter.
///
/// The counter matters. Anything downstream that publishes on change (the
/// decode entities do) would otherwise fire once at the first beacon and never
/// again — which is exactly how the previous catcher went stale five seconds
/// after boot and then reported every wire empty for the rest of the run.
/// Takes the port name and registers it, which is load-bearing: ports used to
/// be registered lazily on first *receive*, so a port that heard nothing was
/// never registered and report() simply never mentioned it. A silent port then
/// looked identical to a port that does not exist — the exact blind spot this
/// catcher exists to remove. Registering on transmit covers every declared
/// port, because every declared port beacons.
inline uint32_t fill_beacon(const char *port_name, uint8_t out[8], uint8_t bus, uint8_t *seq) {
  port_for(port_name, bus);
  uint8_t mac[6];
  own_mac(mac);
  std::memcpy(out, mac, 6);
  out[6] = bus;
  out[7] = (*seq)++;
  return beacon_id(mac, bus);
}

/// Feed one received frame in. Cheap enough for the observe drain: a bounded
/// scan of at most 8 slots and no allocation.
inline void observe(const char *port_name, uint8_t bus, uint32_t can_id, const uint8_t *data, uint8_t dlc,
                    bool extended, bool rtr) {
  if (extended || rtr || dlc != 8 || can_id < 0x100u || can_id > 0x4FFu)
    return;
  // Self-consistency: the id must be the one this payload's MAC would produce.
  // Rejects ordinary traffic that happens to land in the range, without needing
  // a magic number to collide with.
  if (can_id != beacon_id(data, data[6] & 0x03u))
    return;

  Port *port = port_for(port_name, bus);
  if (port == nullptr)
    return;

  const uint32_t now = millis();
  int8_t free_slot = -1;
  for (uint8_t i = 0; i < MAX_PEERS; i++) {
    Peer &peer = port->peers[i];
    if (peer.seen == 0) {
      if (free_slot < 0)
        free_slot = static_cast<int8_t>(i);
      continue;
    }
    if (peer.can_id == can_id && std::memcmp(peer.mac, data, 6) != 0) {
      ESP_LOGE(TAG, "%s ! id=0x%03X carries two MACs (%02X:%02X:%02X:%02X:%02X:%02X vs "
                    "%02X:%02X:%02X:%02X:%02X:%02X) - they corrupt each other, change one board",
               port_name, static_cast<unsigned>(can_id), peer.mac[0], peer.mac[1], peer.mac[2], peer.mac[3],
               peer.mac[4], peer.mac[5], data[0], data[1], data[2], data[3], data[4], data[5]);
      return;
    }
    if (std::memcmp(peer.mac, data, 6) == 0 && peer.bus == data[6]) {
      peer.seen = now;  // known partner, refresh only — no log line
      return;
    }
  }
  if (free_slot < 0)
    return;  // 8 partners on one wire is not this bench; dropping is honest

  Peer &peer = port->peers[free_slot];
  std::memcpy(peer.mac, data, 6);
  peer.bus = data[6];
  peer.can_id = can_id;
  peer.seen = now;
  ESP_LOGI(TAG, "%s + %02X:%02X:%02X:%02X:%02X:%02X.bus%u id=0x%03X (NEW)", port_name, data[0], data[1], data[2],
           data[3], data[4], data[5], data[6], static_cast<unsigned>(can_id));
}

/// Expire silent partners. observe() only ever adds or refreshes, so this is
/// what makes a pulled connector print something instead of just going quiet.
inline void sweep() {
  const uint32_t now = millis();
  for (uint8_t p = 0; p < g_port_count; p++) {
    Port &port = g_ports[p];
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
      Peer &peer = port.peers[i];
      if (peer.seen == 0 || (now - peer.seen) <= TTL_MS)
        continue;
      ESP_LOGW(TAG, "%s - %02X:%02X:%02X:%02X:%02X:%02X.bus%u id=0x%03X (LOST, silent %u ms)", port.name,
               peer.mac[0], peer.mac[1], peer.mac[2], peer.mac[3], peer.mac[4], peer.mac[5], peer.bus,
               static_cast<unsigned>(peer.can_id), static_cast<unsigned>(now - peer.seen));
      peer.seen = 0;
    }
  }
}

/// Periodic full picture, so a console attached midway still learns the state
/// without waiting for something to change.
inline void report() {
  uint8_t mac[6];
  own_mac(mac);
  for (uint8_t p = 0; p < g_port_count; p++) {
    Port &port = g_ports[p];
    char line[192];
    size_t n = 0;
    uint8_t live = 0;
    for (uint8_t i = 0; i < MAX_PEERS && n + 1 < sizeof(line); i++) {
      const Peer &peer = port.peers[i];
      if (peer.seen == 0)
        continue;
      live++;
      // snprintf returns what it WOULD have written, so n must be clamped after
      // every call or the next sizeof(line) - n underflows (size_t) into a huge
      // remaining-space value and the write runs off the end.
      const int w = std::snprintf(line + n, sizeof(line) - n, " %02X:%02X:%02X:%02X:%02X:%02X.bus%u(0x%03X)",
                                  peer.mac[0], peer.mac[1], peer.mac[2], peer.mac[3], peer.mac[4], peer.mac[5],
                                  peer.bus, static_cast<unsigned>(peer.can_id));
      if (w < 0)
        break;
      n += static_cast<size_t>(w);
      if (n >= sizeof(line)) {
        n = sizeof(line) - 1;
        break;
      }
    }
    // The board's own MAC is on EVERY report line, not just the empty one, so a
    // host reading one console can bind it to a board without a name table or a
    // USB descriptor. That matters for the classic ESP32, which sits behind a
    // CP2102 bridge and has no MAC to read over USB at all.
    if (live == 0)
      ESP_LOGW(TAG, "%s [%02X:%02X:%02X:%02X:%02X:%02X.bus%u] = nothing", port.name, mac[0], mac[1], mac[2],
               mac[3], mac[4], mac[5], port.bus);
    else
      ESP_LOGI(TAG, "%s [%02X:%02X:%02X:%02X:%02X:%02X.bus%u] =%s", port.name, mac[0], mac[1], mac[2], mac[3],
               mac[4], mac[5], port.bus, line);
  }
}

}  // namespace topo
