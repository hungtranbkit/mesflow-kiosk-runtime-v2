// Plain C++, no Arduino.h — host-testable. Phase 3A durable journal: the
// in-memory index built from scan_journal() results (event_journal_core.h)
// -- duplicate-event_id detection (§7) and capacity/backpressure accounting
// (§9), kept separate from actual file I/O (storage/event_journal.cpp) so
// this logic is fully host-testable without SPIFFS.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "journal_record.h"

namespace kiosk::protocol {

// §9 of the task -- exact thresholds from docs/OFFLINE.md's "Queue
// pressure" table.
enum class JournalPressure { NORMAL, WARNING, RESTRICTED, FULL };
JournalPressure journal_pressure_for_usage(double usage_pct);

enum class JournalAppendDecision { OK, DUPLICATE_EVENT, FULL };

struct JournalCounts {
  uint32_t pending = 0;
  uint32_t in_flight = 0;
  uint32_t acked = 0;
  uint32_t rejected = 0;
  uint32_t conflict = 0;
  uint32_t human_review = 0;
  uint32_t total() const { return pending + in_flight + acked + rejected + conflict + human_review; }
};

// Per-frame on-disk overhead this layer accounts for (see
// event_journal_core.h's frame layout: 4-byte length + body + 4-byte CRC +
// 1-byte marker).
constexpr uint32_t kJournalFrameOverheadBytes = 4 + 4 + 1;

class EventJournalIndex {
 public:
  explicit EventJournalIndex(uint32_t capacity_bytes) : capacity_bytes_(capacity_bytes) {}

  // Feeds one scanned frame's body into the index -- call once per OK frame
  // from scan_journal(), in file order (oldest first), during boot
  // recovery. EVENT frames insert (first-seen wins on a duplicate
  // event_id -- see apply_event()'s own comment); TRANSITION frames update
  // the matching entry's status fields if found.
  void apply_scanned_frame(const std::string& body_json, uint32_t frame_size);

  // §7: "Journal must reject/flag duplicate event_id insertion locally...
  // retry of an existing event must update/reuse the same durable record,
  // not create a new business event." Call BEFORE writing a new EVENT frame
  // (not during recovery replay, which uses apply_scanned_frame() instead).
  // frame_size is the caller's own estimate (4+body_len+4+1) of the frame
  // about to be written, used for the FULL check.
  JournalAppendDecision decide_append(const std::string& event_id, uint32_t frame_size) const;

  // Records a NEW event that decide_append() just approved (OK), and a
  // transition applied to an EXISTING event respectively -- call AFTER the
  // actual durable write succeeds, mirroring apply_scanned_frame()'s
  // effect on the in-memory state without re-scanning the file.
  void record_appended_event(const JournalRecord& r, uint32_t frame_size);
  void record_appended_transition(const JournalTransition& t, uint32_t frame_size);

  bool has_event(const std::string& event_id) const { return records_.count(event_id) > 0; }
  const JournalRecord* find(const std::string& event_id) const;

  // Phase 3B (2026-08-26 ESP kiosk UX-hardening pass, §17/§19 "real offline
  // replay"): the records a reconnect-driven replay should resend, in the
  // order they were originally created. Only PENDING/IN_FLIGHT qualify --
  // deliberately NOT ACKED (already succeeded, nothing to do), REJECTED
  // (the server already gave a final business answer, resending would be
  // pointless -- app/mesflow/web/kiosk_v2.py's idempotency cache would just
  // hand back the same rejection), or CONFLICT/HUMAN_REVIEW (both need a
  // resync/human decision first, not a blind resend -- same distinction
  // the compaction policy's own "always keep unsynced" already draws
  // between these six statuses). Sorted by device_seq (not map/event_id
  // order) so replay preserves the same happens-before ordering the events
  // were originally created in -- device_seq, not wall-clock time, is this
  // whole protocol's ordering authority (docs/PROTOCOL.md).
  std::vector<const JournalRecord*> pending_in_device_seq_order() const;

  // --- Compaction (2026-08-24, self-recovery task; retention scope widened
  // 2026-08-26 "keep the ESP runtime simple and disposable per request"
  // pass) ---
  // Real, live problem this fixes: the in-memory index kept EVERY record
  // ever appended forever (records_ never shrank), and on-disk usage grew
  // unbounded the same way -- a device left running for a full day of
  // testing hit 211 records / 96%+ journal capacity, and the resulting
  // internal-SRAM pressure caused xTaskCreate() itself to start failing
  // (a genuine, reproduced "kiosk stuck on an error screen" incident).
  //
  // Only PENDING/IN_FLIGHT are ALWAYS kept in full -- dropping a genuinely
  // unsynced event would violate "never silently overwrite unsynced
  // business events" (docs/OFFLINE.md). CONFLICT and HUMAN_REVIEW used to
  // ALSO be kept in full, on the same reasoning -- a REAL bug found live
  // during a 300-scan stress test (2026-08-26): unlike PENDING/IN_FLIGHT,
  // a CONFLICT record is never revisited by any code path once created
  // (kiosk_runtime.cpp sets it exactly once, right before start_resync(),
  // and nothing ever reads a CONFLICT-status record back out again --
  // pending_in_device_seq_order() only selects PENDING/IN_FLIGHT). Once the
  // resync it triggers completes, the specific failed attempt it recorded
  // is done -- the operator's actual action, if still needed, happens via
  // a brand-new event with its own event_id, never a retry of the old one.
  // That makes CONFLICT (and HUMAN_REVIEW, unused today but the same
  // reasoning applies if it's ever wired up) exactly as TERMINAL as ACKED/
  // REJECTED for retention purposes -- confirmed live: this codebase's own
  // "always keep in full" policy let a real device accumulate 43 CONFLICT
  // records in ~200 seconds with no ceiling at all, a direct contributor
  // to that stress run's internal-SRAM exhaustion and reboot. All four
  // terminal buckets are now bounded the same way: the N most-recently-
  // created of each survive (for diagnostic value), the rest are dropped
  // from both the index and the next on-disk rewrite.
  // Lowered from 20 each (2026-08-26, same stress test): 20x4=80 records
  // kept forever even right after a compaction is a real steady-state
  // floor (a few hundred bytes each) that a sustained scan rate can refill
  // and blow past before the NEXT compaction has a chance to run. 5 each
  // (20 total floor) keeps meaningful recent diagnostic history per bucket
  // while cutting that floor 4x -- combined with starting compaction at
  // the WARNING threshold instead of waiting for CRITICAL (see loop()'s own
  // comment), this is the second half of the fix for that same incident.
  struct CompactionPolicy {
    uint32_t acked_retention_count = 5;
    uint32_t rejected_retention_count = 5;
    uint32_t conflict_retention_count = 5;
    uint32_t human_review_retention_count = 5;
  };

  // Pure decision: which event_ids should survive compaction under this
  // policy. No I/O -- storage/event_journal.cpp does the actual file
  // rewrite, using this list to decide what to re-emit.
  std::vector<std::string> select_records_to_keep(const CompactionPolicy& policy) const;

  // Rebuilds this index to contain ONLY `keep_event_ids` -- called after a
  // successful on-disk compaction (storage/event_journal.cpp) to make the
  // in-memory state match the new, smaller file. Every surviving record
  // gets a FRESH frame size (compaction always re-emits one plain EVENT
  // frame per surviving record -- no transition-history frames survive,
  // by design; the record's CURRENT merged status is already what
  // encode_journal_event() will serialize).
  void rebuild_after_compaction(const std::vector<std::string>& keep_event_ids);

  // §6 of the 2026-08-27 "Final Runtime Closure" pass: a hard, COUNT-based
  // safety bound alongside the existing byte-percentage one. Real gap
  // found live during that pass's own stress testing: a burst of many
  // SMALL records (SCAN/CANCEL events have compact JSON bodies) can push
  // record_count() well past 100 while usage_pct() is still comfortably
  // under the 70% WARNING threshold below -- bytes and item count don't
  // move together when payload sizes vary, so a byte-only trigger can miss
  // a genuine "too many records sitting in RAM" situation. This does NOT
  // change what compaction keeps (CompactionPolicy's own per-status
  // retention counts are unaffected, and PENDING/IN_FLIGHT are still never
  // dropped) -- it only makes compaction START sooner when record COUNT
  // alone is already high, independent of byte usage.
  static constexpr uint32_t kMaxInMemoryJournalRecords = 120;

  // True if usage has crossed a threshold where compaction is worth
  // attempting at all (matches journal_pressure_for_usage()'s own
  // WARNING/70% boundary -- see §3 of the self-recovery task -- OR the
  // record-count bound above, whichever fires first).
  bool should_consider_compaction() const {
    return usage_pct() >= 70.0 || record_count() > kMaxInMemoryJournalRecords;
  }

  uint32_t used_bytes() const { return used_bytes_; }
  uint32_t capacity_bytes() const { return capacity_bytes_; }
  double usage_pct() const {
    return capacity_bytes_ == 0 ? 100.0 : (100.0 * static_cast<double>(used_bytes_) / capacity_bytes_);
  }
  JournalPressure pressure() const { return journal_pressure_for_usage(usage_pct()); }
  uint32_t record_count() const { return static_cast<uint32_t>(records_.size()); }
  JournalCounts counts() const;
  // uint32_t max = "never synced yet" sentinel (0xFFFFFFFF) if there is no
  // PENDING/IN_FLIGHT record at all -- never fabricate an age for "none".
  uint32_t oldest_pending_age_ms(uint32_t now_uptime_ms) const;

 private:
  uint32_t capacity_bytes_;
  uint32_t used_bytes_ = 0;
  std::map<std::string, JournalRecord> records_;
  std::map<std::string, uint32_t> frame_sizes_;  // event_id -> its own frame's on-disk size (transitions add their own separately, tracked via used_bytes_ directly)
};

}  // namespace kiosk::protocol
