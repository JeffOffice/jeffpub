#!/usr/bin/env python3
"""Measure how far JeffPub 79's text baselines are from Publisher's own.

For the same .pub/PDF pairs as fidelity.py, every laid-out line on page 1
(jpubtool layout) is matched to the nearest baseline in the PDF's text
positions (Tm operators of upright text) that starts inside the line's box
and lies within 4 pt. Unlike the pixel score, this sees sub-pixel offsets
exactly, whatever the rasterizer does with them.

    tools/baselines.py FOLDER [--tool build/jpubtool] [--compare other/jpubtool]

Prints the mean, median and 90th percentile of the absolute error (points)
and the mean signed error (positive: Publisher's baselines are lower).
"""
import argparse
import os
import re
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from fidelity import find_pairs  # noqa: E402


def pdf_baselines(pdf):
    info = subprocess.run(["pdfinfo", pdf], capture_output=True, text=True).stdout
    height = float(re.search(r"Page size:\s+[0-9.]+ x ([0-9.]+)", info).group(1))
    data = open(pdf, "rb").read()
    runs = []
    for m in re.finditer(rb"stream\r?\n", data):
        end = data.find(b"endstream", m.end())
        try:
            text = zlib.decompress(data[m.end():end])
        except zlib.error:
            continue
        if b"BT" not in text:
            continue
        for o in re.finditer(rb"([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+Tm", text):
            if abs(float(o.group(2))) > 1e-3 or abs(float(o.group(3))) > 1e-3:
                continue  # turned text
            runs.append((float(o.group(5)), height - float(o.group(6))))
        break  # Publisher writes page 1's text in the first text stream
    return runs


def errors(tool, pairs):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    out = []
    for pub, pdf in sorted(pairs.items()):
        try:
            runs = pdf_baselines(pdf)
        except (OSError, AttributeError):
            continue
        layout = subprocess.run([tool, "layout", pub, "x"], capture_output=True, text=True, env=env, timeout=300).stdout
        box = None
        for line in layout.splitlines():
            m = re.match(r"p(\d+) box ([-0-9.]+),([-0-9.]+) ([0-9.]+)x([0-9.]+)", line)
            if m:
                box = tuple(map(float, m.groups()[1:])) if m.group(1) == "1" else None
                continue
            m = re.search(r"line y=\S+ h=\S+ x=\S+ w=\S+ base=([-0-9.]+)\t", line)
            if not m or not box or not line.rstrip().split("\t")[-1].strip():
                continue
            bx, by, bw, _ = box
            base = by + float(m.group(1))
            near = [ry - base for rx, ry in runs if bx - 2 <= rx <= bx + bw + 2 and abs(ry - base) < 4]
            if near:
                out.append(min(near, key=abs))
    return out


def report(name, errs):
    if not errs:
        print(f"{name}: no lines matched")
        return
    a = sorted(abs(e) for e in errs)
    print(f"{name}: {len(errs)} lines  mean {sum(a) / len(a):.3f} pt  median {a[len(a) // 2]:.3f}  "
          f"90% {a[int(len(a) * 0.9)]:.3f}  signed {sum(errs) / len(errs):+.3f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--tool", default=os.path.join(os.path.dirname(__file__), "..", "build", "jpubtool"))
    ap.add_argument("--compare", help="another jpubtool to measure the same way")
    args = ap.parse_args()
    pairs = find_pairs(args.folder)
    print(f"{len(pairs)} publications have a newer reference PDF", file=sys.stderr)
    report(args.tool, errors(args.tool, pairs))
    if args.compare:
        report(args.compare, errors(args.compare, pairs))


if __name__ == "__main__":
    main()
