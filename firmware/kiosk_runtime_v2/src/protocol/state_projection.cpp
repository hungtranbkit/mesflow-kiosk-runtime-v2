#include "state_projection.h"

#include "json_extract.h"

namespace kiosk::protocol {

const char* business_state_to_string(BusinessState s) {
  switch (s) {
    case BusinessState::WAIT_EMPLOYEE: return "WAIT_EMPLOYEE";
    case BusinessState::WAIT_OPERATION: return "WAIT_OPERATION";
    case BusinessState::SESSION_ACTIVE: return "SESSION_ACTIVE";
    case BusinessState::QUANTITY_INPUT: return "QUANTITY_INPUT";
    case BusinessState::DEVICE_DISABLED: return "DEVICE_DISABLED";
    case BusinessState::MAINTENANCE: return "MAINTENANCE";
    case BusinessState::UNSUPPORTED: return "UNSUPPORTED";
  }
  return "UNSUPPORTED";
}

BusinessState business_state_from_string(const std::string& s) {
  if (s == "WAIT_EMPLOYEE") return BusinessState::WAIT_EMPLOYEE;
  if (s == "WAIT_OPERATION") return BusinessState::WAIT_OPERATION;
  if (s == "SESSION_ACTIVE") return BusinessState::SESSION_ACTIVE;
  if (s == "QUANTITY_INPUT") return BusinessState::QUANTITY_INPUT;
  if (s == "DEVICE_DISABLED") return BusinessState::DEVICE_DISABLED;
  if (s == "MAINTENANCE") return BusinessState::MAINTENANCE;
  // §53: never silently map an unrecognized state to WAIT_EMPLOYEE.
  return BusinessState::UNSUPPORTED;
}

const char* screen_id_for_business_state(BusinessState s) {
  switch (s) {
    case BusinessState::WAIT_EMPLOYEE: return "state_wait_employee";
    case BusinessState::WAIT_OPERATION: return "state_wait_operation";
    case BusinessState::SESSION_ACTIVE: return "state_session_active";
    case BusinessState::QUANTITY_INPUT: return "state_quantity_input";
    case BusinessState::DEVICE_DISABLED: return "state_device_disabled";
    case BusinessState::MAINTENANCE: return "state_maintenance";
    case BusinessState::UNSUPPORTED: return "";
  }
  return "";
}

bool ViewModel::operator==(const ViewModel& o) const {
  return has_employee_name == o.has_employee_name && employee_name == o.employee_name &&
         has_operation_code == o.has_operation_code && operation_code == o.operation_code &&
         has_operation_name == o.has_operation_name && operation_name == o.operation_name &&
         has_session_id == o.has_session_id && session_id == o.session_id &&
         has_started_at == o.has_started_at && started_at == o.started_at &&
         has_target_qty == o.has_target_qty && target_qty == o.target_qty &&
         has_produced_qty == o.has_produced_qty && produced_qty == o.produced_qty;
}

bool StateSnapshot::operator==(const StateSnapshot& o) const {
  return state == o.state && state_version == o.state_version &&
         workflow_version == o.workflow_version && view == o.view;
}

namespace {
void parse_view(const std::string& view_json, ViewModel& v) {
  if (json_has_key(view_json, "employee_name") && !json_is_null(view_json, "employee_name")) {
    v.has_employee_name = true;
    v.employee_name = json_extract_string(view_json, "employee_name");
  }
  if (json_has_key(view_json, "operation_code") && !json_is_null(view_json, "operation_code")) {
    v.has_operation_code = true;
    v.operation_code = json_extract_string(view_json, "operation_code");
  }
  if (json_has_key(view_json, "operation_name") && !json_is_null(view_json, "operation_name")) {
    v.has_operation_name = true;
    v.operation_name = json_extract_string(view_json, "operation_name");
  }
  if (json_has_key(view_json, "session_id") && !json_is_null(view_json, "session_id")) {
    v.has_session_id = true;
    v.session_id = json_extract_string(view_json, "session_id");
  }
  if (json_has_key(view_json, "started_at") && !json_is_null(view_json, "started_at")) {
    v.has_started_at = true;
    v.started_at = json_extract_string(view_json, "started_at");
  }
  // §32/KIOSK-092: target_qty/produced_qty of 0 must be distinguishable
  // from absent -- check has_key first, never rely on the numeric value
  // alone (0 is truthy-false in naive checks, exactly the bug this guards).
  if (json_has_key(view_json, "target_qty") && !json_is_null(view_json, "target_qty")) {
    v.has_target_qty = true;
    v.target_qty = static_cast<int32_t>(json_extract_int(view_json, "target_qty", 0));
  }
  if (json_has_key(view_json, "produced_qty") && !json_is_null(view_json, "produced_qty")) {
    v.has_produced_qty = true;
    v.produced_qty = static_cast<int32_t>(json_extract_int(view_json, "produced_qty", 0));
  }
}
}  // namespace

bool parse_state_snapshot_json(const std::string& json, StateSnapshot& out) {
  std::string state_obj = json_extract_object(json, "state");
  if (state_obj.empty()) return false;  // malformed -- no state{} at all

  if (!json_has_key(state_obj, "name") || !json_has_key(state_obj, "version")) {
    return false;  // malformed -- missing required fields
  }

  out.state = business_state_from_string(json_extract_string(state_obj, "name"));
  out.state_version = static_cast<uint64_t>(json_extract_int(state_obj, "version", 0));

  std::string workflow_obj = json_extract_object(json, "workflow");
  out.workflow_version =
      workflow_obj.empty() ? 0 : static_cast<uint32_t>(json_extract_int(workflow_obj, "version", 0));

  std::string view_obj = json_extract_object(json, "view");
  out.view = ViewModel{};  // reset to all-absent before parsing
  if (!view_obj.empty()) parse_view(view_obj, out.view);

  return true;
}

const char* apply_result_to_string(ApplyResult r) {
  switch (r) {
    case ApplyResult::APPLIED: return "APPLIED";
    case ApplyResult::APPLIED_IDENTICAL: return "APPLIED_IDENTICAL";
    case ApplyResult::REJECTED_STALE: return "REJECTED_STALE";
    case ApplyResult::REJECTED_INCONSISTENT: return "REJECTED_INCONSISTENT";
    case ApplyResult::REJECTED_UNSUPPORTED: return "REJECTED_UNSUPPORTED";
  }
  return "UNKNOWN";
}

ApplyResult StateProjection::apply(const StateSnapshot& incoming) {
  if (incoming.state == BusinessState::UNSUPPORTED) {
    // §53: never applied, regardless of version -- retain last-known-good.
    return ApplyResult::REJECTED_UNSUPPORTED;
  }

  if (!has_snapshot_) {
    current_ = incoming;
    has_snapshot_ = true;
    return ApplyResult::APPLIED;
  }

  if (incoming.state_version < current_.state_version) {
    // Invariant 16: a server state version must never move backward on
    // the device. Do not apply; caller reports STATE_VERSION_REGRESSION.
    return ApplyResult::REJECTED_STALE;
  }

  if (incoming.state_version == current_.state_version) {
    if (incoming == current_) {
      return ApplyResult::APPLIED_IDENTICAL;  // §49: harmless duplicate
    }
    // Same version, different content -- a real inconsistency worth
    // surfacing (STATE_SNAPSHOT_INCONSISTENT), not silently picking one.
    return ApplyResult::REJECTED_INCONSISTENT;
  }

  current_ = incoming;
  return ApplyResult::APPLIED;
}

}  // namespace kiosk::protocol
