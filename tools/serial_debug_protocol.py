#!/usr/bin/env python3
"""Serial Visual Debug Fallback wire protocol (docs/VISUAL_DEBUG.md).

Reads the same three payloads the HTTP /debug/* endpoints serve
(screenshot/ui-state/device-state), but over a USB serial connection --
for when the host running this tool isn't on the same LAN/subnet as the
kiosk's WiFi (HTTP unreachable) but a USB cable still reaches the device.

Firmware side: kiosk_runtime_v2.ino's debug-screenshot / debug-ui-state /
debug-device-state serial commands, built by DebugServer::write_*_serial()
(src/debug/debug_server.cpp) -- the SAME builder methods the HTTP handlers
call, not a second hand-maintained copy.

Wire formats:
  Screenshot: a framed binary blob, magic b"MFSB", 24-byte header:
    offset  0  magic "MFSB" (4 bytes)
    offset  4  format version (uint8) = 1
    offset  5  pixel_format (uint8) = 0 (RGB565)
    offset  6  rotation (uint8)
    offset  7  screen_id_len (uint8)
    offset  8  width (uint16 LE)
    offset 10  height (uint16 LE)
    offset 12  frame_id (uint32 LE)
    offset 16  payload_size (uint32 LE) -- screen_id_len + pixel byte count
    offset 20  crc32 (uint32 LE) -- IEEE 802.3, over the payload only
    offset 24  payload: screen_id bytes, then raw RGB565 pixel data

  ui-state / device-state: plain-text JSON wrapped in marker lines --
    ##MFDBG-BEGIN <tag>##
    <one JSON object on one line>
    ##MFDBG-END##
  found by scanning serial lines (structured log lines interleaved before
  the markers are simply skipped, not parsed as the payload).
"""
import json
import struct
import time
import zlib

SCREENSHOT_MAGIC = b"MFSB"
HEADER_SIZE = 24


class SerialProtocolError(RuntimeError):
    pass


def send_command(ser, command: str) -> None:
    ser.reset_input_buffer()
    ser.write((command + "\n").encode("ascii"))
    ser.flush()


def _read_exactly(ser, n, timeout, what):
    """Reads exactly n bytes, polling until `timeout` seconds have elapsed
    with no progress. pyserial's own per-call timeout only bounds a single
    read(); a large framebuffer needs several reads to arrive in full."""
    buf = bytearray()
    deadline = time.time() + timeout
    while len(buf) < n:
        remaining = n - len(buf)
        chunk = ser.read(remaining)
        if chunk:
            buf += chunk
            deadline = time.time() + timeout  # progress -- extend the window
        elif time.time() > deadline:
            raise SerialProtocolError(
                f"timed out reading {what}: got {len(buf)}/{n} bytes")
    return bytes(buf)


def read_screenshot(ser, timeout=20):
    """Sends debug-screenshot and reads back the framed binary blob.
    Returns (pixel_format, width, height, rotation, frame_id, screen_id,
    pixel_bytes) -- decoding to a PIL Image is left to the caller (shared
    with the HTTP path in capture_screen.py) so this module has no Pillow
    dependency of its own.
    """
    send_command(ser, "debug-screenshot")

    # Scan forward for the 4-byte magic -- any structured log lines already
    # queued before the command was processed are simply skipped, not parsed.
    window = bytearray()
    deadline = time.time() + timeout
    while True:
        if time.time() > deadline:
            raise SerialProtocolError(
                f"timed out waiting for {SCREENSHOT_MAGIC!r} magic "
                f"(saw {bytes(window[-64:])!r} before giving up)")
        b = ser.read(1)
        if not b:
            continue
        window += b
        if len(window) > 4:
            del window[:-4]
        if bytes(window) == SCREENSHOT_MAGIC:
            break

    rest = _read_exactly(ser, HEADER_SIZE - 4, timeout, "screenshot header")
    header = SCREENSHOT_MAGIC + rest
    version, pixel_format, rotation, screen_id_len = header[4], header[5], header[6], header[7]
    width, height = struct.unpack_from("<HH", header, 8)
    frame_id, = struct.unpack_from("<I", header, 12)
    payload_size, = struct.unpack_from("<I", header, 16)
    expected_crc, = struct.unpack_from("<I", header, 20)

    if version != 1:
        raise SerialProtocolError(f"unsupported screenshot format version {version}")
    if pixel_format != 0:
        raise SerialProtocolError(f"unsupported pixel_format {pixel_format} (only RGB565=0 supported)")

    payload = _read_exactly(ser, payload_size, timeout, "screenshot payload")
    actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise SerialProtocolError(
            f"CRC32 mismatch: device said {expected_crc:#010x}, "
            f"host computed {actual_crc:#010x} over {len(payload)} bytes "
            f"-- serial transfer corrupted")

    screen_id = payload[:screen_id_len].decode("ascii", errors="replace")
    pixel_bytes = payload[screen_id_len:]
    expected_pixel_bytes = width * height * 2
    if len(pixel_bytes) != expected_pixel_bytes:
        raise SerialProtocolError(
            f"pixel data size mismatch: got {len(pixel_bytes)}, "
            f"expected {expected_pixel_bytes} ({width}x{height}x2)")

    meta = {
        "format_version": version,
        "pixel_format": "RGB565",
        "rotation": rotation,
        "width": width,
        "height": height,
        "frame_id": frame_id,
        "screen_id": screen_id,
    }
    return meta, pixel_bytes


def debug_input(ser, payload: dict, timeout=10):
    """Sends debug-input:<json> (SCAN/KEY_DOWN/KEY_UP, same dispatch as
    POST /debug/input) and returns the parsed {"accepted":...} response.
    Needed to drive real state transitions for visual-parity testing when
    there's no HTTP path to the device and no physical scanner/keypad
    access -- see DebugServer::write_input_result_serial()."""
    body = json.dumps(payload, ensure_ascii=False)
    send_command(ser, f"debug-input:{body}")
    return _read_marked_json(ser, "debug-input", timeout)


def _read_marked_json(ser, tag, timeout):
    # NOTE: unlike ui-state/device-state (pure reads, no side effects),
    # debug-input's bus_.publish(event) can trigger business logic that
    # prints its OWN structured log lines (state changes, event-send
    # start/end...) BEFORE write_input_result_serial() gets to print its own
    # response -- a real ordering surprise found live via this exact tool.
    # So: don't assume exactly one line between BEGIN/END. Read every line
    # until END, and return the LAST one that parses as a JSON object (our
    # own out.println(resp) always runs immediately before out.println(END),
    # so it's always the line right before END -- any other JSON-shaped
    # noise in between is a normal structured log, not the payload).
    begin_marker = f"##MFDBG-BEGIN {tag}##"
    end_marker = "##MFDBG-END##"
    deadline = time.time() + timeout

    while True:
        if time.time() > deadline:
            raise SerialProtocolError(f"timed out waiting for {begin_marker!r}")
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line == begin_marker:
            break

    last_json = None
    while True:
        if time.time() > deadline:
            raise SerialProtocolError(f"timed out waiting for {end_marker!r} after {begin_marker!r}")
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line == end_marker:
            break
        if not line:
            continue
        try:
            last_json = json.loads(line)
        except json.JSONDecodeError:
            continue  # an interleaved structured log line, not our payload -- ignore

    if last_json is None:
        raise SerialProtocolError(f"no JSON line found between {begin_marker!r} and {end_marker!r}")
    return last_json


def read_json_block(ser, tag, timeout=10):
    """Sends `debug-<tag>` and reads the ##MFDBG-BEGIN <tag>##/##MFDBG-END##
    wrapped JSON block. `tag` is "ui-state" or "device-state"."""
    send_command(ser, f"debug-{tag}")
    return _read_marked_json(ser, tag, timeout)
