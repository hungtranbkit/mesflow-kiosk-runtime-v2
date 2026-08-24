#include "event_response.h"

#include "json_extract.h"

namespace kiosk::protocol {

const char* event_outcome_kind_to_string(EventOutcomeKind kind) {
  switch (kind) {
    case EventOutcomeKind::SUCCESS: return "SUCCESS";
    case EventOutcomeKind::BUSINESS_REJECTED: return "BUSINESS_REJECTED";
    case EventOutcomeKind::CONFLICT_RESYNC: return "CONFLICT_RESYNC";
    case EventOutcomeKind::MALFORMED: return "MALFORMED";
  }
  return "MALFORMED";
}

bool parse_event_response_json(const std::string& json, EventResponse& out) {
  out = EventResponse{};

  if (!json_has_key(json, "accepted")) {
    out.kind = EventOutcomeKind::MALFORMED;
    return false;
  }

  bool accepted = json_extract_bool(json, "accepted", false);
  out.event_id = json_extract_string(json, "event_id", "");
  out.server_seq = json_extract_int(json, "server_seq", -1);

  std::string error_obj = json_extract_object(json, "error");
  if (!error_obj.empty()) {
    out.error_code = json_extract_string(error_obj, "code", "");
    out.error_message = json_extract_string(error_obj, "message", "");
  }

  std::string action = json_extract_string(json, "action", "");

  if (!accepted && out.error_code == "STATE_CONFLICT" && action == "RESYNC") {
    // §8: no state snapshot trusted from a conflict response -- the device
    // must GET /state itself rather than self-merge.
    out.current_state_version =
        static_cast<uint64_t>(json_extract_int(json, "current_state_version", 0));
    out.kind = EventOutcomeKind::CONFLICT_RESYNC;
    return true;
  }

  StateSnapshot snap;
  if (!parse_state_snapshot_json(json, snap)) {
    // Both SUCCESS and BUSINESS_REJECTED must carry a full state snapshot
    // per contract -- if it's missing, the whole response is untrustworthy,
    // not just the state part of it (invariant 14: never guess).
    out.kind = EventOutcomeKind::MALFORMED;
    return false;
  }
  out.snapshot = snap;
  out.kind = accepted ? EventOutcomeKind::SUCCESS : EventOutcomeKind::BUSINESS_REJECTED;
  return true;
}

}  // namespace kiosk::protocol
