#!/usr/bin/env python3
"""Compare JeffPub 79's rendering of .pub files with reference PDFs of the same publications.

Finds every .pub under FOLDER that has a PDF beside it (same name, or a name
starting with the .pub's name) that JeffPub did not make and which
is newer than the .pub, so the PDF shows exactly how the .pub should look. Page 1 of
each is rendered at 40 dpi by jpubtool and by pdftoppm and scored by the mean
absolute difference of the blurred grayscale images (0 = identical, 255 = opposite).

    tools/fidelity.py FOLDER [--tool build/jpubtool] [--out DIR] [--baseline report.tsv]

Needs Python 3 with Pillow, and poppler-utils (pdfinfo, pdftoppm).
Writes OUT/report.tsv (key, score, pub path) worst first; with --baseline it
also prints how many files got closer or further than that earlier report.
Reference PDFs printed on a larger sheet are cropped to the centered page.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageChops, ImageFilter, ImageStat


def pdf_info(pdf):
    try:
        out = subprocess.run(["pdfinfo", "-f", "1", "-l", "1", pdf], capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return None
    info = {}
    for line in out.splitlines():
        k, _, v = line.partition(":")
        info[k.strip()] = v.strip()
    return info


def find_pairs(root):
    pairs = {}
    for d, _, files in os.walk(root):
        pubs = [f for f in files if f.lower().endswith(".pub")]
        pdfs = [f for f in files if f.lower().endswith(".pdf")]
        for p in pubs:
            base = os.path.splitext(p)[0].lower()
            best = None
            for f in pdfs:
                b = os.path.splitext(f)[0].lower()
                if b != base and not b.startswith(base):
                    continue
                full_pub, full_pdf = os.path.join(d, p), os.path.join(d, f)
                if os.path.getmtime(full_pdf) < os.path.getmtime(full_pub) - 60:
                    continue
                info = pdf_info(full_pdf)
                if not info or "JeffPub" in info.get("Producer", "") + info.get("Creator", ""):
                    continue
                if best is None or b == base:
                    best = full_pdf
            if best:
                pairs[os.path.join(d, p)] = best
    return pairs


def score(pub, pdf, tool, work):
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    subprocess.run(["pdftoppm", "-r", "40", "-png", "-f", "1", "-l", "1", pdf, os.path.join(work, "ms")], capture_output=True, timeout=120)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    subprocess.run([tool, "render", pub, os.path.join(work, "jp"), "40"], capture_output=True, env=env, timeout=300)
    ms = sorted(f for f in os.listdir(work) if f.startswith("ms"))
    jp = sorted(os.listdir(os.path.join(work, "jp"))) if os.path.isdir(os.path.join(work, "jp")) else []
    if not ms or not jp:
        return None, "no render"
    a = Image.open(os.path.join(work, ms[0])).convert("L")
    b = Image.open(os.path.join(work, "jp", jp[0])).convert("L")
    # A publication printed on a larger sheet sits centered on the PDF page.
    if a.width > b.width * 1.03 and a.height > b.height * 1.03:
        l, t = (a.width - b.width) // 2, (a.height - b.height) // 2
        a = a.crop((l, t, l + b.width, t + b.height))
    if abs(a.width / a.height - b.width / b.height) / (a.width / a.height) > 0.03:
        return None, "page size differs"
    b = b.resize(a.size)
    diff = ImageChops.difference(a.filter(ImageFilter.GaussianBlur(2)), b.filter(ImageFilter.GaussianBlur(2)))
    return round(ImageStat.Stat(diff).mean[0], 2), ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--tool", default=os.path.join(os.path.dirname(__file__), "..", "build", "jpubtool"))
    ap.add_argument("--out", default="fidelity-out")
    ap.add_argument("--baseline")
    args = ap.parse_args()

    pairs = find_pairs(args.folder)
    print(f"{len(pairs)} publications have a newer reference PDF", file=sys.stderr)
    os.makedirs(args.out, exist_ok=True)
    rows = []
    for pub, pdf in sorted(pairs.items()):
        key = hashlib.md5(pub.encode()).hexdigest()[:8]
        s, note = score(pub, pdf, args.tool, os.path.join(args.out, key))
        rows.append((key, s, pub, note))
    rows.sort(key=lambda r: -(r[1] if r[1] is not None else -1))
    with open(os.path.join(args.out, "report.tsv"), "w") as f:
        for key, s, pub, note in rows:
            f.write(f"{key}\t{'' if s is None else s}\t{pub}\t{note}\n")
    scores = sorted(r[1] for r in rows if r[1] is not None)
    if scores:
        print(f"scored {len(scores)}: median {scores[len(scores) // 2]}, mean {sum(scores) / len(scores):.2f}, "
              f"under 1.0: {sum(1 for s in scores if s < 1.0)}")
    if args.baseline:
        old = {}
        for line in open(args.baseline):
            k, s, *_ = line.rstrip("\n").split("\t")
            if s:
                old[k] = float(s)
        better = sum(1 for k, s, *_ in rows if s is not None and k in old and s < old[k] - 0.3)
        worse = sum(1 for k, s, *_ in rows if s is not None and k in old and s > old[k] + 0.3)
        print(f"against baseline: {better} closer, {worse} further")


if __name__ == "__main__":
    main()
