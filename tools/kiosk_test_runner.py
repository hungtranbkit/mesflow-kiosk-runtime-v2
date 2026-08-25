#!/usr/bin/env python3
"""Kiosk v2 autonomous test runner -- reusable QA engine (not a Claude
scratchpad script, per the task's own §22: "Put reusable test engine code
in the repository" so QA Center can invoke these same modes later).

IMPORTANT SCOPE NOTE (honest, not aspirational): this runner exercises the
REAL, CURRENTLY-IMPLEMENTED online flow (employee scan -> operation scan ->
same-employee rescan finish -> GOOD/DEFECT/REWORK quantity flow) plus the
Phase 3A durable journal's SHADOW-MODE observation of that flow. It does
NOT exercise actual offline finish/replay -- that is Phase 3B's own not-yet-
built business logic (as of this runner's creation, OFFLINE BUSINESS
ENABLED: NO). A --mode that claims to test "offline" behavior today would
be testing nothing real. When Phase 3B's offline capability is built, this
runner is the place to add that scenario (see run_cycle()'s docstring).

Modes (§20 of the task):
  FAST (default)   2 representative cycles: GOOD-only, DEFECT+repairable
  TARGETED         one named scenario (--scenario good_only|defect_no_repair|defect_repair|invalid_rework)
  STRESS           --cycles N (20/50/100...), only when explicitly requested
  PHYSICAL         same as FAST/STRESS but refuses to run unless the operator
                   confirms physical scans were used (see --physical-confirmed)
  SOAK             long-duration run, --duration-s

Input ownership (§7 of the task): all quantity/keypad input goes through the
REAL device runtime via debug-input over serial (kiosk::runtime EventBus),
never a raw POST straight to the backend bypassing the ESP -- see keys()/
scan() below, both wrappers around serial_debug_protocol.debug_input().

Usage:
  python3 tools/kiosk_test_runner.py --mode fast
  python3 tools/kiosk_test_runner.py --mode targeted --scenario invalid_rework
  python3 tools/kiosk_test_runner.py --mode stress --cycles 20
"""
import argparse
import json
import os
import subprocess
import sys
import time
import random

sys.path.insert(0, os.path.dirname(__file__))
from serial_debug_protocol import debug_input, read_json_block, SerialProtocolError  # noqa: E402
import serial  # noqa: E402

DEFAULT_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_44:1B:F6:CE:64:4C-if00"
# §9 of the task: reuse the known QA dataset rather than asking the user.
EMP_QR = "WF|EMP|NV002"
OP_QR = "WF|OP|111-THAN-THUNG-R-03"
DB_CONTAINER = "mesflow-local-test-db"
DB_NAME = "mesflow_local_test"
DB_USER = "mesflow"

# §8 of the task: fixed, deterministic, logged test data -- not asked of the user.
SCENARIOS = {
    "good_only": {"good": 25, "defect": 0, "rework": 0},
    "defect_no_repair": {"good": 20, "defect": 3, "rework": 0},
    "defect_repair": {"good": 20, "defect": 4, "rework": 3},
}
INVALID_REWORK = {"good": 20, "defect": 3, "rework": 4}  # must be rejected, never committed


class KioskTestRunner:
    # 2026-08-25 connectivity-recovery task: the QR fixtures and DB
    # container/name were originally fixed module constants (LOCAL-TEST
    # only). Made instance-level, defaulting to those same LOCAL-TEST
    # values, so every existing caller is unaffected -- overriding them is
    # opt-in via KioskTestRunner(...)/--emp-qr/--op-qr/--db-container/
    # --db-name, needed to point this same engine at PROD-TEST's real
    # fixtures (different QR codes, different DB container) instead of
    # LOCAL-TEST's.
    def __init__(self, port=DEFAULT_PORT, evidence_dir=None, emp_qr=EMP_QR, op_qr=OP_QR,
                db_container=DB_CONTAINER, db_name=DB_NAME, db_user=DB_USER):
        self.ser = serial.Serial(port, 115200, timeout=3)
        time.sleep(1)
        self.evidence_dir = evidence_dir
        self.log = []
        self.emp_qr = emp_qr
        self.op_qr = op_qr
        self.db_container = db_container
        self.db_name = db_name
        self.db_user = db_user

    def close(self):
        self.ser.close()

    # --- low-level device I/O (§7: real device runtime, never a raw backend POST) ---
    def state(self, retries=5):
        for _ in range(retries):
            try:
                return read_json_block(self.ser, "device-state", timeout=10)
            except SerialProtocolError:
                self.ser.reset_input_buffer()
                time.sleep(0.3)
        raise RuntimeError("device-state failed after retries")

    # Runner bug found live (2026-08-24), traced end-to-end rather than
    # assumed: a 20s wait here is SHORTER than AsyncEventSender's own
    # legitimate worst case. kMaxAttempts=5, RETRY_BACKOFF_BASE_MS=1000,
    # RETRY_BACKOFF_MAX_MS=30000, RETRY_JITTER_PCT_MAX=25 (runtime_config.h)
    # -> worst-case backoff alone is 1250+2500+5000+10000=18750ms, plus up to
    # 5 * RUNTIME_HTTP_TIMEOUT_MS(5000ms) for the attempts themselves =
    # ~43.75s theoretical ceiling. A 20s wait_biz() gives up mid-retry, and
    # if the caller then re-scans while AsyncEventSender is still busy_,
    # kiosk_runtime.cpp's send_business_event() logs EVENT_DROPPED_BUSY and
    # silently discards the new scan (no new event, no journal record,
    # last_event_id stays frozen on the PRIOR attempt) -- confirmed via a
    # live serial trace showing last_event_id/last_latency_ms completely
    # unchanged for 45s straight after a fresh debug-input SCAN. That is
    # correct, intentional firmware behavior (nowhere honest to queue a
    # second send without a real journal-gated replay path yet) -- the
    # actual bug is this timeout being tighter than the sender's own
    # documented worst case. 60s gives ~16s of margin above the ~44s
    # ceiling.
    def wait_biz(self, target, timeout=60):
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            ds = self.state()
            last = ds["state"]["business"]
            if last == target:
                return ds
            time.sleep(0.3)
        raise TimeoutError(f"never reached {target}, last={last}")

    def scan(self, value, expect_state, timeout=60):
        debug_input(self.ser, {"type": "SCAN", "value": value}, timeout=10)
        return self.wait_biz(expect_state, timeout)

    def keys(self, *ks):
        for k in ks:
            debug_input(self.ser, {"type": "KEY_DOWN", "key": k}, timeout=10)
            debug_input(self.ser, {"type": "KEY_UP", "key": k}, timeout=10)
            time.sleep(0.2)  # real timing floor found live this session -- faster is unreliable

    # --- §10: auto-recover test state before starting ---
    def ensure_clean_state(self):
        ds = self.state()
        biz = ds["state"]["business"]
        if biz == "WAIT_EMPLOYEE":
            return ds
        # A cycle was left mid-flight (interrupted run, network hiccup, etc).
        # Never blindly overwrite projection (§10 "Do NOT blindly overwrite
        # state projection") -- finish it deliberately through the real
        # runtime instead, same as a real operator would.
        if biz == "WAIT_OPERATION":
            self.scan(self.op_qr, "SESSION_ACTIVE")
            biz = "SESSION_ACTIVE"
        if biz == "SESSION_ACTIVE":
            self.scan(self.emp_qr, "QUANTITY_INPUT")
            biz = "QUANTITY_INPUT"
        if biz == "QUANTITY_INPUT":
            self.keys("0", "#")  # GOOD=0
            self.keys("0", "#")  # DEFECT=0 -> finish immediately
            return self.wait_biz("WAIT_EMPLOYEE", timeout=60)
        return self.wait_biz("WAIT_EMPLOYEE", timeout=60)

    # --- Postgres assertions ---
    def db_query(self, sql):
        out = subprocess.run(
            ["docker", "exec", self.db_container, "psql", "-U", self.db_user, "-d", self.db_name,
             "-t", "-A", "-F,", "-c", sql],
            capture_output=True, text=True, timeout=15)
        return [line.split(",") for line in out.stdout.strip().split("\n") if line]

    def session_count(self):
        return int(self.db_query("SELECT COUNT(*) FROM work_sessions;")[0][0])

    def latest_session(self):
        rows = self.db_query(
            "SELECT id, status, employee_id, operation_id, good_qty, defect_qty, rework_qty, finish_request_id "
            "FROM work_sessions ORDER BY id DESC LIMIT 1;")
        if not rows:
            return None
        r = rows[0]
        return {"id": int(r[0]), "status": r[1], "employee_id": int(r[2]), "operation_id": int(r[3]),
                "good_qty": int(r[4]), "defect_qty": int(r[5]), "rework_qty": int(r[6]),
                "finish_request_id": r[7]}

    def open_session_count(self):
        return int(self.db_query("SELECT COUNT(*) FROM work_sessions WHERE status='OPEN';")[0][0])

    def duplicate_finish_request_count(self):
        rows = self.db_query(
            "SELECT COUNT(*) FROM (SELECT finish_request_id FROM work_sessions "
            "WHERE finish_request_id IS NOT NULL GROUP BY finish_request_id HAVING COUNT(*) > 1) x;")
        return int(rows[0][0])

    # --- §11: screenshot-based visual check, Claude inspects it itself elsewhere ---
    def capture_screenshot(self, out_dir):
        script = os.path.join(os.path.dirname(__file__), "capture_screen_serial.py")
        # NOTE: uses a SEPARATE process (its own serial open/close) -- must
        # not be called while this runner's own self.ser is doing anything;
        # callers should only invoke this between scan()/keys() calls.
        subprocess.run([sys.executable, script,
                        self.ser.port, "--timeout", "10", "--out", out_dir],
                       capture_output=True, text=True, timeout=20)

    # --- one full online cycle (real device, real backend, journal shadow-observed) ---
    def run_cycle(self, scenario_name, evidence=None):
        """Runs one canonical online cycle: employee scan -> operation scan ->
        same-employee rescan finish -> GOOD/DEFECT/REWORK quantity flow ->
        DB assertion. This is the ONLINE path (no actual offline/replay --
        see module docstring)."""
        q = SCENARIOS[scenario_name]
        good, defect, rework = q["good"], q["defect"], q["rework"]

        self.ensure_clean_state()
        count_before = self.session_count()
        ds = self.state()
        version_before = ds["state"]["state_version"]

        self.scan(self.emp_qr, "WAIT_OPERATION")
        ds = self.scan(self.op_qr, "SESSION_ACTIVE")
        session_id_str = ds["state"]["view"].get("session_id", "")
        self.scan(self.emp_qr, "QUANTITY_INPUT")

        self.keys(*list(str(good)), "#")
        self.keys(*list(str(defect)), "#")
        if defect > 0:
            if rework > 0:
                self.keys("1")
                time.sleep(0.4)
                self.keys(*list(str(rework)), "#")
            else:
                self.keys("2")
        ds_final = self.wait_biz("WAIT_EMPLOYEE", timeout=60)
        version_after = ds_final["state"]["state_version"]

        count_after = self.session_count()
        latest = self.latest_session()

        result = {
            "scenario": scenario_name, "expected": {"good": good, "defect": defect, "rework": rework},
            "session_id_str": session_id_str, "sessions_before": count_before, "sessions_after": count_after,
            "version_before": version_before, "version_after": version_after, "latest": latest,
        }
        if version_after <= version_before:
            result["ERROR"] = f"state_version did not increase: {version_before} -> {version_after}"
        elif count_after != count_before + 1:
            result["ERROR"] = f"expected exactly +1 session, got {count_after - count_before}"
        elif latest is None:
            result["ERROR"] = "no session row found"
        elif latest["status"] != "CLOSED":
            result["ERROR"] = f"latest session not CLOSED: {latest['status']}"
        elif (latest["good_qty"], latest["defect_qty"], latest["rework_qty"]) != (good, defect, rework):
            result["ERROR"] = f"quantity mismatch: expected ({good},{defect},{rework}), got " \
                              f"({latest['good_qty']},{latest['defect_qty']},{latest['rework_qty']})"
        else:
            result["OK"] = True
        return result

    # --- §15: invalid rework as a small targeted test, not a full cycle ---
    def run_invalid_rework_check(self):
        """Drives GOOD/DEFECT/repairable=YES, then a REWORK value that
        exceeds DEFECT. Expects local rejection (error_view, no network
        call for that specific over-limit attempt) and NO session created
        for that attempt, then recovers with a valid REWORK and completes."""
        q = INVALID_REWORK
        self.ensure_clean_state()

        self.scan(self.emp_qr, "WAIT_OPERATION")
        self.scan(self.op_qr, "SESSION_ACTIVE")  # legitimately creates one new work_sessions row -- must not be
                                             # counted against the invalid-attempt-creates-nothing check below
        self.scan(self.emp_qr, "QUANTITY_INPUT")
        # Sampled HERE, not before the scans above (2026-08-25 fix -- the
        # old count_before was taken before SESSION_ACTIVE's own legitimate
        # session creation, so count_after_invalid was ALWAYS != count_before
        # regardless of whether the invalid REWORK itself committed
        # anything, a false FAIL unrelated to firmware behavior).
        count_before = self.session_count()
        self.keys(*list(str(q["good"])), "#")
        self.keys(*list(str(q["defect"])), "#")
        self.keys("1")
        time.sleep(0.4)
        self.keys(*list(str(q["rework"])), "#")  # REWORK=4 > DEFECT=3 -- must be rejected
        time.sleep(1.0)
        ds = self.state()
        still_recoverable = ds["state"]["business"] == "QUANTITY_INPUT"
        count_after_invalid = self.session_count()
        no_commit = count_after_invalid == count_before

        # Recover with a valid REWORK and finish for real.
        self.keys("2", "#")  # REWORK=2 <= DEFECT=3, valid
        ds_final = self.wait_biz("WAIT_EMPLOYEE", timeout=60)
        latest = self.latest_session()
        recovered_ok = (latest is not None and latest["status"] == "CLOSED" and
                        latest["good_qty"] == q["good"] and latest["defect_qty"] == q["defect"] and
                        latest["rework_qty"] == 2)

        return {
            "still_recoverable_after_invalid": still_recoverable,
            "no_invalid_commit": no_commit,
            "recovered_with_valid_value": recovered_ok,
            "OK": still_recoverable and no_commit and recovered_ok,
        }


def journal_snapshot(ds):
    return ds.get("journal", {})


def run_fast(runner, evidence_dir):
    print("=== FAST mode: 2 cycles ===")
    os.makedirs(evidence_dir, exist_ok=True)
    ds_before = runner.state()
    with open(os.path.join(evidence_dir, "device-state-before.json"), "w") as f:
        json.dump(ds_before, f, indent=2)

    results = {}
    for name, scenario in [("CYCLE 1", "good_only"), ("CYCLE 2", "defect_repair")]:
        print(f"{name}: {scenario} -> {json.dumps(SCENARIOS[scenario])}")
        r = runner.run_cycle(scenario)
        results[name] = r
        status = "PASS" if r.get("OK") else f"FAIL: {r.get('ERROR')}"
        print(f"{name} {scenario.upper()} ........ {status}")
        with open(os.path.join(evidence_dir, f"{name.replace(' ', '_')}_result.json"), "w") as f:
            json.dump(r, f, indent=2)

    print("\n=== Invalid REWORK targeted check ===")
    inv = runner.run_invalid_rework_check()
    print(f"INVALID REWORK ............ {'PASS' if inv.get('OK') else 'FAIL'}")
    with open(os.path.join(evidence_dir, "invalid_rework_result.json"), "w") as f:
        json.dump(inv, f, indent=2)

    ds_after = runner.state()
    with open(os.path.join(evidence_dir, "device-state-after.json"), "w") as f:
        json.dump(ds_after, f, indent=2)
    with open(os.path.join(evidence_dir, "journal-state.json"), "w") as f:
        json.dump(journal_snapshot(ds_after), f, indent=2)

    open_sessions = runner.open_session_count()
    duplicates = runner.duplicate_finish_request_count()

    print(f"\nDB ........................ {'PASS' if open_sessions == 0 else 'FAIL'}")
    print(f"OPEN SESSIONS ............. {open_sessions}")
    print(f"DUPLICATES ................ {duplicates}")

    final_ok = all(r.get("OK") for r in results.values()) and inv.get("OK") and open_sessions == 0 and duplicates == 0
    print(f"\nFINAL: {'PASS' if final_ok else 'FAIL'}")

    summary = {
        "mode": "FAST", "cycles": results, "invalid_rework": inv,
        "open_sessions": open_sessions, "duplicates": duplicates, "final": "PASS" if final_ok else "FAIL",
        "device_state_before": ds_before, "device_state_after": ds_after,
    }
    with open(os.path.join(evidence_dir, "run.json"), "w") as f:
        json.dump(summary, f, indent=2)
    return final_ok


def run_stress(runner, evidence_dir, n_cycles):
    print(f"=== STRESS mode: {n_cycles} cycles ===")
    os.makedirs(evidence_dir, exist_ok=True)
    scenario_names = list(SCENARIOS.keys())
    results = []
    for i in range(1, n_cycles + 1):
        scenario = scenario_names[i % len(scenario_names)]
        try:
            r = runner.run_cycle(scenario)
        except Exception as exc:
            r = {"scenario": scenario, "EXCEPTION": repr(exc)}
        results.append(r)
        status = "OK" if r.get("OK") else f"FAIL: {r.get('ERROR') or r.get('EXCEPTION')}"
        print(f"[{i}/{n_cycles}] {scenario}: {status}")
        with open(os.path.join(evidence_dir, "run.json"), "w") as f:
            json.dump(results, f, indent=2)
    ok = sum(1 for r in results if r.get("OK"))
    print(f"\n=== SUMMARY: {ok}/{len(results)} OK ===")
    return ok == len(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=["fast", "targeted", "stress", "soak"], default="fast")
    parser.add_argument("--scenario", choices=list(SCENARIOS.keys()) + ["invalid_rework"])
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--evidence-dir", default=None)
    parser.add_argument("--physical-confirmed", action="store_true",
                        help="Required for --mode physical: confirms scans were done with the real GM65/keypad.")
    parser.add_argument("--emp-qr", default=EMP_QR,
                        help="Employee scan value (default: LOCAL-TEST's NV002 fixture).")
    parser.add_argument("--op-qr", default=OP_QR,
                        help="Operation scan value (default: LOCAL-TEST's fixture -- pass the target "
                             "environment's own started/IN_PROGRESS operation QR instead, e.g. "
                             "PROD-TEST's OP-FASTTEST-01).")
    parser.add_argument("--db-container", default=DB_CONTAINER,
                        help="Docker container name for the DB assertions (default: LOCAL-TEST's).")
    parser.add_argument("--db-name", default=DB_NAME,
                        help="Database name inside --db-container (default: LOCAL-TEST's).")
    parser.add_argument("--db-user", default=DB_USER)
    args = parser.parse_args()

    evidence_dir = args.evidence_dir or os.path.join(
        os.path.dirname(__file__), "..", "artifacts", "test-runs", time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()))
    evidence_dir = os.path.abspath(evidence_dir)

    runner = KioskTestRunner(port=args.port, evidence_dir=evidence_dir, emp_qr=args.emp_qr, op_qr=args.op_qr,
                             db_container=args.db_container, db_name=args.db_name, db_user=args.db_user)
    try:
        if args.mode == "fast":
            ok = run_fast(runner, evidence_dir)
        elif args.mode == "targeted":
            if not args.scenario:
                print("ERROR: --mode targeted requires --scenario", file=sys.stderr)
                sys.exit(2)
            os.makedirs(evidence_dir, exist_ok=True)
            if args.scenario == "invalid_rework":
                r = runner.run_invalid_rework_check()
            else:
                r = runner.run_cycle(args.scenario)
            print(json.dumps(r, indent=2))
            ok = r.get("OK", False)
        elif args.mode == "stress":
            ok = run_stress(runner, evidence_dir, args.cycles)
        else:
            print(f"mode {args.mode} not yet implemented", file=sys.stderr)
            sys.exit(2)
    finally:
        runner.close()

    print(f"\nEvidence written to: {evidence_dir}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
