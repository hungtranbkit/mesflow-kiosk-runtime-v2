// Plain C++, no Arduino.h — host-testable (see test/host/). §25: this
// module holds authoritative state and a read-only view_model; it NEVER
// calls hardware, never renders, never decides business rules -- it only
// enforces the version/consistency invariants (16) and hands the caller a
// snapshot to render or reject.
#pragma once

#include <cstdint>
#include <string>

namespace kiosk::protocol {

// Canonical business states (§4). UNSUPPORTED is deliberately included and
// is NOT silently mapped to WAIT_EMPLOYEE (§53) -- a server sending a state
// name this runtime doesn't recognize is a real, visible problem (stale
// firmware vs. newer server), not something to guess through.
enum class BusinessState {
  WAIT_EMPLOYEE,
  WAIT_OPERATION,
  SESSION_ACTIVE,
  QUANTITY_INPUT,
  DEVICE_DISABLED,
  MAINTENANCE,
  UNSUPPORTED,
};

const char* business_state_to_string(BusinessState s);
BusinessState business_state_from_string(const std::string& s);  // UNSUPPORTED if unrecognized

// Canonical screen_id for each business state -- shared by the hardcoded
// Renderer screens (renderer.cpp), the Phase 4 UI bundle lookup
// (kiosk_runtime.cpp), and the Python E2E runner's own copy of this same
// mapping (tools/kiosk_e2e_runner.py's SCREEN_ID_FOR_STATE). "" only for
// UNSUPPORTED, which is never looked up in a bundle at all (§53: an
// unrecognized state never gets a plausible-looking screen).
const char* screen_id_for_business_state(BusinessState s);

// Read-only projection for rendering only (§10/§11) -- never consulted for
// permission/business-transition decisions on the device. Each field is
// explicitly present/absent (matching the OptionalQuantity pattern already
// used for protocol events) rather than treating "" as "absent".
struct ViewModel {
  bool has_employee_name = false;
  std::string employee_name;
  bool has_operation_code = false;
  std::string operation_code;
  bool has_operation_name = false;
  std::string operation_name;
  bool has_session_id = false;
  std::string session_id;
  bool has_started_at = false;
  std::string started_at;
  bool has_target_qty = false;
  int32_t target_qty = 0;
  bool has_produced_qty = false;
  int32_t produced_qty = 0;

  bool operator==(const ViewModel& other) const;
  bool operator!=(const ViewModel& other) const { return !(*this == other); }
};

struct StateSnapshot {
  BusinessState state = BusinessState::WAIT_EMPLOYEE;
  uint64_t state_version = 0;
  uint32_t workflow_version = 0;
  ViewModel view;

  bool operator==(const StateSnapshot& other) const;
  bool operator!=(const StateSnapshot& other) const { return !(*this == other); }
};

// Parses `{"state":{"name":...,"version":...},"workflow":{"version":...},
// "view":{...}}` (as found in both /events success responses and /state)
// into a StateSnapshot. Returns false if state.name/state.version couldn't
// be parsed at all (malformed) -- caller must not apply a default snapshot
// in that case.
bool parse_state_snapshot_json(const std::string& json, StateSnapshot& out);

enum class ApplyResult {
  APPLIED,                 // newer version, applied
  APPLIED_IDENTICAL,       // same version AND same content -- harmless re-apply (§49)
  REJECTED_STALE,          // incoming version < current (invariant 16) -- STATE_VERSION_REGRESSION
  REJECTED_INCONSISTENT,   // same version, DIFFERENT content -- STATE_SNAPSHOT_INCONSISTENT
  REJECTED_UNSUPPORTED,    // incoming.state == UNSUPPORTED -- §53, never applied even if
                          // the version is newer; the device can't safely act on a state
                          // name it doesn't recognize. Retains the last-known-good snapshot.
};

const char* apply_result_to_string(ApplyResult r);

// Holds the current authoritative snapshot. Never touches hardware.
class StateProjection {
 public:
  ApplyResult apply(const StateSnapshot& incoming);
  bool has_snapshot() const { return has_snapshot_; }
  const StateSnapshot& current() const { return current_; }

 private:
  bool has_snapshot_ = false;
  StateSnapshot current_;
};

}  // namespace kiosk::protocol
