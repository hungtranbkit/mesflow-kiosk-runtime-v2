// Host test: plain C++, no Arduino. §68.
#include <cstdio>

#include "../../firmware/kiosk_runtime_v2/src/protocol/state_projection.h"

namespace {
int g_failures = 0;
void check(bool condition, const char* description) {
  std::printf("  %s: %s\n", condition ? "PASS" : "FAIL", description);
  if (!condition) ++g_failures;
}

using namespace kiosk::protocol;

StateSnapshot make(BusinessState s, uint64_t version, uint32_t workflow = 1) {
  StateSnapshot snap;
  snap.state = s;
  snap.state_version = version;
  snap.workflow_version = workflow;
  return snap;
}
}  // namespace

int main() {
  std::printf("test_state_projection\n");

  // --- First snapshot always applies ---
  {
    StateProjection proj;
    auto r = proj.apply(make(BusinessState::WAIT_EMPLOYEE, 10));
    check(r == ApplyResult::APPLIED, "first snapshot ever applies");
    check(proj.current().state == BusinessState::WAIT_EMPLOYEE, "state recorded correctly");
    check(proj.current().state_version == 10, "version recorded correctly");
  }

  // --- Newer version applies ---
  {
    StateProjection proj;
    proj.apply(make(BusinessState::WAIT_EMPLOYEE, 10));
    auto r = proj.apply(make(BusinessState::WAIT_OPERATION, 11));
    check(r == ApplyResult::APPLIED, "newer version applies");
    check(proj.current().state == BusinessState::WAIT_OPERATION, "new state reflected");
  }

  // --- Invariant 16: older version rejected, never applied ---
  {
    StateProjection proj;
    proj.apply(make(BusinessState::SESSION_ACTIVE, 20));
    auto r = proj.apply(make(BusinessState::WAIT_EMPLOYEE, 15));
    check(r == ApplyResult::REJECTED_STALE, "older version -> REJECTED_STALE (invariant 16)");
    check(proj.current().state == BusinessState::SESSION_ACTIVE && proj.current().state_version == 20,
          "current snapshot unchanged after a stale rejection");
  }

  // --- Same version, identical content -> harmless (§49) ---
  {
    StateProjection proj;
    auto snap = make(BusinessState::SESSION_ACTIVE, 20);
    proj.apply(snap);
    auto r = proj.apply(snap);  // exact same snapshot again
    check(r == ApplyResult::APPLIED_IDENTICAL, "identical re-apply at same version is harmless");
  }

  // --- Same version, DIFFERENT content -> inconsistent, detected ---
  {
    StateProjection proj;
    proj.apply(make(BusinessState::SESSION_ACTIVE, 20));
    auto different = make(BusinessState::WAIT_OPERATION, 20);  // same version, different state!
    auto r = proj.apply(different);
    check(r == ApplyResult::REJECTED_INCONSISTENT,
          "same version + different content -> REJECTED_INCONSISTENT (STATE_SNAPSHOT_INCONSISTENT)");
    check(proj.current().state == BusinessState::SESSION_ACTIVE,
          "inconsistent snapshot never applied -- current stays the last trusted one");
  }

  // --- Unknown/unsupported state name never silently mapped (§53) ---
  {
    check(business_state_from_string("SOME_FUTURE_STATE") == BusinessState::UNSUPPORTED,
          "unrecognized state name -> UNSUPPORTED, not WAIT_EMPLOYEE");
    check(business_state_from_string("WAIT_EMPLOYEE") == BusinessState::WAIT_EMPLOYEE,
          "known state name parses correctly");
  }

  // --- UNSUPPORTED never applied, even with a newer version, even as the
  //     very first snapshot (§53) ---
  {
    StateProjection proj;
    proj.apply(make(BusinessState::WAIT_EMPLOYEE, 10));
    auto r = proj.apply(make(BusinessState::UNSUPPORTED, 999));  // "newer" but unrecognized
    check(r == ApplyResult::REJECTED_UNSUPPORTED, "UNSUPPORTED state rejected even with a higher version");
    check(proj.current().state == BusinessState::WAIT_EMPLOYEE && proj.current().state_version == 10,
          "last-known-good snapshot retained, not overwritten by the unsupported one");

    StateProjection fresh;
    auto r2 = fresh.apply(make(BusinessState::UNSUPPORTED, 1));
    check(r2 == ApplyResult::REJECTED_UNSUPPORTED, "UNSUPPORTED rejected even as the very first snapshot ever");
    check(!fresh.has_snapshot(), "no snapshot recorded at all in that case -- caller must check has_snapshot()");
  }

  // --- Atomic apply: state + view + workflow_version all update together ---
  {
    StateProjection proj;
    StateSnapshot s1 = make(BusinessState::WAIT_OPERATION, 5, 1);
    s1.view.has_employee_name = true;
    s1.view.employee_name = "Nguyen Van A";
    proj.apply(s1);

    StateSnapshot s2 = make(BusinessState::SESSION_ACTIVE, 6, 2);
    s2.view.has_employee_name = true;
    s2.view.employee_name = "Nguyen Van A";
    s2.view.has_operation_code = true;
    s2.view.operation_code = "OP-882";
    proj.apply(s2);

    check(proj.current().state == BusinessState::SESSION_ACTIVE &&
              proj.current().workflow_version == 2 && proj.current().view.has_operation_code,
          "state/workflow_version/view all reflect the SAME new snapshot together (no half-state)");
  }

  // --- JSON parsing of a snapshot (shared shape: /events success + /state) ---
  {
    std::string json =
        R"({"accepted":true,"event_id":"e1","server_seq":5012,)"
        R"("state":{"name":"SESSION_ACTIVE","version":104},)"
        R"("workflow":{"version":1},)"
        R"("view":{"employee_name":"Nguyen Van A","operation_code":"OP-882",)"
        R"("session_id":"S-99312","target_qty":100,"produced_qty":0}})";
    StateSnapshot out;
    bool ok = parse_state_snapshot_json(json, out);
    check(ok, "well-formed snapshot JSON parses");
    check(out.state == BusinessState::SESSION_ACTIVE, "parsed state correct");
    check(out.state_version == 104, "parsed state_version correct");
    check(out.workflow_version == 1, "parsed workflow_version correct");
    check(out.view.has_employee_name && out.view.employee_name == "Nguyen Van A", "parsed employee_name");
    check(out.view.has_produced_qty && out.view.produced_qty == 0,
          "produced_qty=0 parsed as PRESENT with value 0, not absent (KIOSK-092/§32)");
  }

  // --- Malformed snapshot JSON ---
  {
    StateSnapshot out;
    bool ok = parse_state_snapshot_json(R"({"accepted":true})", out);
    check(!ok, "JSON with no state{} at all fails to parse (malformed, not defaulted)");
  }

  std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
