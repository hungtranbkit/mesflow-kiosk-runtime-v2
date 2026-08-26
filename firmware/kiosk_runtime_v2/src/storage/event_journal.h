#pragma once

#include <Arduino.h>

#include <vector>

#include "../config/runtime_config.h"  // MESFLOW_DEBUG_API
#include "../protocol/event_journal_index.h"
#include "../protocol/journal_record.h"

namespace kiosk::storage {

// Phase 3A durable business-event journal (docs/OFFLINE.md). SPIFFS-backed
// (same partition/filesystem UiBundleStore already uses -- a SEPARATE file,
// /journal.dat, never mixed with the UI bundle files). All the actual
// frame-format/CRC/commit-marker/recovery logic lives in the portable,
// host-tested kiosk::protocol::{event_journal_core,event_journal_index,
// journal_record} -- this class is only the SPIFFS glue + the small
// convenience API kiosk_runtime.cpp's shadow-mode integration calls.
//
// Phase 3A scope (§24 of the task): this class is a durable RECORD of
// business-event lifecycles, used in SHADOW MODE only -- it observes and
// records what the existing, already-proven online send path does, but
// never itself sends anything or is consulted to decide behavior. No
// offline business action reads from this journal yet.
class EventJournal {
 public:
  // Mounts SPIFFS if not already mounted (idempotent/safe alongside
  // UiBundleStore's own init()) and recovers the index from /journal.dat.
  // capacity_bytes bounds how much of SPIFFS this journal will ever use
  // (independent of the UI bundle files' own space) -- §2 of the task.
  void init(uint32_t capacity_bytes);

  // Journals a new PENDING event. Returns false (and logs
  // JOURNAL_DUPLICATE_EVENT or JOURNAL_FULL) if decide_append() refuses --
  // caller (shadow mode) treats this as "couldn't journal it, the real
  // send still proceeds as before" per §16, never blocking the online path.
  bool append_event(const kiosk::protocol::JournalRecord& record);

  // Appends a transition record updating an existing event's status.
  // Silently a no-op (logged) if event_id isn't in the index -- Phase 3A
  // shadow mode should never actually hit this, but must not crash if it
  // does (e.g. a journal that filled up and refused the original append).
  bool append_transition(const kiosk::protocol::JournalTransition& transition);

  bool has_event(const std::string& event_id) const { return index_.has_event(event_id); }
  const kiosk::protocol::JournalRecord* find(const std::string& event_id) const { return index_.find(event_id); }

  // Phase 3B (2026-08-26, §17/§19) -- see EventJournalIndex's own comment.
  std::vector<const kiosk::protocol::JournalRecord*> pending_in_device_seq_order() const {
    return index_.pending_in_device_seq_order();
  }

  // --- Compaction (2026-08-24, self-recovery task) ---
  // Rewrites /journal.dat to contain only what
  // EventJournalIndex::select_records_to_keep() says should survive (see
  // that header for the real bug this fixes: unbounded growth eventually
  // exhausting internal SRAM via xTaskCreate() failures).
  //
  // Sequence (crash-safe at every step -- init()'s boot-time recovery below
  // handles every window this can be interrupted in):
  //   1. write every surviving record as a fresh EVENT frame to a TEMP file
  //      (/journal.dat.tmp), fully separate from the live /journal.dat.
  //   2. VALIDATE the temp file by re-scanning it -- every frame must be OK
  //      and the record count must match what was written. Never swap in an
  //      unvalidated file.
  //   3. only then: rename /journal.dat -> /journal.dat.old, rename
  //      /journal.dat.tmp -> /journal.dat, remove /journal.dat.old.
  //   4. rebuild the in-memory index to match.
  // Returns false (index_ and the on-disk file both UNCHANGED) if anything
  // in steps 1-2 fails -- compaction is a pure optimization, never allowed
  // to risk losing a real record.
  bool compact(const kiosk::protocol::EventJournalIndex::CompactionPolicy& policy =
                   kiosk::protocol::EventJournalIndex::CompactionPolicy());
  bool should_consider_compaction() const { return index_.should_consider_compaction(); }

  // --- Incremental (non-blocking-enough) compaction (2026-08-25) ---
  // compact() above does the whole rewrite in one call -- measured at
  // ~4.2s for 214 records on real hardware, which blocks scanner/keypad/
  // display polling for that whole span since everything runs on the main
  // loop(). begin_compaction()/compact_tick() do the SAME crash-safe
  // temp-file/validate/atomic-swap sequence, but the expensive part (the
  // record-by-record encode+flash-write) is spread across up to
  // kCompactionRecordsPerTick records per compact_tick() call, so the
  // caller's loop() gets control back between chunks -- keypad/scanner/
  // display polling and the watchdog all get serviced on every other
  // iteration instead of only after the whole thing finishes. Validation +
  // the atomic swap + index rebuild still happen synchronously in
  // whichever compact_tick() call writes the LAST record -- they're fast
  // reads/renames, not the slow part.
  //
  // Use this (not compact()) for the ROUTINE periodic 70%+ trigger, where
  // responsiveness matters most. compact() is kept for the rare CRITICAL
  // low-memory / task-creation-failure emergency paths, where finishing
  // the fix in one bounded call (rather than spreading it across several
  // ticks while the emergency condition might still be active) is the
  // more sensible tradeoff -- see kiosk_runtime_v2.ino's own comments at
  // each call site.
  static constexpr uint32_t kCompactionRecordsPerTick = 10;

  // Starts an incremental compaction. False (no-op) if one is already
  // active, or if select_records_to_keep() finds nothing to drop. Opens
  // the temp file and decides the keep-list; does NOT write anything yet
  // (the first compact_tick() call does the first chunk).
  bool begin_compaction(const kiosk::protocol::EventJournalIndex::CompactionPolicy& policy =
                            kiosk::protocol::EventJournalIndex::CompactionPolicy());

  // Call every loop() iteration -- a cheap no-op when compaction_active()
  // is false. Writes up to kCompactionRecordsPerTick more records; if that
  // finishes the keep-list, also validates + atomically swaps + rebuilds
  // the index in this same call, then clears compaction_active().
  void compact_tick();

  bool compaction_active() const { return compaction_active_; }
  uint32_t compaction_records_processed() const { return static_cast<uint32_t>(compaction_write_index_); }
  uint32_t compaction_records_total() const { return static_cast<uint32_t>(compaction_keep_.size()); }
  uint32_t compaction_last_progress_ms() const { return compaction_last_progress_ms_; }

  uint32_t used_bytes() const { return index_.used_bytes(); }
  uint32_t capacity_bytes() const { return index_.capacity_bytes(); }
  double usage_pct() const { return index_.usage_pct(); }
  kiosk::protocol::JournalPressure pressure() const { return index_.pressure(); }
  uint32_t record_count() const { return index_.record_count(); }
  kiosk::protocol::JournalCounts counts() const { return index_.counts(); }
  uint32_t oldest_pending_age_ms(uint32_t now_uptime_ms) const { return index_.oldest_pending_age_ms(now_uptime_ms); }

  bool init_ok() const { return init_ok_; }

#if MESFLOW_DEBUG_API
  // §10 of the 2026-08-25 finish-anti-stuck-recovery follow-up: sets up the
  // exact on-disk file state a REAL crash would leave behind at one of
  // compact()'s interruption windows -- without actually running/crashing
  // a compaction -- so init()'s boot-time orphan-recovery logic can be
  // exercised against every window on real hardware. Caller
  // (kiosk_runtime_v2.ino's 'simulate-compaction-crash:<n>' serial command)
  // reboots immediately after calling this so the NEXT boot's real init()
  // does the actual recovering.
  //   1 = stray INCOMPLETE tmp (crash mid-write of one frame)
  //   2 = complete, VALID tmp, crash before validate+swap ever ran
  //   3 = mid-swap (.dat renamed to .old, the new file never arrived)
  //   4 = swap completed, final .old cleanup didn't run (both present)
  void simulate_compaction_crash_for_test(int scenario);
#endif

 private:
  kiosk::protocol::EventJournalIndex index_{0};
  bool init_ok_ = false;

  // Incremental compaction state -- valid only while compaction_active_.
  bool compaction_active_ = false;
  std::vector<std::string> compaction_keep_;
  size_t compaction_write_index_ = 0;
  uint32_t compaction_last_progress_ms_ = 0;

  // Shared tail (VALIDATE + atomic swap + index rebuild) used by both
  // compact() and compact_tick()'s final chunk -- see compact()'s own
  // comment for the step-by-step crash-safety rationale.
  bool finish_compaction_validate_and_swap(const std::vector<std::string>& keep);
};

}  // namespace kiosk::storage
