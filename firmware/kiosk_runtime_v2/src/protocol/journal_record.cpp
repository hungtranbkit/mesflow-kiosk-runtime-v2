#include "journal_record.h"

#include <cstdio>

#include "json_extract.h"
#include "protocol_codec.h"  // json_escape

namespace kiosk::protocol {

const char* journal_sync_status_to_string(JournalSyncStatus s) {
  switch (s) {
    case JournalSyncStatus::PENDING: return "PENDING";
    case JournalSyncStatus::IN_FLIGHT: return "IN_FLIGHT";
    case JournalSyncStatus::ACKED: return "ACKED";
    case JournalSyncStatus::REJECTED: return "REJECTED";
    case JournalSyncStatus::CONFLICT: return "CONFLICT";
    case JournalSyncStatus::HUMAN_REVIEW: return "HUMAN_REVIEW";
  }
  return "PENDING";
}

bool journal_sync_status_from_string(const std::string& s, JournalSyncStatus& out) {
  if (s == "PENDING") { out = JournalSyncStatus::PENDING; return true; }
  if (s == "IN_FLIGHT") { out = JournalSyncStatus::IN_FLIGHT; return true; }
  if (s == "ACKED") { out = JournalSyncStatus::ACKED; return true; }
  if (s == "REJECTED") { out = JournalSyncStatus::REJECTED; return true; }
  if (s == "CONFLICT") { out = JournalSyncStatus::CONFLICT; return true; }
  if (s == "HUMAN_REVIEW") { out = JournalSyncStatus::HUMAN_REVIEW; return true; }
  return false;  // never guessed -- an unrecognized status string is a real decode failure
}

std::string encode_journal_event(const JournalRecord& r) {
  char num_buf[32];
  std::string out;
  out.reserve(r.payload.size() + 256);
  out += "{\"k\":\"E\"";
  std::snprintf(num_buf, sizeof(num_buf), "%u", r.record_version);
  out += std::string(",\"record_version\":") + num_buf;
  out += ",\"event_id\":\"" + json_escape(r.event_id) + "\"";
  std::snprintf(num_buf, sizeof(num_buf), "%llu", static_cast<unsigned long long>(r.device_seq));
  out += std::string(",\"device_seq\":") + num_buf;
  out += ",\"boot_id\":\"" + json_escape(r.boot_id) + "\"";
  out += ",\"event_type\":\"" + json_escape(r.event_type) + "\"";
  out += ",\"payload\":\"" + json_escape(r.payload) + "\"";
  out += ",\"payload_hash\":\"" + json_escape(r.payload_hash) + "\"";
  std::snprintf(num_buf, sizeof(num_buf), "%llu", static_cast<unsigned long long>(r.expected_state_version));
  out += std::string(",\"expected_state_version\":") + num_buf;
  std::snprintf(num_buf, sizeof(num_buf), "%u", r.created_uptime_ms);
  out += std::string(",\"created_uptime_ms\":") + num_buf;
  out += ",\"time_sync_status\":\"" + json_escape(r.time_sync_status) + "\"";
  std::snprintf(num_buf, sizeof(num_buf), "%u", r.retry_count);
  out += std::string(",\"retry_count\":") + num_buf;
  std::snprintf(num_buf, sizeof(num_buf), "%u", r.last_attempt_uptime_ms);
  out += std::string(",\"last_attempt_uptime_ms\":") + num_buf;
  out += ",\"last_error_code\":\"" + json_escape(r.last_error_code) + "\"";
  out += ",\"sync_status\":\"" + std::string(journal_sync_status_to_string(r.sync_status)) + "\"";
  out += "}";
  return out;
}

std::string encode_journal_transition(const JournalTransition& t) {
  char num_buf[32];
  std::string out;
  out += "{\"k\":\"T\"";
  out += ",\"event_id\":\"" + json_escape(t.event_id) + "\"";
  out += ",\"sync_status\":\"" + std::string(journal_sync_status_to_string(t.sync_status)) + "\"";
  std::snprintf(num_buf, sizeof(num_buf), "%u", t.retry_count);
  out += std::string(",\"retry_count\":") + num_buf;
  std::snprintf(num_buf, sizeof(num_buf), "%u", t.last_attempt_uptime_ms);
  out += std::string(",\"last_attempt_uptime_ms\":") + num_buf;
  out += ",\"last_error_code\":\"" + json_escape(t.last_error_code) + "\"";
  out += "}";
  return out;
}

JournalBodyKind journal_body_kind(const std::string& body_json) {
  std::string k = json_extract_string(body_json, "k", "");
  if (k == "E") return JournalBodyKind::EVENT;
  if (k == "T") return JournalBodyKind::TRANSITION;
  return JournalBodyKind::UNKNOWN;
}

bool decode_journal_event(const std::string& body_json, JournalRecord& out) {
  if (journal_body_kind(body_json) != JournalBodyKind::EVENT) return false;
  // §13/§14 "unknown record version": the ONLY version this runtime
  // understands is 1 -- a higher/different version from a future firmware
  // (or corrupted data) must be rejected explicitly, never guessed through.
  int64_t version = json_extract_int(body_json, "record_version", -1);
  if (version != 1) return false;
  std::string event_id = json_extract_string(body_json, "event_id", "");
  if (event_id.empty()) return false;  // required field
  std::string event_type = json_extract_string(body_json, "event_type", "");
  if (event_type.empty()) return false;  // required field
  std::string sync_status_str = json_extract_string(body_json, "sync_status", "");
  JournalSyncStatus sync_status;
  if (!journal_sync_status_from_string(sync_status_str, sync_status)) return false;

  JournalRecord r;
  r.record_version = static_cast<uint8_t>(version);
  r.event_id = event_id;
  r.device_seq = static_cast<uint64_t>(json_extract_int(body_json, "device_seq", 0));
  r.boot_id = json_extract_string(body_json, "boot_id", "");
  r.event_type = event_type;
  r.payload = json_extract_string(body_json, "payload", "");
  r.payload_hash = json_extract_string(body_json, "payload_hash", "");
  r.expected_state_version = static_cast<uint64_t>(json_extract_int(body_json, "expected_state_version", 0));
  r.created_uptime_ms = static_cast<uint32_t>(json_extract_int(body_json, "created_uptime_ms", 0));
  r.time_sync_status = json_extract_string(body_json, "time_sync_status", "");
  r.retry_count = static_cast<uint32_t>(json_extract_int(body_json, "retry_count", 0));
  r.last_attempt_uptime_ms = static_cast<uint32_t>(json_extract_int(body_json, "last_attempt_uptime_ms", 0));
  r.last_error_code = json_extract_string(body_json, "last_error_code", "");
  r.sync_status = sync_status;
  out = r;
  return true;
}

bool decode_journal_transition(const std::string& body_json, JournalTransition& out) {
  if (journal_body_kind(body_json) != JournalBodyKind::TRANSITION) return false;
  std::string event_id = json_extract_string(body_json, "event_id", "");
  if (event_id.empty()) return false;
  std::string sync_status_str = json_extract_string(body_json, "sync_status", "");
  JournalSyncStatus sync_status;
  if (!journal_sync_status_from_string(sync_status_str, sync_status)) return false;

  JournalTransition t;
  t.event_id = event_id;
  t.sync_status = sync_status;
  t.retry_count = static_cast<uint32_t>(json_extract_int(body_json, "retry_count", 0));
  t.last_attempt_uptime_ms = static_cast<uint32_t>(json_extract_int(body_json, "last_attempt_uptime_ms", 0));
  t.last_error_code = json_extract_string(body_json, "last_error_code", "");
  out = t;
  return true;
}

}  // namespace kiosk::protocol
