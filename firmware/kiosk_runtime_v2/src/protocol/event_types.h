// Plain C++, no Arduino.h — must stay host-testable (see test/host/).
#pragma once

#include <cstdint>
#include <string>

namespace kiosk::protocol {

// §17/§79: the device sends a small, generic event vocabulary; the SERVER
// (mock_backend.py) does all business interpretation (e.g. parsing
// "WF|EMP|00152" out of a SCAN's raw payload) -- picking `SCAN` as one
// generic type rather than `EMPLOYEE_SCANNED`/`OPERATION_SCANNED` is a
// deliberate choice (§79) so the device never has to know which kind of
// scan it just saw.
enum class EventType {
  SCAN,
  FINISH_REQUESTED,
  QUANTITY_SUBMITTED,
  CANCEL_REQUESTED,
};

inline const char* event_type_to_string(EventType t) {
  switch (t) {
    case EventType::SCAN: return "SCAN";
    case EventType::FINISH_REQUESTED: return "FINISH_REQUESTED";
    case EventType::QUANTITY_SUBMITTED: return "QUANTITY_SUBMITTED";
    case EventType::CANCEL_REQUESTED: return "CANCEL_REQUESTED";
  }
  return "UNKNOWN";
}

// §11/§15/§16: device clock is NOT authoritative. UNSYNCED/STALE/FAILED all
// mean "don't trust timestamp_device for anything but diagnostics" --
// device_seq is the real ordering authority regardless of this value.
enum class TimeSyncStatus {
  UNSYNCED,  // never synced this boot
  SYNCING,   // sync attempt in progress
  SYNCED,    // synced and still fresh
  STALE,     // was synced, but sync_age exceeds TIME_SYNC_STALE_AFTER_S
  FAILED,    // an explicit sync attempt failed
};

inline const char* time_sync_status_to_string(TimeSyncStatus s) {
  switch (s) {
    case TimeSyncStatus::UNSYNCED: return "UNSYNCED";
    case TimeSyncStatus::SYNCING: return "SYNCING";
    case TimeSyncStatus::SYNCED: return "SYNCED";
    case TimeSyncStatus::STALE: return "STALE";
    case TimeSyncStatus::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

// A quantity field that may be legitimately 0 or legitimately absent.
// KIOSK-006: these two must never collapse into the same encoded form.
struct OptionalQuantity {
  bool present = false;
  int32_t value = 0;
};

// Payload for a SCAN event. Deliberately just "what the hardware saw" — no
// business interpretation (§26 of the original task spec: hardware layer
// does not parse business entities). `raw` is the verbatim line read from
// the scanner (or injected via /debug/input).
struct ScanPayload {
  std::string source;  // e.g. "GM65" or "DEBUG_INPUT"
  std::string raw;
};

// context{} -- §7: `expected_state_version` is the device's optimistic-
// concurrency claim ("this is the version I last saw") -- the backend
// rejects with STATE_CONFLICT if it has since moved on. workflow_version/
// ui_bundle_version are just the device's current understanding, for the
// backend's own diagnostics; Phase 2 doesn't use them for anything.
struct EventContext {
  uint64_t expected_state_version = 0;
  uint32_t workflow_version = 0;
  uint32_t ui_bundle_version = 0;
};

// device{} -- who is sending this. hardware_id/device_id/boot_id are three
// DIFFERENT identifiers (docs/PROTOCOL.md, kiosk::security::DeviceIdentity)
// -- never conflate them.
struct EventDeviceInfo {
  std::string device_id;
  std::string hardware_id;
  std::string boot_id;
};

// event{} -- identity/ordering of this specific business event.
struct EventInfo {
  std::string event_id;   // stable across retries -- never regenerated
  uint64_t device_seq = 0;
  EventType type = EventType::SCAN;
};

// time{} -- §17: device clock is diagnostics-only. `timestamp_device_iso`
// is EMPTY when sync_status is UNSYNCED/FAILED -- never fabricate an epoch
// (no "1970-01-01" standing in for "we don't know").
struct EventTimeInfo {
  std::string timestamp_device_iso;  // "" if not synced; ISO-8601 if it is
  uint64_t uptime_ms = 0;
  TimeSyncStatus sync_status = TimeSyncStatus::UNSYNCED;
  uint32_t sync_age_s = 0;  // 0 if never synced
};

struct KioskEvent {
  uint32_t protocol_version = 1;
  EventDeviceInfo device;
  EventInfo event;
  EventTimeInfo time;
  EventContext context;
  ScanPayload payload;
  OptionalQuantity quantity_good;  // present only to exercise KIOSK-006 in the codec/tests
  // GOOD/DEFECT/REWORK quantity flow task: DEFECT and REWORK travel with the
  // SAME KIOSK-006 present/absent discipline as quantity_good -- 0 and
  // absent must stay distinguishable for these too. Only ever set together
  // with quantity_good on the single final QUANTITY_SUBMITTED event (§14/§16
  // of the task: one final submit, never partial/early commits).
  OptionalQuantity quantity_defect;
  OptionalQuantity quantity_rework;
};

}  // namespace kiosk::protocol
