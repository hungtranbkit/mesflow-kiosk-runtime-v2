#!/usr/bin/env python3
"""Visual regression comparison tool (Phase 4 V1 Visual Parity).

Compares two screenshot.png captures (from tools/capture_screen.py /
capture_screen_serial.py) and reports:
  - dimensions (must match, or the diff is meaningless)
  - changed pixel percentage
  - changed bounding box (the smallest rect containing every differing pixel)

Deliberately NOT a 0%-diff pass/fail gate (docs/VISUAL_PARITY.md /
the Phase 4 report's own §12: font rasterization/anti-aliasing differences
between runs are expected and not a real regression). This tool reports the
numbers; a human (or a caller script with its own threshold) decides what
counts as a real difference for a given screen.

Usage:
  python3 tools/screenshot_diff.py <a.png> <b.png> [--threshold-pct 2.0]

Exit code: 0 if changed-pixel% <= --threshold-pct (default 2.0) or images
are identical; 1 otherwise. Always prints the numbers regardless of exit
code -- this is a report tool first, a gate second.
"""
import argparse
import sys

try:
    from PIL import Image
except ImportError:
    print("This tool needs Pillow: pip install Pillow", file=sys.stderr)
    sys.exit(2)


def diff_report(path_a, path_b):
    img_a = Image.open(path_a).convert("RGB")
    img_b = Image.open(path_b).convert("RGB")

    report = {
        "a": path_a,
        "b": path_b,
        "size_a": img_a.size,
        "size_b": img_b.size,
        "dimensions_match": img_a.size == img_b.size,
    }
    if not report["dimensions_match"]:
        report["changed_pct"] = None
        report["bounding_box"] = None
        return report

    w, h = img_a.size
    pixels_a = img_a.load()
    pixels_b = img_b.load()

    changed = 0
    min_x, min_y, max_x, max_y = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            if pixels_a[x, y] != pixels_b[x, y]:
                changed += 1
                if x < min_x: min_x = x
                if y < min_y: min_y = y
                if x > max_x: max_x = x
                if y > max_y: max_y = y

    total = w * h
    report["changed_pixels"] = changed
    report["total_pixels"] = total
    report["changed_pct"] = round(100.0 * changed / total, 3)
    report["bounding_box"] = (
        {"x": min_x, "y": min_y, "w": max_x - min_x + 1, "h": max_y - min_y + 1}
        if changed > 0 else None
    )
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image_a")
    parser.add_argument("image_b")
    parser.add_argument("--threshold-pct", type=float, default=2.0,
                         help="Report-only default gate: changed%% at or below this "
                              "counts as PASS on exit code (visual review still matters "
                              "for anything above 0%%).")
    args = parser.parse_args()

    report = diff_report(args.image_a, args.image_b)

    print(f"A: {report['a']}  {report['size_a']}")
    print(f"B: {report['b']}  {report['size_b']}")
    if not report["dimensions_match"]:
        print("DIMENSIONS MISMATCH -- diff not meaningful, treat as FAIL")
        sys.exit(1)

    print(f"Changed pixels: {report['changed_pixels']}/{report['total_pixels']} "
          f"({report['changed_pct']}%)")
    if report["bounding_box"]:
        bb = report["bounding_box"]
        print(f"Changed bounding box: x={bb['x']} y={bb['y']} w={bb['w']} h={bb['h']}")
    else:
        print("Changed bounding box: (none -- images identical)")

    if report["changed_pct"] <= args.threshold_pct:
        print(f"PASS (<= {args.threshold_pct}% threshold)")
        sys.exit(0)
    else:
        print(f"FAIL (> {args.threshold_pct}% threshold -- review visually, "
              f"this may still be acceptable font-rasterization noise)")
        sys.exit(1)


if __name__ == "__main__":
    main()
