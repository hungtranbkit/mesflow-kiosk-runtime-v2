// Arduino-dependent (uses esp_random()/Preferences) — unlike
// protocol_codec/event_types/sequence_reservation, this file is not
// host-tested; it's a thin, easily-inspected wrapper so the NVS/randomness
// glue is the only untested part. The actual reservation-block ALGORITHM
// it wraps (SequenceReservation) is portable and IS host-tested.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sequence_reservation.h"

namespace kiosk::protocol {

// Generates a fresh random hex id. Not persisted, not derived from time —
// matches "boot_id must change every boot" (docs/PROTOCOL.md) and gives
// event_id enough entropy to be a real dedupe key.
std::string generate_random_hex_id(size_t byte_length);

// Persistent, wear-aware, monotonic device_seq (§7/§8, docs/PROTOCOL.md).
// Backed by SequenceReservation (protocol/sequence_reservation.h) with NVS
// load/persist -- see that header for the reservation-block technique and
// its documented trade-off (up to block_size-1 values skipped per reboot).
//
// IMPORTANT: does NOT touch NVS in its constructor. This class is a member
// of KioskRuntime, which is constructed as a global object -- global C++
// static initializers run before Arduino's own startup has called
// nvs_flash_init(), so any Preferences access at that point silently fails
// (Preferences::begin() returns false, every read yields its default, every
// write is a no-op) with no crash to notice. This was a REAL bug found on
// real hardware (device_seq reset to 1 every reboot instead of persisting)
// before this comment existed -- init() MUST be called explicitly from
// KioskRuntime::begin(), which runs from setup(), safely after Arduino's
// own NVS init has completed. Calling next()/current() before init() logs
// a loud warning and returns 0 -- no silent fallback to a working-looking
// but wrong value.
class DeviceSequence {
 public:
  void init();
  uint64_t next();
  uint64_t current() const;

 private:
  std::unique_ptr<SequenceReservation> reservation_;
};

}  // namespace kiosk::protocol
