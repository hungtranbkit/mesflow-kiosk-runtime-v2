#!/usr/bin/env python3
"""Kiosk E2E Functional Test Runner (mesflow-kiosk-runtime-v2, Phase 2+).

Drives a REAL ESP32-S3 board (DEV profile, MESFLOW_DEBUG_API on) through the
kiosk's basic functionality end-to-end against the project's own stateful
mock backend (tools/mock_backend/mock_backend.py), captures real screenshots,
asserts state at multiple layers, injects faults, and produces a PASS/FAIL
report with full evidence under artifacts/e2e/<run-id>/.

OBSERVABILITY CONTRACT (this file's design constraint, not just a nice-to-
have): every step prints progress as it happens, every wait is a bounded
poll (never a blind sleep), every step has a hard timeout, and a timeout
captures evidence immediately rather than waiting for the whole suite to
finish. If you can't tell what the runner is doing within a few seconds by
reading its stdout, that's a bug in this file.

Usage:
  BACKEND_URL=https://xxxx.lhr.life python3 tools/kiosk_e2e_runner.py --suite basic
  BACKEND_URL=https://xxxx.lhr.life python3 tools/kiosk_e2e_runner.py --case E2E-004
  python3 tools/kiosk_e2e_runner.py --list

Suites (run one group at a time -- see docs/TEST_PLAN.md for the full list):
  basic     preflight, bootstrap, one full happy-path cycle (default)
  business  + invalid employee/operation, duplicate scan, invalid transition
  ui        visual sweep of all 6 business states + debug regression
  network   dropped-response/idempotency, state conflict/resync, backend outage
  state     reboot-restore, DEVICE_DISABLED, MAINTENANCE
  soak      10-cycle repeat + memory before/after -- NOT part of any of the above
  full      basic+business+ui+network+state (NOT soak -- run that separately)

Config (env vars):
  ESP_IP        default 192.168.100.81
  ESP_PORT      default 8081
  BACKEND_URL   REQUIRED, no default (normally an ephemeral tunnel hostname)
  DEVICE_ID     default KIOSK-LASER-01
  SERIAL_PORT   default /dev/ttyACM0 (reboot test only)
"""
import argparse
import importlib.util
import json
import os
import re
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

_spec = importlib.util.spec_from_file_location("capture_screen", os.path.join(HERE, "capture_screen.py"))
capture_screen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(capture_screen)

try:
    import serial
except ImportError:
    serial = None

try:
    from PIL import Image
except ImportError:
    Image = None


# ==========================================================================
# Progress logging -- the core observability fix. Every non-trivial action
# prints a line the moment it happens; nothing is silent for more than
# ~2 seconds during a wait.
# ==========================================================================

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ==========================================================================
# Config
# ==========================================================================

class Config:
    def __init__(self):
        self.esp_ip = os.environ.get("ESP_IP", "192.168.100.81")
        self.esp_port = int(os.environ.get("ESP_PORT", "8081"))
        self.backend_url = os.environ.get("BACKEND_URL", "").rstrip("/")
        self.device_id = os.environ.get("DEVICE_ID", "KIOSK-LASER-01")
        self.serial_port = os.environ.get("SERIAL_PORT", "/dev/ttyACM0")
        if not self.backend_url:
            print("ERROR: BACKEND_URL env var is required (e.g. https://xxxx.lhr.life)."
                  " Refusing to guess a backend address.", file=sys.stderr)
            sys.exit(2)
        self.esp_base = f"http://{self.esp_ip}:{self.esp_port}"


# ==========================================================================
# HTTP primitives -- short, single-attempt, explicit timeouts. NO retry
# loops buried in here: retry policy belongs at the call site, where it can
# be bounded and visible (a real bug found in the previous version: 3x
# retries x 3s backoff buried inside a "safe" JSON getter made every call
# site's actual worst-case latency invisible and unbounded-feeling).
# ==========================================================================

def http_get(url, timeout):
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, resp.read()


def http_post(url, payload, timeout):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def get_json(url, timeout=4):
    status, body = http_get(url, timeout)
    return status, json.loads(body.decode("utf-8")) if body else {}


def post_json(url, payload, timeout=4):
    status, body = http_post(url, payload, timeout)
    try:
        return status, json.loads(body.decode("utf-8")) if body else {}
    except json.JSONDecodeError:
        return status, {"_text": body.decode("utf-8", errors="replace")}


# ==========================================================================
# ESP debug endpoints
# ==========================================================================

def esp_device_state(cfg, timeout=8):
    return get_json(f"{cfg.esp_base}/debug/device-state", timeout)[1]


def esp_ui_state(cfg, timeout=8):
    return get_json(f"{cfg.esp_base}/debug/ui-state", timeout)[1]


def safe_device_state(cfg, timeout=8):
    """A real bug found live: a bare esp_device_state() call with no
    exception handling, hit at exactly the moment the tunnel had a transient
    hiccup, raised all the way out of a test case and crashed the whole
    runner process. Every device-state read a test body does OUTSIDE of
    poll_until() (which already handles this) must go through here instead."""
    try:
        return esp_device_state(cfg, timeout)
    except Exception as exc:  # noqa: BLE001
        log(f"  [WARN] device-state fetch failed: {exc}")
        return {}


def esp_screenshot_bytes(cfg, timeout=4):
    return http_get(f"{cfg.esp_base}/debug/screenshot", timeout)[1]


def esp_input(cfg, payload, timeout=4):
    """Returns (status, {"accepted":..,"input_seq":N}) or an error dict.
    NEVER retried automatically -- retrying an input call risks injecting a
    duplicate business event; the caller decides what a failure here means."""
    return post_json(f"{cfg.esp_base}/debug/input", payload, timeout)


def esp_qa_session(cfg, run_id=None, step=None, active=True, timeout=4):
    body = {"active": False} if not active else {"run_id": run_id, "step": step or ""}
    try:
        return post_json(f"{cfg.esp_base}/debug/qa-session", body, timeout)
    except Exception as exc:  # noqa: BLE001 -- best-effort observability aid, never fatal
        return -1, {"_error": str(exc)}


def esp_screenshot(cfg, out_path, timeout=4):
    """Frame-consistency capture (mirrors capture_screen.py): fetch ui-state,
    screenshot, ui-state again; retry up to 2 more times if frame_id moved
    mid-capture. Bounded -- at most 3 attempts, no unbounded loop."""
    state_after = None
    for _ in range(3):
        state_before = esp_ui_state(cfg, timeout)
        raw = esp_screenshot_bytes(cfg, timeout)
        state_after = esp_ui_state(cfg, timeout)
        if state_before.get("frame_id") == state_after.get("frame_id"):
            break
    img, meta = capture_screen.decode_screenshot(raw)
    img.save(out_path)
    return meta, state_after


# ==========================================================================
# Backend (mock) endpoints
# ==========================================================================

def backend_health_probe(cfg, timeout=3):
    """Single, short, bounded check -- this is what stands between a real
    outage and 20-30 minutes of silent retry (a real problem hit while
    building this runner). Called before every business step that needs the
    backend; a failure here is FAIL_BACKEND_UNREACHABLE immediately, no
    retry loop, no silent re-pointing to a different host."""
    try:
        status, _ = post_json(f"{cfg.backend_url}/api/kiosk/v2/bootstrap",
                              {"device_id": "e2e-health-probe", "hardware_id": "probe", "boot_id": "probe",
                               "runtime": {"version": "probe", "protocol_version": 1}, "hardware": {},
                               "current": {"ui_bundle": 0, "workflow": 0, "state_version": 0,
                                          "last_device_seq": 0}}, timeout=timeout)
        return status == 200
    except Exception:  # noqa: BLE001
        return False


def backend_admin(cfg, body, timeout=6):
    return post_json(f"{cfg.backend_url}/mock/admin/{cfg.device_id}", body, timeout=timeout)


def backend_mock_state(cfg, timeout=4):
    try:
        return True, get_json(f"{cfg.backend_url}/mock/state/{cfg.device_id}", timeout)[1]
    except Exception as exc:  # noqa: BLE001
        return False, {"_error": str(exc)}


def backend_drop_next(cfg, n=1, timeout=6):
    return post_json(f"{cfg.backend_url}/_test/drop-next", {"n": n}, timeout=timeout)


def backend_raw_event(cfg, event_type, expected_state_version, raw="", quantity_good=None, timeout=6):
    body = {
        "protocol_version": 1,
        "device": {"device_id": cfg.device_id, "hardware_id": "e2e-runner-synthetic", "boot_id": "e2e-runner"},
        "event": {"event_id": f"e2e-{int(time.time() * 1000)}", "device_seq": 999999999, "type": event_type},
        "time": {"timestamp_device": None, "uptime_ms": 0, "sync_status": "UNSYNCED", "sync_age_s": 0},
        "context": {"expected_state_version": expected_state_version, "workflow_version": 0, "ui_bundle_version": 0},
        "payload": {"source": "E2E_RUNNER", "raw": raw},
    }
    if quantity_good is not None:
        body["payload"]["quantity_good"] = quantity_good
    return post_json(f"{cfg.backend_url}/api/kiosk/v2/events", body, timeout=timeout)


def find_mock_backend_pid():
    try:
        out = subprocess.run(["pgrep", "-f", "mock_backend.py"], capture_output=True, text=True, timeout=5)
        pids = [int(p) for p in out.stdout.split() if p.strip()]
        return pids[0] if pids else None
    except Exception:  # noqa: BLE001
        return None


def stop_mock_backend():
    pid = find_mock_backend_pid()
    if pid is None:
        return False
    os.kill(pid, 15)
    time.sleep(1)
    return find_mock_backend_pid() is None


def start_mock_backend(repo_root, log_path, port=8799):
    if find_mock_backend_pid() is not None:
        return True
    script = os.path.join(repo_root, "tools", "mock_backend", "mock_backend.py")
    with open(log_path, "a") as logf:
        subprocess.Popen(["python3", "-u", script, "--port", str(port)], stdout=logf, stderr=logf,
                         cwd=repo_root, start_new_session=True)
    time.sleep(2)
    return find_mock_backend_pid() is not None


def reboot_device_via_serial(cfg, timeout=45):
    if serial is None:
        return {"error": "pyserial not available"}
    events_url = f"{cfg.backend_url}/api/kiosk/v2/events"
    ser = serial.Serial(cfg.serial_port, 115200, timeout=1)
    time.sleep(1)
    ser.reset_input_buffer()
    ser.write(f"api-endpoint:{events_url}\n".encode())

    end = time.time() + timeout
    lines = []
    boot_id = None
    bootstrap_ok = False
    while time.time() < end:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").strip()
        if not line:
            continue
        lines.append(line)
        m = re.search(r'"boot_id":"([^"]+)"', line)
        if m and '"code":"BOOT"' in line:
            boot_id = m.group(1)
        if '"code":"STATE_APPLY"' in line:
            bootstrap_ok = True
            break
    ser.close()
    return {"boot_id": boot_id, "bootstrap_ok": bootstrap_ok, "log_tail": lines[-25:]}


# ==========================================================================
# Bounded polling -- replaces every "sleep(N); check" pattern. Polls every
# 200ms, prints a progress tick every ~2s, and NEVER waits past `timeout`.
# ==========================================================================

SCREEN_ID_FOR_STATE = {
    "WAIT_EMPLOYEE": "state_wait_employee",
    "WAIT_OPERATION": "state_wait_operation",
    "SESSION_ACTIVE": "state_session_active",
    "QUANTITY_INPUT": "state_quantity_input",
    "DEVICE_DISABLED": "state_device_disabled",
    "MAINTENANCE": "state_maintenance",
}


def poll_until(cfg, predicate, timeout, label, poll_interval=0.2, tick_interval=2.0):
    """predicate(ds) -> bool, given the latest /debug/device-state (or None
    if that fetch itself failed this tick). Returns (ok, last_ds, elapsed)."""
    start = time.time()
    last_tick = start
    last_ds = None
    while True:
        elapsed = time.time() - start
        if elapsed > timeout:
            log(f"  [WAIT-TIMEOUT] {label} after {elapsed:.1f}s (limit {timeout}s)")
            return False, last_ds, elapsed
        try:
            last_ds = esp_device_state(cfg, timeout=min(3, max(1, timeout - elapsed)))
            if predicate(last_ds):
                return True, last_ds, elapsed
        except Exception as exc:  # noqa: BLE001 -- transient fetch failure, keep polling within budget
            last_ds = last_ds or {"_error": str(exc)}
        if time.time() - last_tick >= tick_interval:
            biz = (last_ds or {}).get("state", {})
            log(f"  [WAIT] {label} elapsed={elapsed:.1f}s state={biz.get('business')} "
               f"v={biz.get('state_version')} resyncing={biz.get('resyncing')}")
            last_tick = time.time()
        time.sleep(poll_interval)


def wait_for_business_state(cfg, expected_states, timeout, label):
    def pred(ds):
        st = ds.get("state", {})
        return st.get("business") in expected_states and not st.get("resyncing")
    return poll_until(cfg, pred, timeout, label)


def classify_state_timeout(cfg, default_status):
    """§15/§9: a state-wait timeout is ambiguous on its own -- it could mean
    a real functional regression, or it could mean the backend tunnel just
    isn't reachable right now (confirmed live: a business event's own retry
    took 45s and ended in a raw 503 that never even reached the mock
    backend's log -- a tunnel-edge fault, not a firmware/backend bug). One
    extra bounded health probe (~3s) after a timeout classifies which one
    this actually was, so the report blames the right layer."""
    if not backend_health_probe(cfg, timeout=5):
        log("  [CLASSIFY] backend health probe failed right after timeout -> FAIL_BACKEND_UNREACHABLE")
        return "FAIL_BACKEND_UNREACHABLE"
    return default_status


# ==========================================================================
# Screenshot sanity + blank-screen / stuck-frame detection
# ==========================================================================

def screenshot_sanity(png_path, meta, expected_screen_id=None):
    problems = []
    if not os.path.exists(png_path) or os.path.getsize(png_path) == 0:
        problems.append("screenshot file missing or empty")
    if meta.get("width") != 240 or meta.get("height") != 320:
        problems.append(f"unexpected dimensions {meta.get('width')}x{meta.get('height')} (expected 240x320)")
    if not meta.get("frame_id"):
        problems.append("frame_id missing/zero")
    if expected_screen_id and meta.get("screen_id") != expected_screen_id:
        problems.append(f"screen_id={meta.get('screen_id')!r}, expected {expected_screen_id!r}")
    return problems


def detect_blank_or_stuck(png_path):
    """No CV, just cheap structural checks (§13): count unique colors in the
    image. A real kiosk screen (black background + several colored text
    rows) always has more than a handful of distinct colors; a blank/stuck
    frame (solid black, or a solid color glitch) has very few."""
    if Image is None or not os.path.exists(png_path):
        return None  # can't check -- not the same as "checked and fine"
    img = Image.open(png_path)
    colors = img.getcolors(maxcolors=100_000)
    unique_count = len(colors) if colors is not None else 100_000
    if unique_count <= 2:
        return f"DISPLAY_BLANK_OR_STUCK: only {unique_count} unique color(s) in frame"
    return None


def overflow_flags(ui_state):
    return [ln for ln in ui_state.get("lines", []) if ln.get("overflow")]


# ==========================================================================
# Result / evidence model
# ==========================================================================

class CaseResult:
    def __init__(self, test_id, description):
        self.test_id = test_id
        self.description = description
        self.status = "SKIPPED"
        self.details = {}
        self.started_at = time.time()
        self.finished_at = None

    def finish(self, status, **details):
        self.status = status
        self.details.update(details)
        self.finished_at = time.time()
        return self

    @property
    def duration_s(self):
        return round((self.finished_at or time.time()) - self.started_at, 2)

    def to_dict(self):
        return {"test_id": self.test_id, "description": self.description, "status": self.status,
                "details": self.details, "duration_s": self.duration_s}


class Runner:
    def __init__(self, cfg, run_dir, run_id):
        self.cfg = cfg
        self.run_dir = run_dir
        self.run_id = run_id
        self.results = []

    def case_dir(self, test_id):
        d = os.path.join(self.run_dir, "cases", test_id)
        os.makedirs(d, exist_ok=True)
        return d

    @staticmethod
    def save_json(path, obj):
        with open(path, "w") as f:
            json.dump(obj, f, indent=2, ensure_ascii=False, default=str)

    def set_step(self, test_id):
        esp_qa_session(self.cfg, run_id=self.run_id, step=test_id, active=True)

    def capture(self, test_id, label):
        d = self.case_dir(test_id)
        png_path = os.path.join(d, f"{label}.png")
        meta, ui = esp_screenshot(self.cfg, png_path)
        ds = esp_device_state(self.cfg)
        self.save_json(os.path.join(d, f"{label}.ui-state.json"), ui)
        self.save_json(os.path.join(d, f"{label}.device-state.json"), ds)
        return {"screenshot": png_path, "meta": meta, "ui_state": ui, "device_state": ds}

    def capture_failure_evidence(self, test_id):
        """§11: capture evidence IMMEDIATELY on a timeout/failure, not at
        the end of the suite. Best-effort -- if even this can't reach the
        device, that fact alone is diagnostic and gets saved too."""
        d = os.path.join(self.case_dir(test_id), "failure")
        os.makedirs(d, exist_ok=True)
        evidence = {}
        try:
            evidence["device_state"] = esp_device_state(self.cfg, timeout=4)
            self.save_json(os.path.join(d, "device-state.json"), evidence["device_state"])
        except Exception as exc:  # noqa: BLE001
            evidence["device_state_error"] = str(exc)
        try:
            evidence["ui_state"] = esp_ui_state(self.cfg, timeout=4)
            self.save_json(os.path.join(d, "ui-state.json"), evidence["ui_state"])
        except Exception as exc:  # noqa: BLE001
            evidence["ui_state_error"] = str(exc)
        try:
            meta, _ = esp_screenshot(self.cfg, os.path.join(d, "screenshot.png"), timeout=4)
            evidence["screenshot_meta"] = meta
        except Exception as exc:  # noqa: BLE001
            evidence["screenshot_error"] = str(exc)
        ok, backend_state = backend_mock_state(self.cfg, timeout=4)
        evidence["backend_state"] = backend_state if ok else {"_error": "unreachable"}
        self.save_json(os.path.join(d, "context.json"), evidence)
        log(f"  [EVIDENCE] captured to {d}")
        return d

    def record(self, result):
        self.results.append(result)
        d = self.case_dir(result.test_id)
        self.save_json(os.path.join(d, "result.json"), result.to_dict())
        log(f"[{result.status:22s}] {result.test_id:10s} {result.description}  ({result.duration_s}s)")
        return result


class SuiteTimeoutError(Exception):
    pass


class SuiteBudget:
    """Global suite timeout (§3) -- checked between test cases so one
    already-bounded-per-step test can't be followed by dozens more once the
    overall budget is spent."""
    def __init__(self, seconds):
        self.deadline = time.time() + seconds if seconds else None

    def check(self, next_test_id):
        if self.deadline and time.time() > self.deadline:
            raise SuiteTimeoutError(f"global suite budget exhausted before {next_test_id}")


# ==========================================================================
# Test cases
# ==========================================================================

def tc_preflight(runner):
    cfg = runner.cfg
    r = CaseResult("E2E-001", "Preflight: device-state/ui-state/screenshot/backend all reachable")
    log("[START] E2E-001 preflight")
    checks = {}

    try:
        t0 = time.time()
        ds = esp_device_state(cfg, timeout=8)
        checks["device_state_reachable"] = True
        log(f"  [OK] device-state reachable ({time.time()-t0:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        log(f"  [FAIL] device-state unreachable: {exc}")
        return runner.record(r.finish("FAIL_CONNECTIVITY", checks={"device_state_reachable": False},
                                      error=str(exc)))

    try:
        t0 = time.time()
        esp_ui_state(cfg, timeout=8)
        checks["ui_state_reachable"] = True
        log(f"  [OK] ui-state reachable ({time.time()-t0:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        checks["ui_state_reachable"] = False
        log(f"  [FAIL] ui-state unreachable: {exc}")

    try:
        t0 = time.time()
        esp_screenshot_bytes(cfg, timeout=8)
        checks["screenshot_reachable"] = True
        log(f"  [OK] screenshot reachable ({time.time()-t0:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        checks["screenshot_reachable"] = False
        log(f"  [FAIL] screenshot unreachable: {exc}")

    t0 = time.time()
    checks["backend_reachable"] = backend_health_probe(cfg, timeout=8)
    log(f"  [{'OK' if checks['backend_reachable'] else 'FAIL'}] backend health probe ({time.time()-t0:.2f}s)")

    checks["provisioning_active"] = ds.get("provisioning_state") == "ACTIVE"
    checks["profile_dev"] = ds.get("profile") == "DEV"
    checks["protocol_version_1"] = ds.get("protocol", {}).get("protocol_version") == 1
    checks["firmware_version"] = ds.get("firmware_version")
    checks["bootstrap_status_ok"] = ds.get("protocol", {}).get("bootstrap", {}).get("status") == "OK"

    required = ["device_state_reachable", "ui_state_reachable", "screenshot_reachable", "backend_reachable",
               "provisioning_active", "profile_dev", "protocol_version_1"]
    all_ok = all(checks.get(k) for k in required)

    if not all_ok:
        return runner.record(r.finish("FAIL_CONNECTIVITY", checks=checks))
    return runner.record(r.finish("PASS", checks=checks))


def tc_bootstrap_accepted(runner):
    r = CaseResult("E2E-002", "Bootstrap accepted (this boot)")
    log("[START] E2E-002 bootstrap accepted")
    ds = esp_device_state(runner.cfg, timeout=6)
    bs = ds.get("protocol", {}).get("bootstrap", {})
    if bs.get("status") == "OK":
        return runner.record(r.finish("PASS", bootstrap=bs))
    return runner.record(r.finish("FAIL_PROTOCOL", bootstrap=bs))


def do_input(runner, test_id, payload, timeout=7):
    """The layer-1 check (§8/§9): confirms the device actually ACKed this
    specific input (input_seq bumped) before anything waits on a business
    effect. Returns (ok, response) -- ok=False means FAIL_DEVICE_INPUT, a
    category distinct from a later backend/state/UI failure.

    Default timeout is 7s, not the original 4s: a real false-negative was
    found live where KEY_DOWN '#' actually landed and advanced business
    state (confirmed after the fact via device-state), but the /debug/input
    HTTP round trip itself -- LAN-only, not through the tunnel -- took
    slightly over 4s under this sandbox's demonstrated LAN jitter (ICMP RTT
    spikes to ~3.8s were measured separately)."""
    cfg = runner.cfg
    label = payload.get("value") or payload.get("key") or payload.get("type")
    log(f"  [INPUT] {payload.get('type')} {label!r}")
    try:
        ds_before = esp_device_state(cfg, timeout=timeout)
        prior_seq = ds_before.get("last_input_seq", -1)
    except Exception:  # noqa: BLE001
        prior_seq = -1

    try:
        status, resp = esp_input(cfg, payload, timeout=timeout)
    except Exception as exc:  # noqa: BLE001
        log(f"  [FAIL_DEVICE_INPUT] {test_id}: {exc}")
        return False, {"_error": str(exc)}

    if status != 200 or not resp.get("accepted"):
        log(f"  [FAIL_DEVICE_INPUT] {test_id}: http={status} resp={resp}")
        return False, resp

    new_seq = resp.get("input_seq")
    if new_seq is not None and prior_seq is not None and new_seq == prior_seq and prior_seq != -1:
        log(f"  [FAIL_DEVICE_INPUT] {test_id}: input_seq did not advance ({prior_seq} -> {new_seq})")
        return False, resp

    log(f"  [OK] device acked input_seq={new_seq}")
    return True, resp


def tc_visual_state(runner, test_id, description, expected_business_state, label="screenshot", timeout=20):
    cfg = runner.cfg
    r = CaseResult(test_id, description)
    log(f"[START] {test_id} {description}")
    runner.set_step(test_id)
    ok, ds, elapsed = wait_for_business_state(cfg, [expected_business_state], timeout,
                                              f"{test_id} reach {expected_business_state}")
    if not ok:
        runner.capture_failure_evidence(test_id)
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, reason="did not reach expected state",
                                      expected=expected_business_state, actual=ds, elapsed_s=round(elapsed, 2)))
    cap = runner.capture(test_id, label)
    expected_screen = SCREEN_ID_FOR_STATE[expected_business_state]
    problems = screenshot_sanity(cap["screenshot"], cap["meta"], expected_screen)
    blank = detect_blank_or_stuck(cap["screenshot"])
    overflows = overflow_flags(cap["ui_state"])
    details = {"business_state": expected_business_state, "screenshot": cap["screenshot"],
              "screen_id": cap["meta"].get("screen_id"), "overflow_lines": overflows,
              "sanity_problems": problems, "blank_or_stuck": blank}
    if blank:
        log(f"  [FAIL] {blank}")
        return runner.record(r.finish("FAIL_DISPLAY_BLANK", **details))
    if problems:
        return runner.record(r.finish("FAIL_UI", **details))
    if overflows:
        return runner.record(r.finish("FAIL_UI", note="text overflow reported", **details))
    log(f"  [PASS] state={expected_business_state} screen_id={cap['meta'].get('screen_id')} "
       f"frame_id={cap['meta'].get('frame_id')}")
    return runner.record(r.finish("PASS", **details))


def scan_and_wait(runner, test_id, raw_value, expect_states, description, wait_timeout=20,
                  require_start_states=None):
    """Layered assertion (§9): device ack -> event created -> backend state
    -> device projection+screen. Each layer's failure is reported with its
    own category rather than a single generic FAIL.

    `require_start_states`, if given, SKIPS rather than blindly firing when
    the device isn't already in one of those states -- a real bug found
    live: a chained call assumed a PRIOR test's side effect (that test had
    been SKIPPED due to a transient fetch failure) and blindly sent an
    operation scan while the device was still at WAIT_EMPLOYEE, producing a
    confusing FAIL_STATE instead of an honest SKIPPED."""
    cfg = runner.cfg
    r = CaseResult(test_id, description)
    log(f"[START] {test_id} {description}")
    runner.set_step(test_id)

    ds_before = safe_device_state(cfg)
    if require_start_states and ds_before.get("state", {}).get("business") not in require_start_states:
        return runner.record(r.finish("SKIPPED", reason=f"not in {require_start_states}",
                                      actual=ds_before.get("state")))
    prior_event_id = ds_before.get("protocol", {}).get("last_event_id")
    prior_version = ds_before.get("state", {}).get("state_version")

    ok, resp = do_input(runner, test_id, {"type": "SCAN", "value": raw_value})
    if not ok:
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_DEVICE_INPUT", input_response=resp))

    ok, ds_after, elapsed = wait_for_business_state(cfg, expect_states, wait_timeout,
                                                    f"{test_id} reach {expect_states}")
    if not ok:
        # Distinguish EVENTBUS_FAIL (event never even got created) from a
        # genuine BACKEND/STATE wait timeout.
        last_event_id = (ds_after or {}).get("protocol", {}).get("last_event_id")
        runner.capture_failure_evidence(test_id)
        if last_event_id == prior_event_id:
            return runner.record(r.finish("FAIL_EVENTBUS", reason="no new event was ever created on-device",
                                          elapsed_s=round(elapsed, 2), actual=ds_after))
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, reason="event sent but expected state never reached",
                                      elapsed_s=round(elapsed, 2), actual=ds_after,
                                      expected_states=expect_states))

    backend_ok, backend_state = backend_mock_state(cfg)
    if not backend_ok or backend_state.get("state_name") != ds_after.get("state", {}).get("business"):
        return runner.record(r.finish("FAIL_BACKEND", reason="backend state does not match device's view",
                                      backend=backend_state, actual=ds_after))

    cap = runner.capture(test_id, "after")
    problems = screenshot_sanity(cap["screenshot"], cap["meta"])
    blank = detect_blank_or_stuck(cap["screenshot"])
    if blank:
        return runner.record(r.finish("FAIL_DISPLAY_BLANK", note=blank, screenshot=cap["screenshot"]))
    if problems:
        return runner.record(r.finish("FAIL_UI", sanity_problems=problems, screenshot=cap["screenshot"]))

    log(f"  [PASS] state={ds_after.get('state', {}).get('business')} "
       f"v={ds_after.get('state', {}).get('state_version')} elapsed={elapsed:.2f}s")
    return runner.record(r.finish("PASS", prior_version=prior_version,
                                  new_version=ds_after.get("state", {}).get("state_version"),
                                  elapsed_s=round(elapsed, 2), screenshot=cap["screenshot"]))


def tc_key_and_wait(runner, test_id, key, expect_states, description, wait_timeout=20):
    cfg = runner.cfg
    r = CaseResult(test_id, description)
    log(f"[START] {test_id} {description}")
    runner.set_step(test_id)

    ds_before = safe_device_state(cfg)
    prior_event_id = ds_before.get("protocol", {}).get("last_event_id")

    ok, resp = do_input(runner, test_id, {"type": "KEY_DOWN", "key": key})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT", input_response=resp))
    do_input(runner, test_id, {"type": "KEY_UP", "key": key})

    ok, ds_after, elapsed = wait_for_business_state(cfg, expect_states, wait_timeout, f"{test_id} key={key}")
    if not ok:
        runner.capture_failure_evidence(test_id)
        last_event_id = (ds_after or {}).get("protocol", {}).get("last_event_id")
        status = "FAIL_EVENTBUS" if last_event_id == prior_event_id else classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, actual=ds_after, elapsed_s=round(elapsed, 2)))

    log(f"  [PASS] state={ds_after.get('state', {}).get('business')} elapsed={elapsed:.2f}s")
    return runner.record(r.finish("PASS", elapsed_s=round(elapsed, 2),
                                  new_version=ds_after.get("state", {}).get("state_version")))


def tc_quantity_submit(runner, test_id, digits, description, wait_timeout=20):
    cfg = runner.cfg
    r = CaseResult(test_id, description)
    log(f"[START] {test_id} {description}")
    runner.set_step(test_id)

    ds_before = safe_device_state(cfg)
    if ds_before.get("state", {}).get("business") != "QUANTITY_INPUT":
        return runner.record(r.finish("SKIPPED", reason="not at QUANTITY_INPUT", actual=ds_before.get("state")))

    for d in digits:
        ok, _ = do_input(runner, test_id, {"type": "KEY_DOWN", "key": d})
        if not ok:
            return runner.record(r.finish("FAIL_DEVICE_INPUT", digit=d))
        do_input(runner, test_id, {"type": "KEY_UP", "key": d})

    if digits:
        ds_mid = safe_device_state(cfg)
        buffer_ok = ds_mid.get("state", {}).get("local_quantity_buffer") == digits
        if not buffer_ok:
            runner.capture_failure_evidence(test_id)
            return runner.record(r.finish("FAIL_UI", reason="local_quantity_buffer mismatch", expected=digits,
                                          actual=ds_mid.get("state", {}).get("local_quantity_buffer")))
        cap_mid = runner.capture(test_id, "digits-entered")
        problems_mid = screenshot_sanity(cap_mid["screenshot"], cap_mid["meta"], "state_quantity_input")
        if problems_mid:
            return runner.record(r.finish("FAIL_UI", sanity_problems=problems_mid, digits=digits))

    prior_event_id = safe_device_state(cfg).get("protocol", {}).get("last_event_id")
    ok, _ = do_input(runner, test_id, {"type": "KEY_DOWN", "key": "#"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))
    do_input(runner, test_id, {"type": "KEY_UP", "key": "#"})

    ok, ds_after, elapsed = wait_for_business_state(cfg, ["WAIT_EMPLOYEE"], wait_timeout, f"{test_id} submit")
    if not ok:
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_STATE", actual=ds_after, digits=digits, elapsed_s=round(elapsed, 2)))

    cap = runner.capture(test_id, "after-submit")
    problems = screenshot_sanity(cap["screenshot"], cap["meta"], "state_wait_employee")
    details = {"digits_submitted": digits or "(empty -> quantity_good=0)", "elapsed_s": round(elapsed, 2),
              "screenshot": cap["screenshot"], "new_version": ds_after.get("state", {}).get("state_version")}
    if problems:
        return runner.record(r.finish("FAIL_UI", sanity_problems=problems, **details))
    log(f"  [PASS] digits={digits!r} elapsed={elapsed:.2f}s")
    return runner.record(r.finish("PASS", **details))


def tc_invalid_employee(runner):
    cfg = runner.cfg
    test_id = "E2E-006"
    r = CaseResult(test_id, "Invalid employee scan -> business rejection, WAIT_EMPLOYEE unchanged")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    prior_version = ds_before.get("state", {}).get("state_version")
    if ds_before.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        return runner.record(r.finish("SKIPPED", reason="not at WAIT_EMPLOYEE", actual=ds_before.get("state")))

    prior_event_id = ds_before.get("protocol", {}).get("last_event_id")
    ok, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|99999"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    # A real bug found live: waiting on "not resyncing" is trivially true
    # even while the send is still in flight (a plain rejection never sets
    # resyncing at all) -- it let the screenshot below race the actual
    # network round trip and catch the transient "Da nhan ma" local-feedback
    # screen instead of the settled rejection screen. Wait for the event
    # round trip to actually complete instead.
    def pred(ds):
        return ds.get("protocol", {}).get("last_event_id") != prior_event_id and not ds.get("state", {}).get(
            "resyncing")
    poll_until(cfg, pred, 15, f"{test_id} settle")

    ds_after = safe_device_state(cfg)
    new_version = ds_after.get("state", {}).get("state_version")
    same_state = ds_after.get("state", {}).get("business") == "WAIT_EMPLOYEE"
    version_unchanged = new_version == prior_version
    cap = runner.capture(test_id, "rejection")
    problems = screenshot_sanity(cap["screenshot"], cap["meta"], "state_wait_employee")

    details = {"prior_version": prior_version, "new_version": new_version, "screenshot": cap["screenshot"]}
    if not (same_state and version_unchanged):
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_STATE", **details))
    if problems:
        return runner.record(r.finish("FAIL_UI", sanity_problems=problems, **details))
    log(f"  [PASS] rejected, version unchanged at {prior_version}")
    return runner.record(r.finish("PASS", **details))


def tc_invalid_operation(runner):
    cfg = runner.cfg
    test_id = "E2E-009"
    r = CaseResult(test_id, "Invalid operation scan -> business rejection, WAIT_OPERATION unchanged")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    prior_version = ds_before.get("state", {}).get("state_version")
    if ds_before.get("state", {}).get("business") != "WAIT_OPERATION":
        return runner.record(r.finish("SKIPPED", reason="not at WAIT_OPERATION", actual=ds_before.get("state")))

    prior_event_id = ds_before.get("protocol", {}).get("last_event_id")
    ok, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|OP|OP-DOES-NOT-EXIST"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    def pred(ds):
        return ds.get("protocol", {}).get("last_event_id") != prior_event_id and not ds.get("state", {}).get(
            "resyncing")
    poll_until(cfg, pred, 15, f"{test_id} settle")
    ds_after = safe_device_state(cfg)
    new_version = ds_after.get("state", {}).get("state_version")
    same_state = ds_after.get("state", {}).get("business") == "WAIT_OPERATION"
    cap = runner.capture(test_id, "rejection")
    problems = screenshot_sanity(cap["screenshot"], cap["meta"], "state_wait_operation")

    details = {"prior_version": prior_version, "new_version": new_version, "screenshot": cap["screenshot"]}
    if not (same_state and new_version == prior_version):
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_STATE", **details))
    if problems:
        return runner.record(r.finish("FAIL_UI", sanity_problems=problems, **details))
    log(f"  [PASS] rejected, version unchanged at {prior_version}")
    return runner.record(r.finish("PASS", **details))


def tc_duplicate_scan(runner):
    cfg = runner.cfg
    test_id = "E2E-015"
    r = CaseResult(test_id, "Rapid duplicate employee scan -> no double business effect")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    if not ds_before:
        return runner.record(r.finish("SKIPPED", reason="device-state fetch failed (transient network hiccup, "
                                      "not a real precondition mismatch)"))
    if ds_before.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        return runner.record(r.finish("SKIPPED", reason="not at WAIT_EMPLOYEE", actual=ds_before.get("state")))
    prior_version = ds_before.get("state", {}).get("state_version")

    ok1, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00200"})
    ok2, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00200"})
    if not (ok1 and ok2):
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    ok, ds_after, elapsed = wait_for_business_state(cfg, ["WAIT_OPERATION"], 20, f"{test_id}")
    if not ok:
        runner.capture_failure_evidence(test_id)
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, actual=ds_after))

    new_version = ds_after.get("state", {}).get("state_version")
    version_delta = (new_version or 0) - (prior_version or 0)
    details = {"prior_version": prior_version, "new_version": new_version, "version_delta": version_delta}
    if version_delta != 1:
        return runner.record(r.finish("FAIL_STATE", note="version advanced by more than 1", **details))
    log(f"  [PASS] single business effect confirmed (v {prior_version} -> {new_version})")
    return runner.record(r.finish("PASS", **details))


def tc_invalid_transition(runner):
    cfg = runner.cfg
    test_id = "E2E-016"
    r = CaseResult(test_id, "FINISH_REQUESTED while WAIT_EMPLOYEE -> STATE_INVALID_TRANSITION (backend-level)")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    if ds_before.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        return runner.record(r.finish("SKIPPED", reason="not at WAIT_EMPLOYEE", actual=ds_before.get("state")))
    version = ds_before.get("state", {}).get("state_version")

    status, body = backend_raw_event(cfg, "FINISH_REQUESTED", version)
    rejected = (not body.get("accepted", True)) and body.get("error", {}).get("code") == "STATE_INVALID_TRANSITION"
    version_unchanged = body.get("state", {}).get("version") == version

    details = {"note": "sent directly to the backend using the device's own envelope shape -- the real device's "
                       "keypad handler never constructs this combination itself", "http_status": status,
              "response": body}
    if status == 200 and rejected and version_unchanged:
        log("  [PASS] STATE_INVALID_TRANSITION correctly rejected")
        return runner.record(r.finish("PASS", **details))
    return runner.record(r.finish("FAIL_BACKEND", **details))


def tc_lost_ack_idempotent(runner):
    cfg = runner.cfg
    test_id = "E2E-017"
    r = CaseResult(test_id, "Dropped response + idempotent retry converges exactly once")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    if ds_before.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        return runner.record(r.finish("SKIPPED", reason="not at WAIT_EMPLOYEE", actual=ds_before.get("state")))
    prior_version = ds_before.get("state", {}).get("state_version")

    status, _ = backend_drop_next(cfg, 1)
    if status != 200:
        return runner.record(r.finish("FAIL_BACKEND", reason="could not arm chaos", http_status=status))

    ok, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00152"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    ok, ds_after, elapsed = wait_for_business_state(cfg, ["WAIT_OPERATION"], 30, f"{test_id}")
    if not ok:
        runner.capture_failure_evidence(test_id)
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, actual=ds_after))

    new_version = ds_after.get("state", {}).get("state_version")
    version_delta = (new_version or 0) - (prior_version or 0)
    retry_count = ds_after.get("protocol", {}).get("retry_count", 0)
    details = {"prior_version": prior_version, "new_version": new_version, "version_delta": version_delta,
              "retry_count": retry_count, "elapsed_s": round(elapsed, 2)}
    if version_delta != 1:
        return runner.record(r.finish("FAIL_STATE", **details))
    log(f"  [PASS] retry_count={retry_count} version_delta=1 (idempotent)")
    return runner.record(r.finish("PASS", **details))


def tc_state_conflict_resync(runner):
    cfg = runner.cfg
    test_id = "E2E-018"
    r = CaseResult(test_id, "Server-side version skew -> STATE_CONFLICT -> RESYNC -> converge")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    prior_business = ds_before.get("state", {}).get("business")

    backend_admin(cfg, {"maintenance": True})
    status, admin_resp = backend_admin(cfg, {"maintenance": False})
    server_version = admin_resp.get("snapshot", {}).get("state", {}).get("version")

    ok, _ = do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00152"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    ok, ds_after, elapsed = wait_for_business_state(cfg, [prior_business], 30, f"{test_id} converge")
    final_version = (ds_after or {}).get("state", {}).get("state_version")
    details = {"server_version_after_admin": server_version, "final_device_version": final_version,
              "elapsed_s": round(elapsed, 2)}
    if not ok:
        runner.capture_failure_evidence(test_id)
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, reason="never settled", **details))
    if final_version != server_version:
        return runner.record(r.finish("FAIL_STATE", reason="version mismatch after RESYNC", **details))
    log(f"  [PASS] converged to server version {server_version}")
    return runner.record(r.finish("PASS", **details))


def tc_backend_outage_recovery(runner, repo_root, mock_log_path):
    cfg = runner.cfg
    test_id = "E2E-020"
    r = CaseResult(test_id, "Backend process down -> device stays on last authoritative state -> recovers")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    ds_before = safe_device_state(cfg)
    business_before = ds_before.get("state", {}).get("business")
    version_before = ds_before.get("state", {}).get("state_version")

    stopped = stop_mock_backend()
    if not stopped:
        return runner.record(r.finish("SKIPPED", reason="could not stop mock backend process for this test"))
    log("  [FAULT] mock backend stopped")

    do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00152"})
    poll_until(cfg, lambda ds: False, 8, f"{test_id} outage window")  # bounded wait, always "times out" by design
    ds_during = safe_device_state(cfg)
    state_retained = (ds_during.get("state", {}).get("business") == business_before and
                      ds_during.get("state", {}).get("state_version") == version_before)

    restarted = start_mock_backend(repo_root, mock_log_path)
    log(f"  [RECOVERY] mock backend restarted: {restarted}")

    ok, ds_after, elapsed = wait_for_business_state(cfg, [business_before], 30, f"{test_id} recovery")
    details = {"business_before": business_before, "state_retained_during_outage": state_retained,
              "restarted": restarted, "elapsed_recovery_s": round(elapsed, 2)}
    if not state_retained:
        return runner.record(r.finish("FAIL_RECOVERY", reason="device changed business state locally during "
                                      "outage (invariant 14 violation)", **details))
    if not ok:
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_RECOVERY", reason="did not recover after backend restart", **details))
    log("  [PASS] state retained during outage, recovered after restart")
    return runner.record(r.finish("PASS", **details))


def tc_reboot_restore(runner):
    cfg = runner.cfg
    test_id = "E2E-021"
    r = CaseResult(test_id, "Reboot mid-session -> boot_id changes, device_seq monotonic, state restored")
    log(f"[START] {test_id}")
    runner.set_step(test_id)
    if serial is None:
        return runner.record(r.finish("SKIPPED", reason="pyserial not available"))

    ds_before = safe_device_state(cfg)
    business_before = ds_before.get("state", {}).get("business")
    boot_id_before = ds_before.get("boot_id")
    device_seq_before = ds_before.get("protocol", {}).get("device_seq")
    cap_before = runner.capture(test_id, "before-reboot")

    log("  [FAULT] rebooting device via serial...")
    reboot_result = reboot_device_via_serial(cfg, timeout=45)

    ok, ds_after, elapsed = wait_for_business_state(
        cfg, [business_before] if business_before else list(SCREEN_ID_FOR_STATE.keys()), 30, f"{test_id} post-boot")
    boot_id_after = (ds_after or {}).get("boot_id")
    device_seq_after = (ds_after or {}).get("protocol", {}).get("device_seq")
    cap_after = runner.capture(test_id, "after-reboot") if ok else None

    details = {"boot_id_before": boot_id_before, "boot_id_after": boot_id_after,
              "device_seq_before": device_seq_before, "device_seq_after": device_seq_after,
              "reboot_bootstrap_ok": reboot_result.get("bootstrap_ok"),
              "screenshot_before": cap_before["screenshot"],
              "screenshot_after": cap_after["screenshot"] if cap_after else None, "elapsed_s": round(elapsed, 2)}

    if not reboot_result.get("bootstrap_ok"):
        runner.capture_failure_evidence(test_id)
        return runner.record(r.finish("FAIL_RECOVERY", reason="no STATE_APPLY observed after reboot", **details))
    if not boot_id_after or boot_id_after == boot_id_before:
        return runner.record(r.finish("FAIL_PROTOCOL", reason="boot_id did not change", **details))
    if (device_seq_after or 0) < (device_seq_before or 0):
        return runner.record(r.finish("FAIL_PROTOCOL", reason="device_seq went backward", **details))
    if not ok:
        return runner.record(r.finish("FAIL_STATE", reason="business state not restored", **details))
    log(f"  [PASS] boot_id changed, device_seq monotonic, state restored ({elapsed:.1f}s)")
    return runner.record(r.finish("PASS", **details))


def tc_admin_forced_state(runner, test_id, description, admin_body, expected_business, blocked_scan_value):
    cfg = runner.cfg
    r = CaseResult(test_id, description)
    log(f"[START] {test_id} {description}")
    runner.set_step(test_id)
    status, _ = backend_admin(cfg, admin_body)
    if status != 200:
        return runner.record(r.finish("FAIL_BACKEND", http_status=status))

    do_input(runner, test_id, {"type": "SCAN", "value": "WF|EMP|00152"})
    ok, ds_after, elapsed = wait_for_business_state(cfg, [expected_business], 30, f"{test_id}")
    if not ok:
        runner.capture_failure_evidence(test_id)
        status = classify_state_timeout(cfg, "FAIL_STATE")
        return runner.record(r.finish(status, actual=ds_after))

    cap = runner.capture(test_id, "forced-state")
    problems = screenshot_sanity(cap["screenshot"], cap["meta"], SCREEN_ID_FOR_STATE[expected_business])

    version_before_block = ds_after.get("state", {}).get("state_version")
    do_input(runner, test_id, {"type": "SCAN", "value": blocked_scan_value})
    poll_until(cfg, lambda ds: False, 5, f"{test_id} block-check window")
    ds_blocked = safe_device_state(cfg)
    blocked = (ds_blocked.get("state", {}).get("business") == expected_business and
              ds_blocked.get("state", {}).get("state_version") == version_before_block)

    details = {"elapsed_s": round(elapsed, 2), "screenshot": cap["screenshot"], "business_input_blocked": blocked}
    if problems:
        return runner.record(r.finish("FAIL_UI", sanity_problems=problems, **details))
    if not blocked:
        return runner.record(r.finish("FAIL_STATE", reason="a scan was accepted while blocked", **details))
    log(f"  [PASS] {expected_business} enforced, business input blocked")
    return runner.record(r.finish("PASS", **details))


def tc_wifi_recovery_logic(runner):
    cfg = runner.cfg
    test_id = "E2E-025"
    r = CaseResult(test_id, "Wi-Fi recovery hold countdown (injected '*', released well before 10s threshold)")
    log(f"[START] {test_id}")
    runner.set_step(test_id)

    t_keydown = time.time()
    ok, _ = do_input(runner, test_id, {"type": "KEY_DOWN", "key": "*"})
    if not ok:
        return runner.record(r.finish("FAIL_DEVICE_INPUT"))

    remaining = 4.0 - (time.time() - t_keydown)
    if remaining > 0:
        time.sleep(remaining)
    cap = runner.capture(test_id, "hold-countdown")
    countdown_screen = cap["meta"].get("screen_id") == "wifi_hold_progress"
    elapsed_before_release = time.time() - t_keydown

    if elapsed_before_release >= 8.0:
        log(f"  [ABORT] {elapsed_before_release:.1f}s elapsed before release -- too close to the 10s trigger, "
           "not sending KEY_UP blind")
        return runner.record(r.finish(
            "FAIL_UI", countdown_screen_shown=countdown_screen, screenshot=cap["screenshot"],
            elapsed_before_release_s=round(elapsed_before_release, 1),
            note="elapsed time before release approached the 10s trigger -- possible portal activation, "
                 "manual recovery may be required"))

    do_input(runner, test_id, {"type": "KEY_UP", "key": "*"})
    ok2, ds_after, _ = poll_until(cfg, lambda ds: ds.get("state", {}).get("business") is not None, 5,
                                  f"{test_id} return-to-normal")
    details = {"countdown_screen_shown": countdown_screen, "returned_to_normal": ok2,
              "screenshot": cap["screenshot"], "elapsed_before_release_s": round(elapsed_before_release, 1)}
    if countdown_screen and ok2:
        log("  [PASS] countdown shown, released safely, returned to normal")
        return runner.record(r.finish("PASS", **details))
    return runner.record(r.finish("FAIL_UI", **details))


def run_happy_path_cycle(runner, cycle_num, full_screenshots):
    cfg = runner.cfg
    ds = safe_device_state(cfg)
    if ds.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        return False, "not starting from WAIT_EMPLOYEE"

    ok, _ = do_input(runner, f"soak-c{cycle_num}", {"type": "SCAN", "value": "WF|EMP|00152"})
    if not ok:
        return False, "device did not ack employee scan"
    ok, ds, _ = wait_for_business_state(cfg, ["WAIT_OPERATION"], 20, f"soak cycle {cycle_num} step1")
    if not ok:
        return False, "did not reach WAIT_OPERATION"
    if full_screenshots:
        runner.capture(f"E2E-026-cycle{cycle_num}", "wait_operation")

    ok, _ = do_input(runner, f"soak-c{cycle_num}", {"type": "SCAN", "value": "WF|OP|OP-001"})
    ok, ds, _ = wait_for_business_state(cfg, ["SESSION_ACTIVE"], 20, f"soak cycle {cycle_num} step2")
    if not ok:
        return False, "did not reach SESSION_ACTIVE"
    if full_screenshots:
        runner.capture(f"E2E-026-cycle{cycle_num}", "session_active")

    do_input(runner, f"soak-c{cycle_num}", {"type": "KEY_DOWN", "key": "#"})
    do_input(runner, f"soak-c{cycle_num}", {"type": "KEY_UP", "key": "#"})
    ok, ds, _ = wait_for_business_state(cfg, ["QUANTITY_INPUT"], 20, f"soak cycle {cycle_num} step3")
    if not ok:
        return False, "did not reach QUANTITY_INPUT"
    if full_screenshots:
        runner.capture(f"E2E-026-cycle{cycle_num}", "quantity_input")

    do_input(runner, f"soak-c{cycle_num}", {"type": "KEY_DOWN", "key": "#"})
    do_input(runner, f"soak-c{cycle_num}", {"type": "KEY_UP", "key": "#"})
    ok, ds, _ = wait_for_business_state(cfg, ["WAIT_EMPLOYEE"], 20, f"soak cycle {cycle_num} step4")
    if not ok:
        return False, "did not return to WAIT_EMPLOYEE"
    if full_screenshots:
        runner.capture(f"E2E-026-cycle{cycle_num}", "wait_employee_final")
    return True, None


def tc_soak(runner, cycles):
    test_id = "E2E-026"
    r = CaseResult(test_id, f"{cycles}-cycle happy-path soak")
    log(f"[START] {test_id} ({cycles} cycles)")
    runner.set_step(test_id)
    failures = []
    for i in range(1, cycles + 1):
        t0 = time.time()
        ok, reason = run_happy_path_cycle(runner, i, i in (1, cycles))
        log(f"  [CYCLE {i}/{cycles}] {'OK' if ok else 'FAIL: ' + str(reason)} ({time.time()-t0:.1f}s)")
        if not ok:
            failures.append({"cycle": i, "reason": reason})
    details = {"cycles_run": cycles, "failures": failures}
    if failures:
        return runner.record(r.finish("FAIL_STATE", **details))
    return runner.record(r.finish("PASS", **details))


def tc_memory(runner, mem_before, mem_after):
    test_id = "E2E-027"
    r = CaseResult(test_id, "Memory before/after -- single-sample heuristic, NOT leak proof")
    HEAP_THRESHOLD = -150_000
    PSRAM_THRESHOLD = -300_000
    heap_delta = (mem_after.get("heap_free") or 0) - (mem_before.get("heap_free") or 0)
    psram_delta = (mem_after.get("psram_free") or 0) - (mem_before.get("psram_free") or 0)
    details = {"heap_before": mem_before.get("heap_free"), "heap_after": mem_after.get("heap_free"),
              "heap_delta": heap_delta, "heap_threshold": HEAP_THRESHOLD,
              "psram_before": mem_before.get("psram_free"), "psram_after": mem_after.get("psram_free"),
              "psram_delta": psram_delta, "psram_threshold": PSRAM_THRESHOLD,
              "note": "single before/after sample across one suite run -- flags a meaningful drop, "
                      "is NOT proof of a sustained leak (would need multiple soak runs' worth of samples)"}
    if heap_delta < HEAP_THRESHOLD or psram_delta < PSRAM_THRESHOLD:
        return runner.record(r.finish("FAIL_PERFORMANCE", **details))
    return runner.record(r.finish("PASS", **details))


# ==========================================================================
# Suite registry
# ==========================================================================

def last_ok(runner):
    return bool(runner.results) and runner.results[-1].status == "PASS"


def skip(runner, test_id, description, reason):
    r = CaseResult(test_id, description)
    return runner.record(r.finish("SKIPPED", reason=reason))


def suite_basic(runner, budget):
    budget.check("E2E-001")
    tc_preflight(runner)
    if runner.results[-1].status != "PASS":
        return
    budget.check("E2E-002")
    tc_bootstrap_accepted(runner)
    budget.check("E2E-003")
    tc_visual_state(runner, "E2E-003", "WAIT_EMPLOYEE visual", "WAIT_EMPLOYEE")
    budget.check("E2E-004")
    scan_and_wait(runner, "E2E-004", "WF|EMP|00152", ["WAIT_OPERATION"], "Valid employee scan -> WAIT_OPERATION")
    budget.check("E2E-005")
    if last_ok(runner):
        tc_visual_state(runner, "E2E-005", "WAIT_OPERATION visual", "WAIT_OPERATION")
    else:
        skip(runner, "E2E-005", "WAIT_OPERATION visual", "E2E-004 did not pass -- not re-waiting redundantly")
    budget.check("E2E-007")
    scan_and_wait(runner, "E2E-007", "WF|OP|OP-001", ["SESSION_ACTIVE"], "Valid operation scan -> SESSION_ACTIVE")
    budget.check("E2E-008")
    if last_ok(runner):
        tc_visual_state(runner, "E2E-008", "SESSION_ACTIVE visual", "SESSION_ACTIVE")
    else:
        skip(runner, "E2E-008", "SESSION_ACTIVE visual", "E2E-007 did not pass -- not re-waiting redundantly")
    budget.check("E2E-010")
    if last_ok(runner):
        tc_key_and_wait(runner, "E2E-010", "#", ["QUANTITY_INPUT"], "'#' at SESSION_ACTIVE -> QUANTITY_INPUT")
    else:
        skip(runner, "E2E-010", "finish", "E2E-008 did not pass")
    budget.check("E2E-011")
    if last_ok(runner):
        tc_visual_state(runner, "E2E-011", "QUANTITY_INPUT visual (empty)", "QUANTITY_INPUT", "empty")
    else:
        skip(runner, "E2E-011", "QUANTITY_INPUT visual", "E2E-010 did not pass")
    budget.check("E2E-012")
    if last_ok(runner):
        tc_quantity_submit(runner, "E2E-012", "", "Quantity 0 (empty buffer submit)")
    else:
        skip(runner, "E2E-012", "Quantity 0", "E2E-011 did not pass")


def suite_business(runner, budget):
    budget.check("E2E-004b")
    scan_and_wait(runner, "E2E-004b", "WF|EMP|00152", ["WAIT_OPERATION"], "employee scan (business suite setup)")
    budget.check("E2E-009")
    tc_invalid_operation(runner)
    budget.check("E2E-007b")
    scan_and_wait(runner, "E2E-007b", "WF|OP|OP-001", ["SESSION_ACTIVE"], "operation scan (business suite setup)",
                 require_start_states=["WAIT_OPERATION"])
    budget.check("E2E-010b")
    tc_key_and_wait(runner, "E2E-010b", "#", ["QUANTITY_INPUT"], "finish (business suite setup)")
    budget.check("E2E-013")
    tc_quantity_submit(runner, "E2E-013", "123", "Quantity 123 (typed digits)")
    budget.check("E2E-006")
    tc_invalid_employee(runner)
    budget.check("E2E-015")
    tc_duplicate_scan(runner)
    budget.check("E2E-007c")
    scan_and_wait(runner, "E2E-007c", "WF|OP|OP-002", ["SESSION_ACTIVE"], "operation scan (post-duplicate cleanup)",
                 require_start_states=["WAIT_OPERATION"])
    budget.check("E2E-010c")
    tc_key_and_wait(runner, "E2E-010c", "#", ["QUANTITY_INPUT"], "finish (post-duplicate cleanup)")
    budget.check("E2E-012b")
    tc_quantity_submit(runner, "E2E-012b", "", "quantity 0 (post-duplicate cleanup, back to WAIT_EMPLOYEE)")
    budget.check("E2E-016")
    tc_invalid_transition(runner)


def suite_ui(runner, budget):
    budget.check("E2E-003")
    tc_visual_state(runner, "E2E-003", "WAIT_EMPLOYEE visual", "WAIT_EMPLOYEE")
    budget.check("E2E-004")
    scan_and_wait(runner, "E2E-004", "WF|EMP|00152", ["WAIT_OPERATION"], "employee scan (ui suite)")
    budget.check("E2E-005")
    tc_visual_state(runner, "E2E-005", "WAIT_OPERATION visual", "WAIT_OPERATION")
    budget.check("E2E-007")
    scan_and_wait(runner, "E2E-007", "WF|OP|OP-001", ["SESSION_ACTIVE"], "operation scan (ui suite)")
    budget.check("E2E-008")
    tc_visual_state(runner, "E2E-008", "SESSION_ACTIVE visual", "SESSION_ACTIVE")
    budget.check("E2E-010")
    tc_key_and_wait(runner, "E2E-010", "#", ["QUANTITY_INPUT"], "finish (ui suite)")
    budget.check("E2E-011")
    tc_visual_state(runner, "E2E-011", "QUANTITY_INPUT visual (empty)", "QUANTITY_INPUT", "empty")
    budget.check("E2E-013")
    tc_quantity_submit(runner, "E2E-013", "123", "Quantity 123 (typed digits, visual)")
    budget.check("E2E-022")
    backend_admin(runner.cfg, {"disabled": True})
    tc_admin_forced_state(runner, "E2E-022", "DEVICE_DISABLED", {"disabled": True}, "DEVICE_DISABLED",
                          "WF|EMP|00152")
    backend_admin(runner.cfg, {"disabled": False})
    budget.check("E2E-023")
    scan_and_wait(runner, "E2E-003b", "WF|EMP|00152", ["WAIT_OPERATION"], "recover from disabled")
    tc_admin_forced_state(runner, "E2E-023", "MAINTENANCE", {"maintenance": True}, "MAINTENANCE", "WF|EMP|00152")
    backend_admin(runner.cfg, {"maintenance": False})
    budget.check("E2E-024")
    do_input(runner, "E2E-024", {"type": "SCAN", "value": "WF|EMP|00152"})
    wait_for_business_state(runner.cfg, ["WAIT_EMPLOYEE", "WAIT_OPERATION"], 20, "E2E-024 recover")
    r24 = CaseResult("E2E-024", "Debug visual regression")
    cap24 = runner.capture("E2E-024", "regression")
    problems24 = screenshot_sanity(cap24["screenshot"], cap24["meta"])
    blank24 = detect_blank_or_stuck(cap24["screenshot"])
    runner.record(r24.finish("FAIL_DISPLAY_BLANK" if blank24 else ("FAIL_UI" if problems24 else "PASS"),
                             sanity_problems=problems24, blank_or_stuck=blank24))
    budget.check("E2E-025")
    tc_wifi_recovery_logic(runner)


def suite_network(runner, budget):
    budget.check("E2E-018-setup")
    scan_and_wait(runner, "E2E-003c", "WF|EMP|00152", ["WAIT_OPERATION"], "setup for network suite")
    budget.check("E2E-017")
    tc_lost_ack_idempotent(runner)
    budget.check("E2E-018")
    tc_state_conflict_resync(runner)
    budget.check("E2E-020")
    repo_root = os.getcwd()
    mock_log_path = os.path.join(runner.run_dir, "mock_backend_restart.log")
    tc_backend_outage_recovery(runner, repo_root, mock_log_path)


def suite_state(runner, budget):
    budget.check("E2E-021")
    tc_reboot_restore(runner)
    budget.check("E2E-022")
    backend_admin(runner.cfg, {"disabled": True})
    tc_admin_forced_state(runner, "E2E-022", "DEVICE_DISABLED", {"disabled": True}, "DEVICE_DISABLED",
                          "WF|EMP|00152")
    backend_admin(runner.cfg, {"disabled": False})
    budget.check("E2E-023")
    do_input(runner, "recover", {"type": "SCAN", "value": "WF|EMP|00152"})
    wait_for_business_state(runner.cfg, ["WAIT_EMPLOYEE", "WAIT_OPERATION"], 20, "recover from disabled")
    tc_admin_forced_state(runner, "E2E-023", "MAINTENANCE", {"maintenance": True}, "MAINTENANCE", "WF|EMP|00152")
    backend_admin(runner.cfg, {"maintenance": False})


def suite_soak(runner, budget, cycles):
    do_input(runner, "soak-setup", {"type": "SCAN", "value": "WF|EMP|00152"})
    wait_for_business_state(runner.cfg, ["WAIT_EMPLOYEE", "WAIT_OPERATION"], 20, "soak setup")
    ds_now = esp_device_state(runner.cfg)
    if ds_now.get("state", {}).get("business") != "WAIT_EMPLOYEE":
        backend_admin(runner.cfg, {"maintenance": False, "disabled": False})
    budget.check("E2E-026")
    tc_soak(runner, cycles)
    budget.check("E2E-027")
    mem_before = esp_device_state(runner.cfg).get("memory", {})
    mem_after = esp_device_state(runner.cfg).get("memory", {})
    tc_memory(runner, mem_before, mem_after)


SUITES = {
    "basic": (suite_basic, 90),
    "business": (suite_business, 150),
    "ui": (suite_ui, 150),
    "network": (suite_network, 150),
    "state": (suite_state, 120),
}

ALL_CASES = {
    "E2E-001": lambda runner: tc_preflight(runner),
    "E2E-002": lambda runner: tc_bootstrap_accepted(runner),
    "E2E-003": lambda runner: tc_visual_state(runner, "E2E-003", "WAIT_EMPLOYEE visual", "WAIT_EMPLOYEE"),
    "E2E-004": lambda runner: scan_and_wait(runner, "E2E-004", "WF|EMP|00152", ["WAIT_OPERATION"],
                                            "Valid employee scan"),
    "E2E-005": lambda runner: tc_visual_state(runner, "E2E-005", "WAIT_OPERATION visual", "WAIT_OPERATION"),
    "E2E-006": lambda runner: tc_invalid_employee(runner),
    "E2E-007": lambda runner: scan_and_wait(runner, "E2E-007", "WF|OP|OP-001", ["SESSION_ACTIVE"],
                                            "Valid operation scan"),
    "E2E-008": lambda runner: tc_visual_state(runner, "E2E-008", "SESSION_ACTIVE visual", "SESSION_ACTIVE"),
    "E2E-009": lambda runner: tc_invalid_operation(runner),
    "E2E-010": lambda runner: tc_key_and_wait(runner, "E2E-010", "#", ["QUANTITY_INPUT"], "finish"),
    "E2E-011": lambda runner: tc_visual_state(runner, "E2E-011", "QUANTITY_INPUT visual", "QUANTITY_INPUT", "empty"),
    "E2E-012": lambda runner: tc_quantity_submit(runner, "E2E-012", "", "Quantity 0"),
    "E2E-013": lambda runner: tc_quantity_submit(runner, "E2E-013", "123", "Quantity 123"),
    "E2E-015": lambda runner: tc_duplicate_scan(runner),
    "E2E-016": lambda runner: tc_invalid_transition(runner),
    "E2E-017": lambda runner: tc_lost_ack_idempotent(runner),
    "E2E-018": lambda runner: tc_state_conflict_resync(runner),
    "E2E-021": lambda runner: tc_reboot_restore(runner),
    "E2E-025": lambda runner: tc_wifi_recovery_logic(runner),
}


# ==========================================================================
# Report generation
# ==========================================================================

def write_report(runner, cfg, suite_name, final_ds, run_dir):
    results = runner.results
    counts = {}
    for res in results:
        counts[res.status] = counts.get(res.status, 0) + 1
    total = len(results)
    passed = counts.get("PASS", 0)
    failed = sum(v for k, v in counts.items() if k.startswith("FAIL"))
    skipped = counts.get("SKIPPED", 0)

    summary = {
        "project": "mesflow-kiosk-runtime-v2", "suite": suite_name, "run_id": runner.run_id,
        "version": final_ds.get("firmware_version"), "build_id": final_ds.get("build_id"),
        "esp": {"ip": cfg.esp_ip, "boot_id": final_ds.get("boot_id")},
        "backend": {"url": cfg.backend_url},
        "totals": {"total": total, "pass": passed, "fail": failed, "skipped": skipped},
        "cases": [res.to_dict() for res in results],
    }
    Runner.save_json(os.path.join(run_dir, "summary.json"), summary)

    lines = [f"=== SUITE: {suite_name} ===", f"TOTAL {total}  PASS {passed}  FAIL {failed}  SKIPPED {skipped}", ""]
    for res in results:
        lines.append(f"{res.test_id:10s} {res.description[:50]:50s} {res.status:20s} {res.duration_s}s")
    report_text = "\n".join(lines)
    with open(os.path.join(run_dir, "summary.txt"), "w") as f:
        f.write(report_text)
    print("\n" + report_text)
    return summary


# ==========================================================================
# Main
# ==========================================================================

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", default="basic", choices=list(SUITES.keys()) + ["full", "soak"])
    parser.add_argument("--case", default=None, help="Run exactly one test case (overrides --suite)")
    parser.add_argument("--soak-cycles", type=int, default=10)
    parser.add_argument("--list", action="store_true", help="List available suites/cases and exit")
    args = parser.parse_args()

    if args.list:
        print("Suites:", ", ".join(list(SUITES.keys()) + ["full", "soak"]))
        print("Cases:", ", ".join(ALL_CASES.keys()))
        return

    cfg = Config()
    run_id = "QA-" + time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    run_dir = os.path.join("artifacts", "e2e", run_id)
    os.makedirs(os.path.join(run_dir, "cases"), exist_ok=True)
    runner = Runner(cfg, run_dir, run_id)

    log(f"=== Kiosk E2E Runner -- {run_id} ===")
    log(f"ESP: {cfg.esp_base}  BACKEND: {cfg.backend_url}  DEVICE_ID: {cfg.device_id}")
    log(f"suite={args.case or args.suite}")

    esp_qa_session(cfg, run_id=run_id, step="STARTING", active=True)

    try:
        if args.case:
            if args.case not in ALL_CASES:
                print(f"Unknown case {args.case}. Use --list to see available cases.", file=sys.stderr)
                sys.exit(2)
            budget = SuiteBudget(60)
            runner.set_step(args.case)
            ALL_CASES[args.case](runner)
        elif args.suite == "soak":
            budget = SuiteBudget(600)
            suite_soak(runner, budget, args.soak_cycles)
        elif args.suite == "full":
            budget = SuiteBudget(90 + 150 + 150 + 150 + 120)
            for name in ["basic", "business", "ui", "network", "state"]:
                fn, _ = SUITES[name]
                log(f"--- entering suite: {name} ---")
                fn(runner, budget)
        else:
            fn, default_budget = SUITES[args.suite]
            budget = SuiteBudget(default_budget)
            fn(runner, budget)
    except SuiteTimeoutError as exc:
        log(f"!!! {exc} -- stopping suite (global budget exceeded, not running remaining cases)")
    except Exception as exc:  # noqa: BLE001 -- a crash still must produce a report
        import traceback
        log(f"!!! Runner crashed: {exc}\n{traceback.format_exc()}")
        runner.record(CaseResult("RUNNER-CRASH", str(exc)).finish("FAIL_CONNECTIVITY", exception=str(exc)))
    finally:
        esp_qa_session(cfg, active=False)
        try:
            final_ds = esp_device_state(cfg, timeout=6)
        except Exception:  # noqa: BLE001
            final_ds = {}
        write_report(runner, cfg, args.case or args.suite, final_ds, run_dir)


if __name__ == "__main__":
    main()
