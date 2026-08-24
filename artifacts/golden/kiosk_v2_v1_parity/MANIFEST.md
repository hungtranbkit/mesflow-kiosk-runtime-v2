# Kiosk v2 -- V1 Visual Parity Golden References

Captured 2026-08-24 on the real confirmed kiosk board
(`hardware_id=esp32s3-4C64CEF61B44`, `device_id=KIOSK-LASER-01`) via the
Serial Visual Debug Fallback (`tools/capture_screen_serial.py`), rendering
UI bundle **version 3** ("v1 visual parity bundle",
`tools/mock_backend/mock_backend.py`'s `UI_BUNDLE_CONTENT[3]`) -- NOT
hardcoded firmware screens. Every screen here comes from the cached,
backend-managed bundle system (Phase 4), matching the legacy esp-kiosk
(v1)'s color scheme, text hierarchy, and content read directly from its
own source (`esp-kiosk/esp/mesflow_app.cpp`'s drawReady/drawWorker/
drawStartSuccess/drawQtyInput/drawMaintenanceScreen functions).

These are SHADOW-FRAMEBUFFER captures (render/shadow verification), not
photographs of the physical LCD -- see the Phase 4 V1 Visual Parity report
for the separate physical-panel check.

| File | Business state | Source capture |
|---|---|---|
| `state_wait_employee.png` | WAIT_EMPLOYEE | `artifacts/debug/parity/20260824-001653` |
| `state_wait_operation.png` | WAIT_OPERATION (after employee scan `WF\|EMP\|00152`) | `artifacts/debug/parity/20260824-001930` |
| `state_session_active.png` | SESSION_ACTIVE (after operation scan `WF\|OP\|OP-001`) | `artifacts/debug/parity/20260824-002005` |
| `state_quantity_input.png` | QUANTITY_INPUT (after `#` to finish, digits "52" typed) | `artifacts/debug/parity/20260824-002619` (post `UI_TEXT_OVERFLOW` fix) |
| `state_device_disabled.png` | DEVICE_DISABLED (admin-forced) | `artifacts/debug/parity/20260824-002800` |
| `state_maintenance.png` | MAINTENANCE (admin-forced) | `artifacts/debug/parity/20260824-002841` |
| `error_banner_on_wait_employee.png` | WAIT_EMPLOYEE + transient EMPLOYEE_NOT_FOUND banner (scan `WF\|EMP\|99999`) | `artifacts/debug/parity/20260824-002940` |

## Known, accepted differences from legacy v1 (not bugs -- see the Phase 4
V1 Visual Parity report for full detail)

- Text Y is quantized to one of 10 fixed 18px rows (0-166px); legacy uses
  true pixel Y and a bottom-anchored footer at y=272 on its 320px screen.
- No icon/graphic component type yet (legacy's QR/robot/checkmark line-art
  icons, WiFi signal-bar icon) -- text-only equivalents used instead.
- Single font size for all TEXT components (legacy's giant `FONT_QUANTITY`
  digits on QUANTITY_INPUT have no equivalent yet).
- TEXT `x` is decorative-only (fixed left margin) -- no true two-column
  footer split; both footer actions render on one row separated by spaces.
- No header/footer divider LINEs on the v1-parity bundle itself -- present
  in the schema (used correctly by the serial-fallback / UI bundle system's
  own tests) but cut from this specific bundle to fit the ~2-2.3KB per-
  bundle NVS budget found live (see the report).
- MAINTENANCE is simplified vs. legacy's SSID/IP/pending-sync/last-sync
  panel -- that data comes from the Phase 3 offline queue, which v2 doesn't
  have yet (explicitly out of scope this task).
- DEVICE_DISABLED has no legacy equivalent (esp-kiosk has no device-
  suspension concept) -- styled consistent with legacy's C_ERR-for-critical
  convention rather than invented from scratch.
- ERROR is a transient yellow banner over the current screen (row 6), not
  legacy's dedicated red "LỖI" full-screen -- v2's BusinessState enum has
  no separate ERROR state; errors surface as a message over whatever
  screen was already showing, per the existing (pre-Phase-4) renderer
  design.
