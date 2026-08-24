// Host test: plain C++, no Arduino, no ESP toolchain. Exercises the /events
// response parser against exactly the shapes tools/mock_backend/mock_backend.py
// actually sends (KIOSK-089/090/091/094 groundwork).

#include <cstdio>
#include <string>

#include "../../firmware/kiosk_runtime_v2/src/protocol/event_response.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* description) {
  if (condition) {
    std::printf("  PASS: %s\n", description);
  } else {
    std::printf("  FAIL: %s\n", description);
    ++g_failures;
  }
}

}  // namespace

int main() {
  using namespace kiosk::protocol;

  std::printf("test_event_response\n");

  // --- SUCCESS shape ---
  {
    std::string json =
        R"({"accepted": true, "event_id": "e-1", "server_seq": 7, )"
        R"("state": {"name": "WAIT_OPERATION", "version": 2}, )"
        R"("workflow": {"version": 1}, )"
        R"("view": {"employee_name": "Nguyen Van A"}})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    check(ok, "SUCCESS shape parses");
    check(resp.kind == EventOutcomeKind::SUCCESS, "kind == SUCCESS");
    check(resp.event_id == "e-1", "event_id extracted");
    check(resp.server_seq == 7, "server_seq extracted");
    check(resp.snapshot.state == BusinessState::WAIT_OPERATION, "snapshot.state extracted");
    check(resp.snapshot.state_version == 2, "snapshot.state_version extracted");
    check(resp.snapshot.view.has_employee_name && resp.snapshot.view.employee_name == "Nguyen Van A",
          "snapshot.view extracted");
  }

  // --- BUSINESS_REJECTED shape (still carries a snapshot -- state unchanged) ---
  {
    std::string json =
        R"({"accepted": false, "event_id": "e-2", "server_seq": 8, )"
        R"("error": {"code": "EMPLOYEE_NOT_FOUND", "message": "Nhan vien khong hop le"}, )"
        R"("state": {"name": "WAIT_EMPLOYEE", "version": 1}, )"
        R"("workflow": {"version": 1}, "view": {}})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    check(ok, "BUSINESS_REJECTED shape parses");
    check(resp.kind == EventOutcomeKind::BUSINESS_REJECTED, "kind == BUSINESS_REJECTED");
    check(resp.error_code == "EMPLOYEE_NOT_FOUND", "error.code extracted");
    check(resp.error_message == "Nhan vien khong hop le", "error.message extracted");
    check(resp.snapshot.state == BusinessState::WAIT_EMPLOYEE,
          "rejection still carries the (unchanged) current state snapshot");
  }

  // --- CONFLICT_RESYNC shape (no snapshot -- device must GET /state) ---
  {
    std::string json =
        R"({"accepted": false, "event_id": "e-3", )"
        R"("error": {"code": "STATE_CONFLICT"}, "action": "RESYNC", )"
        R"("current_state_version": 9, "server_seq": 10})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    check(ok, "CONFLICT_RESYNC shape parses");
    check(resp.kind == EventOutcomeKind::CONFLICT_RESYNC, "kind == CONFLICT_RESYNC");
    check(resp.current_state_version == 9, "current_state_version extracted");
    check(resp.error_code == "STATE_CONFLICT", "error.code == STATE_CONFLICT");
  }

  // --- IDEMPOTENCY_KEY_REUSE_MISMATCH shape (HTTP 409 body -- this parser
  // only sees the body text, transport status is the caller's job) ---
  {
    std::string json =
        R"({"accepted": false, "event_id": "e-4", )"
        R"("error": {"code": "IDEMPOTENCY_KEY_REUSE_MISMATCH"}})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    // Not STATE_CONFLICT/RESYNC, and no state{} present -- this shape has no
    // snapshot at all, so it correctly falls through to MALFORMED rather
    // than being silently treated as a normal business rejection with a
    // fabricated/default snapshot.
    check(!ok, "no-snapshot error body is not silently treated as BUSINESS_REJECTED");
    check(resp.kind == EventOutcomeKind::MALFORMED, "kind == MALFORMED when no snapshot and not a conflict");
  }

  // --- Malformed: no "accepted" key at all ---
  {
    std::string json = R"({"foo": "bar"})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    check(!ok, "missing 'accepted' key entirely -> parse fails");
    check(resp.kind == EventOutcomeKind::MALFORMED, "kind == MALFORMED");
  }

  // --- Malformed: accepted:true but no state{} (violates contract) ---
  {
    std::string json = R"({"accepted": true, "event_id": "e-5", "server_seq": 1})";
    EventResponse resp;
    bool ok = parse_event_response_json(json, resp);
    check(!ok, "accepted:true with no state snapshot is untrusted, not defaulted");
    check(resp.kind == EventOutcomeKind::MALFORMED, "kind == MALFORMED");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
