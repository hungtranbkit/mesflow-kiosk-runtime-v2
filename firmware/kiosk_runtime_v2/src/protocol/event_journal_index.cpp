#include "event_journal_index.h"

#include <algorithm>

namespace kiosk::protocol {

namespace {
bool is_terminal(JournalSyncStatus s) {
  return s == JournalSyncStatus::ACKED || s == JournalSyncStatus::REJECTED;
}
}  // namespace

JournalPressure journal_pressure_for_usage(double usage_pct) {
  if (usage_pct >= 100.0) return JournalPressure::FULL;
  if (usage_pct >= 90.0) return JournalPressure::RESTRICTED;
  if (usage_pct >= 70.0) return JournalPressure::WARNING;
  return JournalPressure::NORMAL;
}

void EventJournalIndex::apply_scanned_frame(const std::string& body_json, uint32_t frame_size) {
  used_bytes_ += frame_size;
  JournalBodyKind kind = journal_body_kind(body_json);
  if (kind == JournalBodyKind::EVENT) {
    JournalRecord r;
    if (!decode_journal_event(body_json, r)) return;  // unknown version/malformed -- already logged by the caller
    if (records_.count(r.event_id)) return;  // §7: first-seen wins on a duplicate event_id in the log itself
    records_[r.event_id] = r;
    frame_sizes_[r.event_id] = frame_size;
  } else if (kind == JournalBodyKind::TRANSITION) {
    JournalTransition t;
    if (!decode_journal_transition(body_json, t)) return;
    auto it = records_.find(t.event_id);
    if (it == records_.end()) return;  // transition for an event not (yet/ever) seen -- caller may log this
    it->second.sync_status = t.sync_status;
    it->second.retry_count = t.retry_count;
    it->second.last_attempt_uptime_ms = t.last_attempt_uptime_ms;
    it->second.last_error_code = t.last_error_code;
  }
  // JournalBodyKind::UNKNOWN: framing was valid (passed CRC/commit marker)
  // but the body itself doesn't parse as either kind -- used_bytes_ still
  // counts its on-disk space (it's real, occupied space), but it
  // contributes no record. Caller logs this as a decode-level anomaly.
}

JournalAppendDecision EventJournalIndex::decide_append(const std::string& event_id, uint32_t frame_size) const {
  if (records_.count(event_id)) return JournalAppendDecision::DUPLICATE_EVENT;
  if (used_bytes_ + frame_size > capacity_bytes_) return JournalAppendDecision::FULL;
  return JournalAppendDecision::OK;
}

void EventJournalIndex::record_appended_event(const JournalRecord& r, uint32_t frame_size) {
  records_[r.event_id] = r;
  frame_sizes_[r.event_id] = frame_size;
  used_bytes_ += frame_size;
}

void EventJournalIndex::record_appended_transition(const JournalTransition& t, uint32_t frame_size) {
  auto it = records_.find(t.event_id);
  if (it != records_.end()) {
    it->second.sync_status = t.sync_status;
    it->second.retry_count = t.retry_count;
    it->second.last_attempt_uptime_ms = t.last_attempt_uptime_ms;
    it->second.last_error_code = t.last_error_code;
  }
  used_bytes_ += frame_size;
}

const JournalRecord* EventJournalIndex::find(const std::string& event_id) const {
  auto it = records_.find(event_id);
  return it == records_.end() ? nullptr : &it->second;
}

std::vector<const JournalRecord*> EventJournalIndex::pending_in_device_seq_order() const {
  std::vector<const JournalRecord*> out;
  for (const auto& kv : records_) {
    if (kv.second.sync_status == JournalSyncStatus::PENDING ||
        kv.second.sync_status == JournalSyncStatus::IN_FLIGHT) {
      out.push_back(&kv.second);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const JournalRecord* a, const JournalRecord* b) { return a->device_seq < b->device_seq; });
  return out;
}

JournalCounts EventJournalIndex::counts() const {
  JournalCounts c;
  for (const auto& kv : records_) {
    switch (kv.second.sync_status) {
      case JournalSyncStatus::PENDING: ++c.pending; break;
      case JournalSyncStatus::IN_FLIGHT: ++c.in_flight; break;
      case JournalSyncStatus::ACKED: ++c.acked; break;
      case JournalSyncStatus::REJECTED: ++c.rejected; break;
      case JournalSyncStatus::CONFLICT: ++c.conflict; break;
      case JournalSyncStatus::HUMAN_REVIEW: ++c.human_review; break;
    }
  }
  return c;
}

std::vector<std::string> EventJournalIndex::select_records_to_keep(const CompactionPolicy& policy) const {
  std::vector<std::string> keep;
  // Non-terminal states: always kept, in whatever order the map gives us
  // (event_id order -- doesn't matter, each record is self-contained).
  for (const auto& kv : records_) {
    if (!is_terminal(kv.second.sync_status)) keep.push_back(kv.first);
  }

  // Terminal states: sort each bucket by created_uptime_ms DESCENDING
  // (most recent first) and keep only the policy's retention count.
  auto collect_and_trim = [&](JournalSyncStatus status, uint32_t retention) {
    std::vector<std::pair<uint32_t, std::string>> bucket;  // (created_uptime_ms, event_id)
    for (const auto& kv : records_) {
      if (kv.second.sync_status == status) bucket.emplace_back(kv.second.created_uptime_ms, kv.first);
    }
    std::sort(bucket.begin(), bucket.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    uint32_t n = std::min(retention, static_cast<uint32_t>(bucket.size()));
    for (uint32_t i = 0; i < n; ++i) keep.push_back(bucket[i].second);
  };
  collect_and_trim(JournalSyncStatus::ACKED, policy.acked_retention_count);
  collect_and_trim(JournalSyncStatus::REJECTED, policy.rejected_retention_count);

  return keep;
}

void EventJournalIndex::rebuild_after_compaction(const std::vector<std::string>& keep_event_ids) {
  std::map<std::string, JournalRecord> kept;
  uint32_t new_used_bytes = 0;
  std::map<std::string, uint32_t> kept_frame_sizes;
  for (const auto& id : keep_event_ids) {
    auto it = records_.find(id);
    if (it == records_.end()) continue;  // shouldn't happen -- defensive, never fabricate a record
    kept[id] = it->second;
    std::string body = encode_journal_event(it->second);
    uint32_t frame_size = kJournalFrameOverheadBytes + static_cast<uint32_t>(body.size());
    kept_frame_sizes[id] = frame_size;
    new_used_bytes += frame_size;
  }
  records_ = std::move(kept);
  frame_sizes_ = std::move(kept_frame_sizes);
  used_bytes_ = new_used_bytes;
}

uint32_t EventJournalIndex::oldest_pending_age_ms(uint32_t now_uptime_ms) const {
  bool found = false;
  uint32_t oldest_created = 0;
  for (const auto& kv : records_) {
    auto st = kv.second.sync_status;
    if (st != JournalSyncStatus::PENDING && st != JournalSyncStatus::IN_FLIGHT) continue;
    if (!found || kv.second.created_uptime_ms < oldest_created) {
      oldest_created = kv.second.created_uptime_ms;
      found = true;
    }
  }
  if (!found) return 0xFFFFFFFFu;  // sentinel: nothing pending, never fabricate an age
  return now_uptime_ms >= oldest_created ? (now_uptime_ms - oldest_created) : 0;
}

}  // namespace kiosk::protocol
