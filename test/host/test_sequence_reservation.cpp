// Host test: plain C++, no Arduino. §7/§8/§51.
#include <cstdio>
#include <set>

#include "../../firmware/kiosk_runtime_v2/src/protocol/sequence_reservation.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}
}  // namespace

int main() {
  using kiosk::protocol::SequenceReservation;

  std::printf("test_sequence_reservation\n");

  // --- Monotonicity within one "boot" ---
  {
    uint64_t backing = 0;
    SequenceReservation seq([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 1000);
    uint64_t a = seq.next();
    uint64_t b = seq.next();
    uint64_t c = seq.next();
    check(a < b && b < c, "values strictly increase within one instance");
    check(a == 1, "first value handed out is 1 (backing store started at 0)");
  }

  // --- Simulated clean reboot: new instance shares the same backing store ---
  {
    uint64_t backing = 0;
    uint64_t last = 0;
    {
      SequenceReservation boot1([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 10);
      for (int i = 0; i < 5; ++i) last = boot1.next();
    }
    // "reboot": construct a fresh instance against the same backing value.
    SequenceReservation boot2([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 10);
    uint64_t next_val = boot2.next();
    check(next_val > last, "value after simulated reboot is still greater than before it");
  }

  // --- Power loss BEFORE the block was exhausted: the persisted high-water
  //     mark is always AHEAD of the last handed-out value (reserved eagerly
  //     at construction), so even "crashing" without calling next() again
  //     can't cause a repeat on the next simulated boot. ---
  {
    uint64_t backing = 0;
    uint64_t handed_out_before_crash;
    {
      SequenceReservation boot1([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 100);
      handed_out_before_crash = boot1.next();  // only ONE value used, block still has 99 left
      // Simulated crash: boot1 goes out of scope, "backing" already holds
      // the eagerly-reserved ceiling (100), not just "1".
    }
    check(backing >= 100, "high-water mark was persisted eagerly at construction, "
                          "not only when the block was exhausted");
    SequenceReservation boot2([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 100);
    uint64_t next_val = boot2.next();
    check(next_val > handed_out_before_crash,
          "post-crash value still greater than the last one hand out before the crash");
  }

  // --- No write-per-event: persist() should be called far less often than
  //     next() -- this is the whole point (§8: don't wear NVS per event). ---
  {
    uint64_t backing = 0;
    int persist_calls = 0;
    SequenceReservation seq([&]() { return backing; },
                            [&](uint64_t v) {
                              backing = v;
                              ++persist_calls;
                            },
                            1000);
    for (int i = 0; i < 2500; ++i) seq.next();
    // 1 reservation at construction + 2 more block rollovers for ~2500 values
    // at block size 1000 => persist called a handful of times, not 2500.
    check(persist_calls <= 5, "persist() called a small, bounded number of times, not once per next()");
    std::printf("    (persist_calls=%d for 2500 next() calls)\n", persist_calls);
  }

  // --- Uniqueness across many values within a run ---
  {
    uint64_t backing = 0;
    SequenceReservation seq([&]() { return backing; }, [&](uint64_t v) { backing = v; }, 50);
    std::set<uint64_t> seen;
    bool all_unique = true;
    for (int i = 0; i < 500; ++i) {
      if (!seen.insert(seq.next()).second) all_unique = false;
    }
    check(all_unique, "500 sequential values are all unique");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
