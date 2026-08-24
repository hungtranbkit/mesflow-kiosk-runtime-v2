#!/usr/bin/env python3
"""Remote Visual Debug capture tool (docs/VISUAL_DEBUG.md).

Fetches /debug/screenshot, /debug/ui-state, /debug/device-state from a
running kiosk (MESFLOW_DEBUG_API dev builds only) and writes a self-
contained capture bundle so a screenshot can actually be OPENED and
inspected -- not just "the endpoint returned 200".

Usage:
  python3 tools/capture_screen.py <device-ip> [--port 8081] [--out artifacts/debug]
  python3 tools/capture_screen.py <device-ip> --serial /dev/serial/by-id/... [--out artifacts/debug]

If HTTP is unreachable (host and device on different subnets -- see
docs/VISUAL_DEBUG.md "Serial Visual Debug Fallback") and --serial is given,
falls back to the same three payloads over USB serial instead. QA tooling
downstream doesn't need to care which path produced the bundle -- the
output shape (screenshot.png/ui-state.json/device-state.json/manifest.json/
logs.txt) is identical either way; manifest.json's "capture_source" records
which path actually ran, for provenance only.

Output:
  artifacts/debug/<timestamp>/
    screenshot.png
    ui-state.json
    device-state.json
    manifest.json
    logs.txt
  artifacts/debug/latest -> <timestamp>  (symlink, so nothing has to guess
                                           the most recent capture's path)
"""
import argparse
import json
import os
import struct
import sys
import time
import urllib.request
import urllib.error

try:
    from PIL import Image
except ImportError:
    print("This tool needs Pillow: pip install Pillow", file=sys.stderr)
    sys.exit(1)

SCREENSHOT_MAGIC = b"MFSC"
HEADER_SIZE = 16


def fetch(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read()


def fetch_json(url, timeout=10):
    return json.loads(fetch(url, timeout).decode("utf-8"))


def pixels_to_image(width, height, pixel_bytes):
    """RGB565 little-endian raw bytes -> Pillow RGB image. Shared by both
    the HTTP screenshot decoder below and the serial fallback path
    (tools/serial_debug_protocol.py), which parses its own (differently
    framed) header but produces the same (width, height, pixel_bytes)
    shape before handing off here -- one pixel decoder, not two."""
    img = Image.new("RGB", (width, height))
    pixels = img.load()
    idx = 0
    for y in range(height):
        for x in range(width):
            lo = pixel_bytes[idx]
            hi = pixel_bytes[idx + 1]
            idx += 2
            value = lo | (hi << 8)
            r5 = (value >> 11) & 0x1F
            g6 = (value >> 5) & 0x3F
            b5 = value & 0x1F
            r8 = (r5 * 255) // 31
            g8 = (g6 * 255) // 63
            b8 = (b5 * 255) // 31
            pixels[x, y] = (r8, g8, b8)
    return img


def decode_screenshot(raw):
    if len(raw) < HEADER_SIZE or raw[0:4] != SCREENSHOT_MAGIC:
        raise ValueError(f"bad screenshot magic/length: {len(raw)} bytes, "
                          f"magic={raw[0:4]!r}")
    version, pixel_format, rotation, screen_id_len = raw[4], raw[5], raw[6], raw[7]
    width, height = struct.unpack_from("<HH", raw, 8)
    frame_id, = struct.unpack_from("<I", raw, 12)
    offset = HEADER_SIZE
    screen_id = raw[offset:offset + screen_id_len].decode("ascii", errors="replace")
    offset += screen_id_len

    if pixel_format != 0:
        raise ValueError(f"unsupported pixel_format {pixel_format} (only RGB565=0 supported)")

    pixel_bytes = raw[offset:]
    expected = width * height * 2
    if len(pixel_bytes) != expected:
        raise ValueError(f"pixel data size mismatch: got {len(pixel_bytes)}, expected {expected} "
                          f"({width}x{height}x2)")

    img = pixels_to_image(width, height, pixel_bytes)
    meta = {
        "format_version": version,
        "pixel_format": "RGB565",
        "rotation": rotation,
        "width": width,
        "height": height,
        "frame_id": frame_id,
        "screen_id": screen_id,
    }
    return img, meta


def capture_via_http(device_ip, port, timeout=10):
    base = f"http://{device_ip}:{port}"

    # §28: verify screen/frame didn't change between screenshot and
    # ui-state -- request state, capture, request state again, retry if it
    # moved. Cheap correctness check given Phase 0 has no cross-request
    # atomicity guarantee at the HTTP layer (only within a single request,
    # since renderer draws and HTTP handling share one task -- see
    # debug_server.h's frame-consistency comment).
    state_after = None
    raw_shot = None
    for attempt in range(3):
        state_before = fetch_json(f"{base}/debug/ui-state", timeout)
        raw_shot = fetch(f"{base}/debug/screenshot", timeout)
        state_after = fetch_json(f"{base}/debug/ui-state", timeout)
        if state_before.get("frame_id") == state_after.get("frame_id"):
            break
        print(f"screen changed during capture (attempt {attempt + 1}), retrying...")
    else:
        print("WARNING: screen kept changing across 3 attempts; using last capture anyway")

    device_state = fetch_json(f"{base}/debug/device-state", timeout)
    img, shot_meta = decode_screenshot(raw_shot)
    return img, shot_meta, state_after, device_state


def capture_via_serial(serial_path, baud=115200, timeout=20):
    import serial  # local import: only needed for the serial fallback path

    import serial_debug_protocol as proto

    ser = serial.Serial(serial_path, baud, timeout=1)
    try:
        time.sleep(0.3)  # let the port settle before the first command
        shot_meta, pixel_bytes = proto.read_screenshot(ser, timeout=timeout)
        img = pixels_to_image(shot_meta["width"], shot_meta["height"], pixel_bytes)
        ui_state = proto.read_json_block(ser, "ui-state", timeout=timeout)
        device_state = proto.read_json_block(ser, "device-state", timeout=timeout)
        return img, shot_meta, ui_state, device_state
    finally:
        ser.close()


def write_capture_bundle(img, shot_meta, ui_state, device_state, out_base, source, source_detail):
    ts = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    out_dir = os.path.join(out_base, ts)
    os.makedirs(out_dir, exist_ok=True)

    img.save(os.path.join(out_dir, "screenshot.png"))
    with open(os.path.join(out_dir, "ui-state.json"), "w") as f:
        json.dump(ui_state, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "device-state.json"), "w") as f:
        json.dump(device_state, f, indent=2, ensure_ascii=False)

    manifest = {
        "capture_time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "capture_source": source,  # "HTTP" or "SERIAL" -- provenance only,
                                    # QA tooling downstream treats both the same
        "capture_source_detail": source_detail,
        "firmware_version": device_state.get("firmware_version"),
        "build_id": device_state.get("build_id"),
        "boot_id": device_state.get("boot_id"),
        "screen_id": shot_meta["screen_id"],
        "frame_id": shot_meta["frame_id"],
        "display": f"{shot_meta['width']}x{shot_meta['height']}",
        "rotation": shot_meta["rotation"],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)

    # logs.txt: Phase 0 has no persisted device-side log store (no LittleFS
    # log file yet) -- honest placeholder rather than fabricating content.
    with open(os.path.join(out_dir, "logs.txt"), "w") as f:
        f.write("Phase 0 has no persisted device-side log capture yet.\n"
                "Use scripts/monitor.sh for live serial logs during a capture session.\n")

    latest_link = os.path.join(out_base, "latest")
    try:
        if os.path.islink(latest_link) or os.path.exists(latest_link):
            os.remove(latest_link)
        os.symlink(ts, latest_link)
    except OSError as exc:
        print(f"(non-fatal) could not update 'latest' symlink: {exc}", file=sys.stderr)

    print(f"Capture written to {out_dir}  (source={source})")
    print(f"  screenshot.png   {shot_meta['width']}x{shot_meta['height']} "
          f"rotation={shot_meta['rotation']} screen_id={shot_meta['screen_id']} "
          f"frame_id={shot_meta['frame_id']}")
    print(f"  latest -> {latest_link}")
    return out_dir


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("device_ip")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--out", default="artifacts/debug")
    parser.add_argument("--serial", default=None,
                         help="Serial port (e.g. /dev/serial/by-id/...) to fall back to if "
                              "HTTP is unreachable -- see docs/VISUAL_DEBUG.md 'Serial Visual "
                              "Debug Fallback'.")
    parser.add_argument("--serial-baud", type=int, default=115200)
    args = parser.parse_args()

    try:
        img, shot_meta, ui_state, device_state = capture_via_http(args.device_ip, args.port)
        write_capture_bundle(img, shot_meta, ui_state, device_state, args.out,
                              "HTTP", f"{args.device_ip}:{args.port}")
        return
    except (urllib.error.URLError, ConnectionError, TimeoutError, OSError) as exc:
        print(f"HTTP capture failed ({exc}); ", end="", file=sys.stderr)
        if not args.serial:
            print("no --serial fallback given, aborting.", file=sys.stderr)
            sys.exit(1)
        print(f"falling back to serial ({args.serial})...", file=sys.stderr)

    try:
        img, shot_meta, ui_state, device_state = capture_via_serial(args.serial, args.serial_baud)
    except Exception as exc:  # noqa: BLE001 -- surfacing ANY fallback failure honestly
        print(f"ERROR: serial fallback also failed: {exc}", file=sys.stderr)
        sys.exit(1)

    write_capture_bundle(img, shot_meta, ui_state, device_state, args.out,
                          "SERIAL", args.serial)


if __name__ == "__main__":
    main()
