// Plain C++, no Arduino.h — host-testable (see test/host/).
//
// Parses the /events response body into one of the three shapes
// docs/PROTOCOL.md defines (tools/mock_backend/mock_backend.py is the
// authoritative reference implementation this was built against):
//
//   SUCCESS:           accepted:true,  event_id, server_seq, state{}/workflow{}/view{}
//   BUSINESS_REJECTED: accepted:false, event_id, server_seq, error{code,message}, state{}/workflow{}/view{}
//   CONFLICT_RESYNC:   accepted:false, event_id, error.code=STATE_CONFLICT, action=RESYNC,
//                       current_state_version -- deliberately NO state snapshot here; the
//                       device must GET /state itself rather than trust anything the
//                       conflicting event's response might guess (§8: never self-merge).
//
// A response that matches none of these shapes is MALFORMED: the caller
// must not apply anything from it (invariant 14 -- a bad parse is not
// license to guess a state transition).
#pragma once

#include <cstdint>
#include <string>

#include "state_projection.h"

namespace kiosk::protocol {

enum class EventOutcomeKind {
  SUCCESS,
  BUSINESS_REJECTED,
  CONFLICT_RESYNC,
  MALFORMED,
};

const char* event_outcome_kind_to_string(EventOutcomeKind kind);

struct EventResponse {
  EventOutcomeKind kind = EventOutcomeKind::MALFORMED;
  std::string event_id;
  int64_t server_seq = -1;             // -1 if absent
  std::string error_code;              // set for BUSINESS_REJECTED and CONFLICT_RESYNC
  std::string error_message;           // set for BUSINESS_REJECTED (mock backend doesn't send one for conflict)
  StateSnapshot snapshot;              // valid only when kind == SUCCESS or BUSINESS_REJECTED
  uint64_t current_state_version = 0;  // valid only when kind == CONFLICT_RESYNC
};

// Returns false (and sets out.kind = MALFORMED) if the body doesn't match
// any recognized shape -- true otherwise, with `out` fully populated for
// whichever shape it matched.
bool parse_event_response_json(const std::string& json, EventResponse& out);

}  // namespace kiosk::protocol
