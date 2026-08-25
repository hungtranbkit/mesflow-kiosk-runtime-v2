#!/usr/bin/env python3
"""Stateful mock MESFlow kiosk-v2 backend (Phase 2: Server Authoritative
State + Workflow Contract).

This is the SERVER side of "server owns business state" (docs/ARCHITECTURE.md
invariant 1/13/14): it holds real per-device business state
(WAIT_EMPLOYEE/WAIT_OPERATION/SESSION_ACTIVE/QUANTITY_INPUT/DEVICE_DISABLED/
MAINTENANCE), a state_version, a global server_seq, mock employee/operation
data, and an in-memory idempotency store. The device only ever asks and
renders what this backend decides -- it never runs this transition table
itself (see docs/PROTOCOL.md's local-business-transition audit).

Endpoints:
  POST /api/kiosk/v2/bootstrap
  POST /api/kiosk/v2/events
  POST /api/kiosk/v2/heartbeat
  GET  /api/kiosk/v2/state?device_id=...
  GET  /api/kiosk/v2/ui-bundles/<version>   (Phase 4: backend-managed, locally-cached UI)
  GET  /mock/state/<device_id>        (DEV inspection, not part of the real protocol)
  POST /mock/admin/<device_id>        ({"disabled":true}/{"maintenance":true}/{"reset":true})
  POST /mock/admin/set-ui-version     ({"version":N}) -- DEV lever to change the desired UI bundle version
  POST /_test/drop-next               (chaos: delay next N event responses)

Not a real backend: in-memory only (lost on restart), no auth, no real
persistence. Good enough to prove Envelope v1 + retry + state-authority
end to end against real hardware.

Usage:
  python3 tools/mock_backend/mock_backend.py [--port 8799]
"""
import argparse
import hashlib
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --- Mock reference data (§16/§35/§36) ---
EMPLOYEES = {
    "00152": {"name": "Nguyen Van A", "active": True},
    "00200": {"name": "Tran Thi B", "active": True},
    "00999": {"name": "Le Van Disabled", "active": False},
    # NV001: the real card used for the live physical-scan regression test
    # (2026-08-24) -- device-state's last_scan showed "WF|EMP|NV001" from an
    # actual GM65 scan that failed with API_HTTP_5XX (dead tunnel), not
    # EMPLOYEE_NOT_FOUND. Added so the real card can complete the happy path
    # against this mock backend once the transport issue is fixed.
    "NV001": {"name": "Real Test Card NV001", "active": True},
    "NV004": {"name": "Real Test Card NV004", "active": True},
    # anything else -> EMPLOYEE_NOT_FOUND
}
OPERATIONS = {
    "OP-001": {"name": "Chan", "open": True},
    "OP-002": {"name": "Han", "open": True},
    "OP-CLOSED": {"name": "Đã đóng", "open": False},
    # 111-THAN-THUNG-R-05: the real card's actual operation QR code, seen
    # live during the 2026-08-24 physical scan regression test (correctly
    # rejected as OPERATION_NOT_FOUND before this was added -- proof the
    # error classification distinguishes business rejection from transport
    # failure). Added so the real card can complete the happy path.
    "111-THAN-THUNG-R-05": {"name": "Than Thung R-05", "open": True},
    # anything else -> OPERATION_NOT_FOUND
}

# --- Global server-wide state ---
devices = {}          # device_id -> device state dict
idempotency = {}      # (device_id, event_id) -> (payload_hash, response_dict)
server_seq_counter = 0
drop_next_n = 0
DROP_DELAY_S = 9  # past the device's RUNTIME_HTTP_HARD_DEADLINE_MS (8s)

# --- Phase 4: backend-managed, locally-cached UI bundle ---
# Component whitelist is TEXT/RECT/LINE only (matches
# firmware/kiosk_runtime_v2/src/protocol/ui_bundle.h's first real
# implementation -- icon/value/status/progress/keypad from
# docs/UI_SCHEMA.md's full aspirational list are reserved, not implemented).
# `x` is decorative only when `align` is unset (row-grid legacy bundles,
# v1/v2 below); when a component sets "align":"center" the renderer computes
# `x` itself via centered_x() and auto-shrinks font_size if the resolved
# text would overflow (see draw_from_bundle() in renderer.cpp) -- v3 below
# relies on that path exclusively.
#
# UI Consistency Cleanup (2026-08-25): the renderer's screen-level C++ code
# was cut over to exactly TWO named font roles (kFontSmall=Body/16px,
# kFontLarge=Large/24px -- see renderer.cpp's select_vn_font() comment).
# Bundle wire format is unchanged/back-compat (any font_size 1-N still
# decodes), but v3 -- the bundle meant to visually match that same design --
# now only ever emits 2 (Body) or 3 (Large), never the old FONT_VALUE=2/
# FONT_QUANTITY=7 legacy-parity roles that predate this cleanup.
FONT_SMALL = 2   # == renderer.cpp's kFontSmall (Body/16px) -- most text
FONT_LARGE = 3   # == renderer.cpp's kFontLarge (Large/24px) -- titles only


def _text(y, text, color="#FFFFFF", size=1):
    # §NVS/SPIFFS storage note: bundle JSON now lives on SPIFFS (1.5MB,
    # see firmware/kiosk_runtime_v2's default_8MB.csv), not the ~20KB nvs
    # partition -- the old ~2-2.3KB practical ceiling this comment used to
    # warn about no longer applies (Phase 4.1 §3 migration). `w`/`h` are
    # still omitted since the renderer computes them itself via
    # getTextBounds() at draw time (see emit_component_text()); `font_size`
    # IS read now (true-geometry stabilization pass). Used only by the
    # legacy row-grid v1/v2 bundles below -- v3 uses `_c()` instead.
    return {"type": "text", "x": 4, "y": y, "color": color, "text": text, "font_size": size}


def _c(y, text, color="#FFFFFF", size=FONT_SMALL, align="center"):
    """Centered text component -- what v3 (the default/current-design
    bundle) uses for every screen. `align` is read by draw_from_bundle():
    the renderer computes `x` itself (centered_x()) and auto-shrinks
    font_size if centered text would overflow, so `x` is omitted here on
    purpose (would be dead/ignored input when align="center")."""
    return {"type": "text", "x": 0, "y": y, "color": color, "text": text, "font_size": size, "align": align}


# MESFlow v5 legacy (esp-kiosk) midnight-blue palette, read verbatim from its
# own source (esp-kiosk/esp/mesflow_app.cpp's C_* constants' own hex
# comments). Kept as the current renderer's own accent/status palette too
# (colors didn't change in the 2026-08-25 UI Consistency Cleanup, only
# typography/layout did) -- the V1_ prefix is now a historical name, not a
# claim that this bundle is still v1-parity (see UI_BUNDLE_CONTENT[3]).
V1_WHITE = "#FFFFFF"    # C_TEXT
V1_MUTED = "#94A3B8"    # C_MUTED
V1_OK = "#22C55E"       # C_OK (status-positive green)
V1_WARN = "#60A5FA"     # C_WARN/C_INFO (blue attention, no orange in this theme)
V1_ERR = "#F43F5E"      # C_ERR (status-critical rose/red)
V1_BORDER = "#1E293B"   # C_PANEL_2 (slate divider line)

UI_BUNDLE_CONTENT = {
    1: {
        "manifest": {"version": 1, "schema_version": 1, "min_runtime_version": "0.4.0"},
        "screens": [
            {"id": "state_wait_employee", "components": [
                _text(4, "MESFlow Kiosk Runtime v2", "#00FF00"),
                _text(40, "Sẵn sàng quét thẻ nhân viên"),
            ]},
            {"id": "state_wait_operation", "components": [
                _text(4, "{{employee_name}}", "#00FF00"),
                _text(40, "Quét mã công đoạn"),
            ]},
            {"id": "state_session_active", "components": [
                _text(4, "{{employee_name}}", "#00FF00"),
                _text(22, "{{operation_name}}"),
                _text(58, "Mục tiêu: {{target_qty}}"),
                _text(94, "Bấm # để kết thúc"),
            ]},
            {"id": "state_quantity_input", "components": [
                _text(4, "{{operation_name}}", "#00FF00"),
                _text(40, "Nhập số lượng đạt, # để gửi:"),
                _text(76, "{{local_digit_buffer}}", "#00FF00"),
            ]},
            {"id": "state_device_disabled", "components": [
                _text(4, "THIẾT BỊ ĐÃ BỊ VÔ HIỆU HÓA", "#FFFF00"),
                _text(40, "Liên hệ quản trị viên"),
            ]},
            {"id": "state_maintenance", "components": [
                _text(4, "THIẾT BỊ ĐANG BẢO TRÌ", "#FFFF00"),
            ]},
        ],
    },
}

# Version 2: identical structure, ONE real visible difference (accent color
# green -> cyan, plus an extra line) -- exists purely so UI-003/UI-010
# (new version triggers a download + a real, observable visual change) has
# something concrete to assert against, not just a version-number bump.
import copy as _copy  # noqa: E402 (deliberately local, only this module needs it)
UI_BUNDLE_CONTENT[2] = _copy.deepcopy(UI_BUNDLE_CONTENT[1])
UI_BUNDLE_CONTENT[2]["manifest"]["version"] = 2
for _screen in UI_BUNDLE_CONTENT[2]["screens"]:
    if _screen["id"] == "state_wait_employee":
        _screen["components"][0]["color"] = "#00FFFF"  # green -> cyan: the visible v2 marker
        _screen["components"].append(_text(58, "UI v2"))


# Version 3: the DEFAULT/CURRENT-DESIGN bundle -- content, colors and
# per-screen Y positions read verbatim from the renderer's OWN hardcoded
# fallback (draw_business_state() in renderer.cpp), which is itself the
# 2026-08-25 UI Consistency Cleanup's source of truth. This bundle exists so
# a server-pushed UI can be exercised end-to-end (UI-003/UI-010's whole
# point) while still looking IDENTICAL to what the device draws when no
# bundle is active -- not a separate design, a mirror of one.
#
# Superseded the earlier "V1 VISUAL PARITY" bundle that used to live at this
# version number (row-grid, left-aligned, single font size) -- that layout
# predates the two-size/centered/fixed-zone design and no longer matches
# what state_* screen_ids fall back to, so keeping it would have made a
# real, observable regression (bundle active -> old row-grid look; bundle
# absent -> new centered look) invisible to anyone testing only against
# this mock. QUANTITY_INPUT/defect/rework are deliberately NOT covered here
# (see note below the screens list) -- v2's giant-digit rendering
# (kFontValueScale) has no bundle-schema equivalent, so those screen_ids
# are left out of the manifest and the device transparently falls back to
# its own hardcoded draw_quantity_input_screen()/etc. for them.
UI_BUNDLE_CONTENT[3] = {
    "manifest": {"version": 3, "schema_version": 1, "min_runtime_version": "0.9.0"},
    "screens": [
        # draw_business_state() BusinessState::WAIT_EMPLOYEE / draw_title_2line()
        {"id": "state_wait_employee", "components": [
            _c(122, "SẴN SÀNG", V1_WHITE, FONT_LARGE),
            _c(122 + 34, "QUÉT MÃ", V1_WHITE, FONT_LARGE),
        ]},
        # WAIT_OPERATION: employee name (accent) above a 2-line title
        {"id": "state_wait_operation", "components": [
            _c(64, "{{employee_name}}", V1_OK, FONT_SMALL),
            _c(142, "QUÉT MÃ", V1_WHITE, FONT_LARGE),
            _c(142 + 34, "CÔNG ĐOẠN", V1_WHITE, FONT_LARGE),
        ]},
        # SESSION_ACTIVE: title + employee/operation detail lines + footer hint
        {"id": "state_session_active", "components": [
            _c(100, "ĐANG LÀM", V1_OK, FONT_LARGE),
            _c(160, "{{employee_name}}", V1_WHITE, FONT_SMALL),
            _c(186, "{{operation_name}}", V1_MUTED, FONT_SMALL),
            _c(230, "Quét lại thẻ để kết thúc", V1_MUTED, FONT_SMALL),
        ]},
        # No direct legacy equivalent (esp-kiosk has no device-suspension
        # concept) -- styled consistent with the critical/warn palette
        # rather than invented from scratch.
        {"id": "state_device_disabled", "components": [
            _c(130, "ĐÃ VÔ HIỆU HÓA", V1_ERR, FONT_LARGE),
            _c(190, "Liên hệ quản trị viên", V1_WHITE, FONT_SMALL),
        ]},
        {"id": "state_maintenance", "components": [
            _c(140, "ĐANG BẢO TRÌ", V1_WARN, FONT_LARGE),
        ]},
    ],
}


def _bundle_json_bytes(version):
    return json.dumps(UI_BUNDLE_CONTENT[version], ensure_ascii=False).encode("utf-8")


def _bundle_hash(version):
    return hashlib.sha256(_bundle_json_bytes(version)).hexdigest()


# The version this mock server currently wants every device running --
# separate from any individual device's own state (§22: UI version must
# never track business state). Starts at the only version that exists;
# /mock/admin/set-ui-version changes this for testing UI-003/UI-010.
desired_ui_bundle_version = 3  # current-design (2-size/centered) bundle is the default


def new_device_state():
    return {
        "state_name": "WAIT_EMPLOYEE",
        "state_version": 1,
        "workflow_version": 1,
        "session": None,
        "disabled": False,
        "maintenance": False,
    }


def get_device(device_id):
    if device_id not in devices:
        devices[device_id] = new_device_state()
    return devices[device_id]


def build_view(dev):
    view = {}
    session = dev["session"]
    if session:
        view["employee_name"] = session["employee_name"]
        if session.get("operation_code"):
            view["operation_code"] = session["operation_code"]
            view["operation_name"] = session["operation_name"]
        if session.get("session_id"):
            view["session_id"] = session["session_id"]
            view["started_at"] = session["started_at"]
            view["target_qty"] = session["target_qty"]
            view["produced_qty"] = session["produced_qty"]
    return view


def snapshot(dev):
    name = dev["state_name"]
    if dev["disabled"]:
        name = "DEVICE_DISABLED"
    elif dev["maintenance"]:
        name = "MAINTENANCE"
    return {
        "state": {"name": name, "version": dev["state_version"]},
        "workflow": {"version": dev["workflow_version"]},
        "view": build_view(dev),
    }


def bump_version(dev):
    dev["state_version"] += 1


def parse_scan(raw):
    """WF|EMP|<id> or WF|OP|<code> -- server-side business parsing (§17):
    the device just forwards raw bytes, this backend decides what they mean."""
    parts = raw.split("|")
    if len(parts) == 3 and parts[0] == "WF":
        return parts[1], parts[2]
    return None, None


def apply_event(dev, event_type, payload, quantity_good):
    """Returns (accepted: bool, error_code: str|None, error_message: str|None).
    Mutates `dev` in place ONLY when a real transition happens (§15: no
    version bump on rejections that don't change state)."""
    if dev["disabled"]:
        return False, "DEVICE_DISABLED", "Thiết bị đã bị vô hiệu hóa"
    if dev["maintenance"]:
        return False, "MAINTENANCE", "Thiết bị đang bảo trì"

    state = dev["state_name"]

    if event_type == "SCAN":
        kind, value = parse_scan(payload.get("raw", ""))

        if state == "WAIT_EMPLOYEE":
            if kind != "EMP":
                return False, "STATE_INVALID_TRANSITION", "Cần quét thẻ nhân viên"
            emp = EMPLOYEES.get(value)
            if emp is None and value.startswith("NV") and value[2:].isdigit():
                # DEV convenience for the 2026-08-24 real-scanner regression
                # test: the user has multiple real "NV###" cards in hand and
                # whack-a-moling each one into EMPLOYEES individually wasted
                # turns without proving anything new -- any NV### code is
                # auto-accepted here as a valid active employee. This is a
                # MOCK backend fixture rule, not a real backend's employee
                # lookup; a real backend would still require a real DB match.
                emp = {"name": f"Real Card {value}", "active": True}
                EMPLOYEES[value] = emp
            if emp is None:
                return False, "EMPLOYEE_NOT_FOUND", "Nhân viên không hợp lệ"
            if not emp["active"]:
                return False, "EMPLOYEE_DISABLED", "Nhân viên đã bị khóa"
            dev["session"] = {
                "employee_id": value, "employee_name": emp["name"],
                "operation_code": None, "operation_name": None,
                "session_id": None, "started_at": None,
                "target_qty": 0, "produced_qty": 0,
            }
            dev["state_name"] = "WAIT_OPERATION"
            bump_version(dev)
            return True, None, None

        if state == "WAIT_OPERATION":
            if kind != "OP":
                return False, "STATE_INVALID_TRANSITION", "Cần quét mã công đoạn"
            op = OPERATIONS.get(value)
            if op is None and value:
                # Same DEV convenience as the NV### employee wildcard above:
                # real operation QR codes (seen live: "111-THAN-THUNG-R-05",
                # "111-THAN-THUNG-R-02") don't match this fixture's made-up
                # "OP-001" style keys and vary per scan -- this regression
                # test is about proving the scan-to-render PATH works, not
                # re-validating real operation-code business data (that's
                # this mock's own job, separately, elsewhere). Any non-empty
                # scanned code is accepted as a valid open operation.
                op = {"name": value, "open": True}
                OPERATIONS[value] = op
            if op is None:
                return False, "OPERATION_NOT_FOUND", "Công đoạn không hợp lệ"
            if not op["open"]:
                return False, "OPERATION_CLOSED", "Công đoạn đã đóng"
            dev["session"]["operation_code"] = value
            dev["session"]["operation_name"] = op["name"]
            dev["session"]["session_id"] = f"S-{server_seq_counter:05d}"
            dev["session"]["started_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            dev["session"]["target_qty"] = 100
            dev["state_name"] = "SESSION_ACTIVE"
            bump_version(dev)
            return True, None, None

        return False, "STATE_INVALID_TRANSITION", "Không thể quét mã ở trạng thái này"

    if event_type == "FINISH_REQUESTED":
        if state != "SESSION_ACTIVE":
            return False, "STATE_INVALID_TRANSITION", "Không có phiên đang hoạt động"
        dev["state_name"] = "QUANTITY_INPUT"
        bump_version(dev)
        return True, None, None

    if event_type == "QUANTITY_SUBMITTED":
        if state != "QUANTITY_INPUT":
            return False, "STATE_INVALID_TRANSITION", "Chưa yêu cầu kết thúc"
        if quantity_good is None:
            return False, "QUANTITY_INVALID", "Thiếu số lượng"
        dev["session"] = None
        dev["state_name"] = "WAIT_EMPLOYEE"
        bump_version(dev)
        return True, None, None

    if event_type == "CANCEL_REQUESTED":
        if state == "WAIT_EMPLOYEE":
            return False, "STATE_INVALID_TRANSITION", "Không có gì để hủy"
        dev["session"] = None
        dev["state_name"] = "WAIT_EMPLOYEE"
        bump_version(dev)
        return True, None, None

    return False, "PROTOCOL_DECODE_FAILED", f"unknown event type {event_type}"


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            return json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as exc:
            print(f"[mock_backend] bad JSON body: {exc}: {raw!r}")
            return None

    def do_GET(self):
        if self.path.startswith("/api/kiosk/v2/state"):
            from urllib.parse import urlparse, parse_qs
            qs = parse_qs(urlparse(self.path).query)
            device_id = qs.get("device_id", [None])[0]
            if not device_id:
                self._send_json(400, {"error": {"code": "PROTOCOL_DECODE_FAILED"}})
                return
            dev = get_device(device_id)
            body = {"device_id": device_id}
            body.update(snapshot(dev))
            self._send_json(200, body)
            return

        if self.path.startswith("/mock/state/"):
            device_id = self.path.split("/mock/state/", 1)[1]
            dev = devices.get(device_id)
            if dev is None:
                self._send_json(404, {"error": "unknown device"})
                return
            body = {"device_id": device_id, **dev, "computed_snapshot": snapshot(dev)}
            self._send_json(200, body)
            return

        if self.path.startswith("/api/kiosk/v2/ui-bundles/"):
            version_str = self.path.split("/api/kiosk/v2/ui-bundles/", 1)[1]
            try:
                version = int(version_str)
            except ValueError:
                self._send_json(400, {"error": {"code": "PROTOCOL_DECODE_FAILED"}})
                return
            if version not in UI_BUNDLE_CONTENT:
                print(f"[mock_backend] UI_BUNDLE_NOT_FOUND version={version}")
                self._send_json(404, {"error": {"code": "UI_BUNDLE_NOT_FOUND"}})
                return
            body_bytes = _bundle_json_bytes(version)
            print(f"[mock_backend] UI_BUNDLE_SERVED version={version} bytes={len(body_bytes)} "
                  f"sha256={_bundle_hash(version)}")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body_bytes)))
            self.end_headers()
            self.wfile.write(body_bytes)
            return

        self._send_json(404, {"accepted": False, "error": {"code": "NOT_FOUND"}})

    def do_POST(self):
        global drop_next_n, server_seq_counter, desired_ui_bundle_version

        if self.path == "/api/kiosk/v2/bootstrap":
            body = self._read_json()
            device_id = body.get("device_id") if body else None
            print(f"[mock_backend] BOOTSTRAP device_id={device_id} "
                  f"hardware_id={body.get('hardware_id') if body else None}")
            resp = {
                "accepted": True,
                "device_status": "ACTIVE",
                "server_time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "protocol": {"accepted_version": 1},
                "desired": {"config_version": 1, "workflow_version": 1,
                           "ui_bundle_version": desired_ui_bundle_version,
                           "ui_bundle_hash": _bundle_hash(desired_ui_bundle_version)},
            }
            if device_id:
                resp.update(snapshot(get_device(device_id)))
            self._send_json(200, resp)
            return

        if self.path == "/api/kiosk/v2/events":
            body = self._read_json()
            if body is None:
                self._send_json(400, {"accepted": False, "error": {"code": "PROTOCOL_DECODE_FAILED"}})
                return

            if body.get("protocol_version") != 1:
                self._send_json(400, {"accepted": False,
                                       "error": {"code": "PROTOCOL_UNSUPPORTED_VERSION"}})
                return

            device = body.get("device", {})
            event = body.get("event", {})
            context = body.get("context", {})
            payload = body.get("payload", {})
            device_id = device.get("device_id") or device.get("hardware_id") or "unknown"
            event_id = event.get("event_id")
            event_type = event.get("type")
            expected_version = context.get("expected_state_version")
            quantity_good = payload.get("quantity_good")

            server_seq_counter += 1
            this_seq = server_seq_counter

            # --- Idempotency (§20/§21/§22) ---
            payload_hash = hashlib.sha256(json.dumps(body, sort_keys=True).encode()).hexdigest()
            cache_key = (device_id, event_id)
            if cache_key in idempotency:
                cached_hash, cached_response = idempotency[cache_key]
                if cached_hash == payload_hash:
                    print(f"[mock_backend] EVENT event_id={event_id} DUPLICATE -- returning cached response "
                          f"(seen again, seq not re-bumped)")
                    self._send_json(200, cached_response)
                    return
                else:
                    print(f"[mock_backend] EVENT event_id={event_id} PAYLOAD MISMATCH on reuse -- rejecting")
                    self._send_json(409, {
                        "accepted": False, "event_id": event_id,
                        "error": {"code": "IDEMPOTENCY_KEY_REUSE_MISMATCH"},
                    })
                    return

            dev = get_device(device_id)

            # --- Optimistic concurrency (§7/§8) ---
            if expected_version is not None and expected_version < dev["state_version"]:
                resp = {
                    "accepted": False, "event_id": event_id,
                    "error": {"code": "STATE_CONFLICT"},
                    "action": "RESYNC",
                    "current_state_version": dev["state_version"],
                    "server_seq": this_seq,
                }
                idempotency[cache_key] = (payload_hash, resp)
                print(f"[mock_backend] EVENT event_id={event_id} STATE_CONFLICT "
                      f"expected={expected_version} actual={dev['state_version']}")
                self._send_json(200, resp)
                return

            accepted, error_code, error_message = apply_event(dev, event_type, payload, quantity_good)

            resp = {"accepted": accepted, "event_id": event_id, "server_seq": this_seq}
            resp.update(snapshot(dev))
            if not accepted:
                resp["error"] = {"code": error_code, "message": error_message}

            idempotency[cache_key] = (payload_hash, resp)

            print(f"[mock_backend] EVENT device_id={device_id} event_id={event_id} type={event_type} "
                  f"accepted={accepted} error={error_code} "
                  f"state={dev['state_name']}/{dev['state_version']} server_seq={this_seq}")

            if drop_next_n > 0:
                drop_next_n -= 1
                print(f"[mock_backend] CHAOS: delaying response {DROP_DELAY_S}s ({drop_next_n} more queued)")
                time.sleep(DROP_DELAY_S)

            # §83: business outcomes (accept/reject/conflict) all use HTTP 200
            # -- "accepted" is a field in the body, not conflated with
            # transport status. 409 is reserved for the idempotency-mismatch
            # case above, which IS a real HTTP-level conflict.
            self._send_json(200, resp)
            return

        if self.path == "/api/kiosk/v2/heartbeat":
            body = self._read_json()
            if body:
                print(f"[mock_backend] HEARTBEAT device_id={body.get('device_id')} "
                      f"uptime_ms={body.get('uptime_ms')} "
                      f"provisioning_state={body.get('provisioning_state')}")
            self._send_json(200, {"accepted": True})
            return

        if self.path == "/mock/admin/set-ui-version":
            body = self._read_json() or {}
            requested = int(body.get("version", 1))
            if requested not in UI_BUNDLE_CONTENT:
                self._send_json(400, {"ok": False, "error": f"no such bundle version {requested}"})
                return
            desired_ui_bundle_version = requested
            print(f"[mock_backend] ADMIN set-ui-version desired_ui_bundle_version={desired_ui_bundle_version}")
            self._send_json(200, {"ok": True, "desired_ui_bundle_version": desired_ui_bundle_version,
                                  "hash": _bundle_hash(desired_ui_bundle_version)})
            return

        if self.path.startswith("/mock/admin/"):
            device_id = self.path.split("/mock/admin/", 1)[1]
            body = self._read_json() or {}
            dev = get_device(device_id)
            if body.get("reset"):
                # Monotonic reset (E2E runner §2 -- a real Phase 2 foot-gun
                # found and documented): NEVER rewind state_version back to
                # 1 on a device that may already hold a higher version --
                # the firmware correctly (invariant 16) refuses forever to
                # accept a snapshot older than what it already has, which
                # left a live device permanently stuck in RESYNCING after a
                # naive reset. Instead: keep the version monotonic by
                # continuing from whatever it already was, +1.
                old_version = dev["state_version"]
                old_workflow_version = dev["workflow_version"]
                devices[device_id] = new_device_state()
                devices[device_id]["state_version"] = old_version + 1
                devices[device_id]["workflow_version"] = old_workflow_version
                print(f"[mock_backend] ADMIN reset device_id={device_id} -> "
                      f"WAIT_EMPLOYEE/{old_version + 1} (monotonic, was {old_version})")
            else:
                if "disabled" in body:
                    dev["disabled"] = bool(body["disabled"])
                    bump_version(dev)
                if "maintenance" in body:
                    dev["maintenance"] = bool(body["maintenance"])
                    bump_version(dev)
                print(f"[mock_backend] ADMIN device_id={device_id} disabled={dev['disabled']} "
                      f"maintenance={dev['maintenance']} version={dev['state_version']}")
            self._send_json(200, {"ok": True, "snapshot": snapshot(get_device(device_id))})
            return

        if self.path == "/_test/drop-next":
            body = self._read_json() or {}
            drop_next_n = int(body.get("n", 1))
            print(f"[mock_backend] CHAOS: will delay the next {drop_next_n} event POST(s)")
            self._send_json(200, {"ok": True, "drop_next_n": drop_next_n})
            return

        self._send_json(404, {"accepted": False, "error": {"code": "NOT_FOUND"}})

    def log_message(self, format, *args):  # noqa: A002
        pass  # suppress default access logging; structured prints above instead


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8799)
    args = parser.parse_args()

    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"[mock_backend] listening on 0.0.0.0:{args.port}")
    print("[mock_backend] endpoints: bootstrap, events, heartbeat, state, "
          "/mock/state/<id>, /mock/admin/<id>, /_test/drop-next")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock_backend] stopping")


if __name__ == "__main__":
    main()
