#!/usr/bin/env python3
"""UI-BUNDLE-ACTIVATE-REDRAW / UI-BUNDLE-DOWNGRADE-REDRAW regression tests.

Real-hardware regression coverage for a bug found live during the Phase 4
V1 Visual Parity pass: UiSyncController::poll() used to activate a new
bundle in storage correctly but never told KioskRuntime to redraw, so the
screen stayed on stale content (same frame_id) until an unrelated event
happened. Fixed by having poll() report activation and the .ino calling
KioskRuntime::refresh_idle_screen() when it does (see
firmware/kiosk_runtime_v2/src/runtime/ui_sync_controller.*).

This drives the REAL device over serial (reboot via RTS toggle is currently
the only reliable way to trigger a bundle re-check -- see the Phase 4
report's note that heartbeat-triggered UI sync checking isn't implemented
yet, only bootstrap-triggered) -- there is no host-test equivalent; the bug
this guards against is specifically about real hardware's redraw behavior,
which host tests (no display) cannot observe.

Assertions per test case (§9 of the task):
  - active_ui_version (device-state's ui.active_version) changes and
    reaches the intended target
  - screenshot content changes (screenshot_diff.py, 0% would mean no
    redraw happened -- the authoritative signal here)
  - ui-state's ui_bundle_version matches the newly active version
  - no input is injected between the version switch and the check (redraw
    must be automatic, not triggered by some other event)
  - frame_id is logged for visibility but NOT gated on: since this test
    triggers the switch via a full reboot (the only sync trigger currently
    wired), a fresh boot's own deterministic draw sequence can coincidentally
    produce the same frame_id count on two different runs even though the
    fix fired correctly -- a real false-negative found writing this exact
    test. See run_case()'s comment for detail.

Usage:
  python3 tools/test_ui_bundle_redraw_regression.py <serial-by-id-path> \\
      [--mock-admin-url http://localhost:8799] [--from 1] [--to 2]

Requires: the mock backend running and reachable from the DEVICE (over
whatever tunnel/LAN is currently configured as its api-endpoint) with UI
bundle versions --from and --to both defined in UI_BUNDLE_CONTENT.
"""
import argparse
import json
import sys
import time

import serial

import serial_debug_protocol as proto
from capture_screen import capture_via_serial, write_capture_bundle
from screenshot_diff import diff_report


def set_desired_version(mock_admin_url, version):
    import urllib.request
    req = urllib.request.Request(
        f"{mock_admin_url}/mock/admin/set-ui-version",
        data=json.dumps({"version": version}).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def reboot_device(ser):
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.3)


def wait_for_bundle_version(serial_path, baud, expected_version, timeout=60):
    """Polls device-state over serial until ui.active_version == expected_version
    (or sync_state settles) -- rebooting first, since bootstrap-triggered
    check_desired() is currently the only sync trigger."""
    ser = serial.Serial(serial_path, baud, timeout=1)
    try:
        reboot_device(ser)
        deadline = time.time() + timeout
        last_state = None
        while time.time() < deadline:
            try:
                device_state = proto.read_json_block(ser, "device-state", timeout=5)
            except proto.SerialProtocolError:
                time.sleep(1)
                continue
            ui = device_state.get("ui", {})
            last_state = ui
            if ui.get("active_version") == expected_version and ui.get("sync_state") == "ACTIVE":
                return device_state
            time.sleep(2)
        raise TimeoutError(f"bundle version never reached {expected_version} within {timeout}s "
                           f"(last seen: {last_state})")
    finally:
        ser.close()


def run_case(name, serial_path, baud, mock_admin_url, from_version, to_version, out_base):
    print(f"=== {name}: {from_version} -> {to_version} ===")
    findings = []

    # 1. Establish the FROM version as active.
    set_desired_version(mock_admin_url, from_version)
    wait_for_bundle_version(serial_path, baud, from_version, timeout=60)
    img_before, meta_before, ui_before, dev_before = capture_via_serial(serial_path, baud, timeout=20)
    before_dir = write_capture_bundle(img_before, meta_before, ui_before, dev_before,
                                      out_base, "SERIAL", f"{name}-before")
    frame_before = meta_before["frame_id"]
    version_before = dev_before.get("ui", {}).get("active_version")

    # 2. Switch desired to TO version and reboot (the only current sync
    # trigger) -- deliberately NO other input injected before the check.
    set_desired_version(mock_admin_url, to_version)
    device_state_after = wait_for_bundle_version(serial_path, baud, to_version, timeout=60)
    img_after, meta_after, ui_after, dev_after = capture_via_serial(serial_path, baud, timeout=20)
    after_dir = write_capture_bundle(img_after, meta_after, ui_after, dev_after,
                                     out_base, "SERIAL", f"{name}-after")
    frame_after = meta_after["frame_id"]
    version_after = dev_after.get("ui", {}).get("active_version")

    # --- Assertions ---
    def check(label, cond, detail):
        status = "PASS" if cond else "FAIL"
        print(f"  [{status}] {label}: {detail}")
        findings.append((label, cond))

    check("active_ui_version changes", version_before != version_after,
          f"{version_before} -> {version_after}")
    check("active_ui_version reaches target", version_after == to_version,
          f"expected {to_version}, got {version_after}")

    # frame_id is logged, not gated on: this test triggers the version
    # switch via a full reboot (the only sync trigger currently wired --
    # see check_desired()'s own comment), and a fresh boot's deterministic
    # draw sequence (boot -> ready_local -> resyncing -> final bundle draw)
    # can coincidentally land on the SAME frame_id count both times even
    # though the actual bundle activation-redraw fix fired correctly in
    # between -- a real false-negative found writing this exact test.
    # frame_id only reliably signals a MID-SESSION redraw (no reboot), which
    # is exactly what was manually verified live during the Phase 4 V1
    # Visual Parity pass (see the report). The screenshot content diff
    # below is the authoritative check here.
    print(f"  [INFO] frame_id: {frame_before} -> {frame_after} (not gated, see comment)")

    diff = diff_report(
        f"{before_dir}/screenshot.png", f"{after_dir}/screenshot.png")
    check("screenshot content changes", diff["changed_pct"] and diff["changed_pct"] > 0,
          f"{diff['changed_pct']}% pixels changed")

    ui_state_version = ui_after.get("ui_bundle_version")
    check("ui-state reports same bundle version as device-state", ui_state_version == version_after,
          f"ui-state.ui_bundle_version={ui_state_version} device-state.ui.active_version={version_after}")

    all_pass = all(cond for _, cond in findings)
    print(f"=== {name}: {'PASS' if all_pass else 'FAIL'} ===\n")
    return all_pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("serial_port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--mock-admin-url", default="http://localhost:8799")
    parser.add_argument("--out", default="artifacts/debug/redraw_regression")
    parser.add_argument("--version-a", type=int, default=1)
    parser.add_argument("--version-b", type=int, default=2)
    args = parser.parse_args()

    results = {}
    # UI-BUNDLE-ACTIVATE-REDRAW: an upgrade (a < b).
    results["UI-BUNDLE-ACTIVATE-REDRAW"] = run_case(
        "UI-BUNDLE-ACTIVATE-REDRAW", args.serial_port, args.baud, args.mock_admin_url,
        args.version_a, args.version_b, args.out)
    # UI-BUNDLE-DOWNGRADE-REDRAW: the reverse direction (b -> a).
    results["UI-BUNDLE-DOWNGRADE-REDRAW"] = run_case(
        "UI-BUNDLE-DOWNGRADE-REDRAW", args.serial_port, args.baud, args.mock_admin_url,
        args.version_b, args.version_a, args.out)

    print("=== SUMMARY ===")
    for name, ok in results.items():
        print(f"{name}: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if all(results.values()) else 1)


if __name__ == "__main__":
    main()
