#!/usr/bin/env python3
"""Dump the structure of a .pub file (Publisher 2000 and later).

    tools/pubdump.py FILE.pub [--contents] [--quill] [--escher] [--hex]

Prints the compound file's streams, then (all by default):
  --contents  the Contents stream: the chunk directory in its trailer and
              every chunk as a tree of blocks (id, type, value)
  --quill     the text stream (Quill/QuillSub/CONTENTS): its section index
  --escher    the drawing stream (Escher/EscherStm): the record tree
--hex adds raw bytes for variable-length blocks. Used to study the format
for the .pub writer and to compare its output with real files. Needs
Python 3 with olefile.
"""
import struct
import sys

import olefile

# Block data lengths by block type (libmspub MSPUBParser::getBlockDataLength).
FIXED = {0x00: 0, 0x78: 0, 0x05: 0, 0x08: 0, 0x0A: 0, 0x10: 2, 0x12: 2, 0x18: 2, 0x1A: 2, 0x07: 2,
         0x20: 4, 0x22: 4, 0x58: 4, 0x68: 4, 0x70: 4, 0xB8: 4, 0x28: 8, 0x38: 16, 0x48: 24}
VARIABLE = {0xC0, 0x80, 0x82, 0x88, 0x8A, 0x90, 0x98, 0xA0}
CHUNK_TYPES = {0x01: "SHAPE", 0x20: "ALTSHAPE", 0x44: "DOCUMENT", 0x43: "PAGE", 0x5C: "PALETTE",
               0x46: "BORDER_ART", 0x30: "GROUP", 0x31: "LOGO", 0x10: "TABLE", 0x63: "CELLS", 0x6C: "FONT", 0x75: "SECTIONS"}


def blocks(b, off, end, depth, out, show_hex, limit=4000):
    """Parse a run of blocks between off and end."""
    while off + 2 <= end and len(out) < limit:
        bid, typ = b[off], b[off + 1]
        p = off + 2
        pad = "  " * depth
        if typ in FIXED:
            n = FIXED[typ]
            raw = b[p:p + n]
            val = int.from_bytes(raw, "little") if n in (1, 2, 4) else raw.hex()
            out.append(f"{pad}{bid:02x}:{typ:02x} = {val}")
            off = p + n
        elif typ in VARIABLE:
            if p + 4 > end:
                out.append(f"{pad}{bid:02x}:{typ:02x} truncated")
                return
            ln = struct.unpack_from("<I", b, p)[0]
            if ln < 4 or p + ln > end:
                out.append(f"{pad}{bid:02x}:{typ:02x} bad length {ln}")
                return
            if typ == 0xC0:
                s = b[p + 4:p + ln].decode("utf-16-le", "replace")
                out.append(f"{pad}{bid:02x}:{typ:02x} string {s!r}")
            else:
                line = f"{pad}{bid:02x}:{typ:02x} [{ln}]"
                if show_hex:
                    line += " " + b[p + 4:p + min(ln, 68)].hex()
                out.append(line)
                blocks(b, p + 4, p + ln, depth + 1, out, show_hex, limit)
            off = p + ln
        else:
            out.append(f"{pad}{bid:02x}:{typ:02x} unknown type; rest: {b[off:off + 16].hex()}")
            return


def dump_contents(b, show_hex):
    trailer = struct.unpack_from("<I", b, 0x1A)[0]
    print(f"Contents: {len(b)} bytes, header {b[:0x1E].hex()}, trailer at {trailer}")
    out = []
    blocks(b, trailer + 4, trailer + struct.unpack_from("<I", b, trailer)[0], 1, out, False, 100000)
    # The trailer's directory part lists one block per sequence number
    # (libmspub numbers them in order); 88 blocks are chunk references
    # holding 02 (type), 04 (offset) and 05 (parent sequence number).
    refs = []
    end = trailer + struct.unpack_from("<I", b, trailer)[0]
    o = trailer + 4
    for _ in range(3):
        bid, typ = b[o], b[o + 1]
        p = o + 2
        if typ in FIXED:
            o = p + FIXED[typ]
            continue
        ln = struct.unpack_from("<I", b, p)[0]
        if typ == 0x90:  # directory
            q, seq = p + 4, 0
            while q + 2 <= p + ln:
                i2, t2 = b[q], b[q + 1]
                r = q + 2
                if t2 in FIXED:
                    q = r + FIXED[t2]
                else:
                    l2 = struct.unpack_from("<I", b, r)[0]
                    if t2 == 0x88:
                        fields, z = {}, r + 4
                        while z + 2 <= r + l2:
                            i3, t3 = b[z], b[z + 1]
                            n3 = FIXED.get(t3)
                            if n3 is None:
                                break
                            fields[i3] = int.from_bytes(b[z + 2:z + 2 + n3], "little")
                            z += 2 + n3
                        refs.append((seq, fields))
                    q = r + l2
                seq += 1
        o = p + ln
    print("Chunk directory (seq: type offset parent):")
    chunks = []
    for s, f in refs:
        if 2 in f and 4 in f:
            chunks.append((f[4], s, f[2], f.get(5, 0)))
    chunks.sort()
    for i, (o, s, t, par) in enumerate(chunks):
        print(f"  seq {s:4d}: {CHUNK_TYPES.get(t, hex(t)):10s} at {o:6d} parent {par}")
    print()
    for o, s, t, par in chunks:
        ln = struct.unpack_from("<I", b, o)[0]
        print(f"chunk seq {s} {CHUNK_TYPES.get(t, hex(t))} at {o} length {ln}")
        out = []
        blocks(b, o + 4, o + ln, 1, out, show_hex)
        print("\n".join(out))
        print()


def dump_quill(b):
    print(f"Quill CONTENTS: {len(b)} bytes, header {b[:32].hex()}")
    # CHNKINK index: blocks chained from 0x18, each with its count (+2), the
    # next block's offset (+4, 0xFFFFFFFF at the end) and 24-byte entries (+8).
    at, seen = 0x18, set()
    while at != 0xFFFFFFFF and at not in seen and at + 8 <= len(b):
        seen.add(at)
        count, nxt = struct.unpack_from("<HI", b, at + 2)
        for i in range(count):
            e = at + 8 + i * 24
            if e + 24 > len(b):
                break
            name = b[e + 2:e + 6].decode("latin-1")
            sec_id = struct.unpack_from("<H", b, e + 6)[0]
            off, ln = struct.unpack_from("<II", b, e + 16)
            print(f"  {name} id={sec_id} offset={off} length={ln}")
        at = nxt


def dump_escher(b):
    names = {0xF000: "DggContainer", 0xF001: "BStoreContainer", 0xF002: "DgContainer", 0xF003: "SpgrContainer",
             0xF004: "SpContainer", 0xF006: "Dgg", 0xF007: "BSE", 0xF008: "Dg", 0xF009: "Spgr", 0xF00A: "Sp",
             0xF00B: "OPT", 0xF00F: "ChildAnchor", 0xF010: "ClientAnchor", 0xF011: "ClientData",
             0xF11E: "SplitMenuColors", 0xF122: "TertiaryOPT"}
    print(f"Escher: {len(b)} bytes")

    def walk(off, end, depth):
        while off + 8 <= end:
            vi, t, ln = struct.unpack_from("<HHI", b, off)
            if not 0xF000 <= t <= 0xF200 or off + 8 + ln > end:
                off += 1  # Publisher pads between drawings
                continue
            inst = vi >> 4
            line = "  " * depth + f"{names.get(t, hex(t))} inst={inst} len={ln}"
            if t in (0xF00B, 0xF122):
                props = []
                for i in range(inst):
                    pid, val = struct.unpack_from("<HI", b, off + 8 + i * 6)
                    props.append(f"{pid:04x}={val}")
                line += " " + " ".join(props)
            elif t in (0xF00A, 0xF010, 0xF011, 0xF00F, 0xF009, 0xF008):
                line += " " + b[off + 8:off + 8 + ln].hex()
            print(line)
            if vi & 0xF == 0xF:
                walk(off + 8, off + 8 + ln, depth + 1)
            off += 8 + ln
    walk(0, len(b), 1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    opts = set(sys.argv[2:])
    show_hex = "--hex" in opts
    want = {o for o in opts if o != "--hex"} or {"--contents", "--quill", "--escher"}
    o = olefile.OleFileIO(path)
    print("Streams:")
    for e in o.listdir():
        n = "/".join(e)
        print(f"  {n!r} {o.get_size(n)}")
    print()
    if "--contents" in want and o.exists("Contents"):
        dump_contents(o.openstream("Contents").read(), show_hex)
    if "--quill" in want and o.exists("Quill/QuillSub/CONTENTS"):
        dump_quill(o.openstream("Quill/QuillSub/CONTENTS").read())
        print()
    if "--escher" in want and o.exists("Escher/EscherStm"):
        dump_escher(o.openstream("Escher/EscherStm").read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
