// Plain C++, no Arduino.h — host-testable (see test/host/). §7/§8.
#pragma once

#include <cstdint>
#include <functional>

namespace kiosk::protocol {

// Wear-aware persistent monotonic counter ("reservation block" technique).
//
// Instead of persisting on every next() call (which would wear NVS flash
// under high event frequency), this persists a "high-water mark" only once
// per `block_size` values handed out. On construction (i.e. every boot),
// it reserves a fresh block ABOVE whatever was last persisted, immediately
// -- so even a crash before this boot ever persists again can't cause a
// value to repeat on the NEXT boot (that boot will load the already-bumped
// high-water mark).
//
// Trade-off: up to `block_size - 1` sequence values are skipped/wasted on
// every reboot. That's the standard, correct cost of this technique --
// documented rather than hidden. device_seq has effectively unlimited
// range for this purpose (uint64_t), so this is cheap in practice.
//
// `load`/`persist` are injected so this stays host-testable: pass in-memory
// lambdas sharing a variable to simulate "reboot" (construct a new
// SequenceReservation against the same backing store) versus "power loss
// before persisting" (construct a new one WITHOUT calling persist first).
class SequenceReservation {
 public:
  using LoadFn = std::function<uint64_t()>;
  using PersistFn = std::function<void(uint64_t)>;

  SequenceReservation(LoadFn load, PersistFn persist, uint64_t block_size)
      : persist_(std::move(persist)), block_size_(block_size) {
    uint64_t high_water_mark = load();
    current_ = high_water_mark;
    reserved_ceiling_ = high_water_mark;
    reserve_next_block();
  }

  // Returns the next monotonically-increasing value. Never returns the
  // same value twice, even across construction (reboot) boundaries, as
  // long as `persist` actually writes durably.
  uint64_t next() {
    if (current_ >= reserved_ceiling_) {
      reserve_next_block();
    }
    return ++current_;
  }

  uint64_t current() const { return current_; }
  uint64_t reserved_ceiling() const { return reserved_ceiling_; }

 private:
  void reserve_next_block() {
    reserved_ceiling_ = current_ + block_size_;
    persist_(reserved_ceiling_);
  }

  PersistFn persist_;
  uint64_t block_size_;
  uint64_t current_ = 0;
  uint64_t reserved_ceiling_ = 0;
};

}  // namespace kiosk::protocol
