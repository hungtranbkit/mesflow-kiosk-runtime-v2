// Plain C++, no Arduino.h — host-testable. Phase 3A durable journal: the
// RECORD SHAPE this file encodes/decodes matches docs/OFFLINE.md's "Record
// shape (target)" exactly (record_version/event_id/device_seq/event_type/
// payload/CRC/commit_marker/sync_status/retry_metadata), plus the fields
// the Phase 3A task additionally specified (boot_id, payload_hash,
// expected_state_version, created_uptime_ms, time_sync_status,
// last_attempt_uptime_ms, last_error_code).
//
// This layer only knows how to turn a JournalRecord/JournalTransition into
// a JSON "body" string and back -- it has no opinion on file framing (see
// event_journal_core.h) or storage (see storage/event_journal.h). Same
// protocol/-vs-storage/ layering ui_bundle.h/ui_bundle_store.h already use.
#pragma once

#include <cstdint>
#include <string>

namespace kiosk::protocol {

// §3 of the task: do not invent ambiguous states like "DONE". These six are
// the only durable business-event lifecycle states.
enum class JournalSyncStatus {
  PENDING,
  IN_FLIGHT,
  ACKED,
  REJECTED,
  CONFLICT,
  HUMAN_REVIEW,
};

const char* journal_sync_status_to_string(JournalSyncStatus s);
// false (and leaves `out` unchanged) for any string that isn't one of the
// six above -- never guessed/defaulted to PENDING.
bool journal_sync_status_from_string(const std::string& s, JournalSyncStatus& out);

// A full durable business-event record (§3 of the task's required fields).
// `payload` is the exact envelope JSON string that would be/was sent over
// the wire (encode_event_json()'s output) -- the journal stores what was
// ACTUALLY sent, never a re-derived approximation.
struct JournalRecord {
  uint8_t record_version = 1;
  std::string event_id;
  uint64_t device_seq = 0;
  std::string boot_id;
  std::string event_type;
  std::string payload;
  std::string payload_hash;  // hex crc32 of `payload`, computed at journal-time
  uint64_t expected_state_version = 0;
  uint32_t created_uptime_ms = 0;
  std::string time_sync_status;
  uint32_t retry_count = 0;
  uint32_t last_attempt_uptime_ms = 0;
  std::string last_error_code;
  JournalSyncStatus sync_status = JournalSyncStatus::PENDING;
};

// A small append-only status-transition record (§6 of the task: "append-
// only state transition records...instead of rewriting the entire journal
// file every time event status changes"). Identifies which event_id it
// updates; recovery replays these over the original JournalRecord in file
// order, latest transition wins.
struct JournalTransition {
  std::string event_id;
  JournalSyncStatus sync_status = JournalSyncStatus::PENDING;
  uint32_t retry_count = 0;
  uint32_t last_attempt_uptime_ms = 0;
  std::string last_error_code;
};

// JSON body encoders/decoders. Each body carries its own "k":"E"/"T"
// discriminator (event_journal_core.h's scanner doesn't need to know the
// difference -- only the higher-level recovery/index logic does).
std::string encode_journal_event(const JournalRecord& r);
std::string encode_journal_transition(const JournalTransition& t);

enum class JournalBodyKind { EVENT, TRANSITION, UNKNOWN };
JournalBodyKind journal_body_kind(const std::string& body_json);

// Both return false (record_version unrecognized, or a required field is
// missing/malformed) without partially filling `out` -- §13's "unknown
// record version" test case relies on this returning false cleanly, not
// crashing or guessing.
bool decode_journal_event(const std::string& body_json, JournalRecord& out);
bool decode_journal_transition(const std::string& body_json, JournalTransition& out);

}  // namespace kiosk::protocol
