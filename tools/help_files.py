#!/usr/bin/env python3
"""Lists Help's files in resources/resources.qrc.

Help's topics are Markdown files in resources/help/<language>/, built into
the program under :/help. Run this after adding, renaming, or removing one
(the test helpTopicsAreComplete checks that the list is current):

    python3 tools/help_files.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")


def main():
    files = sorted(os.path.relpath(os.path.join(d, f), RES).replace(os.sep, "/")
                   for d, _, fs in os.walk(os.path.join(RES, "help")) for f in fs)
    path = os.path.join(RES, "resources.qrc")
    with open(path, encoding="utf-8") as f:
        qrc = f.read()
    qrc = re.sub(r'  <qresource prefix="/help">.*?</qresource>\n', "", qrc, flags=re.S)
    block = '  <qresource prefix="/help">\n' + "".join(
        f'    <file alias="{f[len("help/"):]}">{f}</file>\n' for f in files) + "  </qresource>\n"
    qrc = qrc.replace("</RCC>", block + "</RCC>")
    with open(path, "w", encoding="utf-8") as f:
        f.write(qrc)
    print(f"{len(files)} files")


if __name__ == "__main__":
    main()
