#!/usr/bin/env python3
"""Hunt for a POCSAG capcode (RIC) in a pager EEPROM dump.

    ./ric_hunt.py dump.bin [--id 559424 --id 04325-559424 ...]

Reports a region map, searches for known subscriber identifiers in the
encodings a pager might plausibly use, scans every bit alignment for valid
POCSAG codewords, and lists 21-bit RIC candidates from the structured areas.

Written against a Philips 53D dump whose 24C32N turned out to hold only the
LCD font and stored messages, so treat a negative result as real: config often
lives in a separate part.
"""

import argparse
import re
import sys
from collections import Counter

POCSAG_POLY = 0x769
SYNC = 0x7CD215D8
IDLE = 0x7A89C197
MAX_RIC = 0x1FFFFF


def bch_encode(codeword):
    """Fill BCH(31,21) check bits and trailing even parity."""
    cw = codeword & 0xFFFFF800
    rem = cw
    for i in range(31, 10, -1):
        if rem & (1 << i):
            rem ^= POCSAG_POLY << (i - 10)
    cw |= rem & 0x7FE
    return cw | (bin(cw).count("1") & 1)


def region_map(data, block=64):
    """Collapse the image into runs of blank / erased / populated blocks."""
    out, prev, start = [], None, 0
    for i in range(0, len(data), block):
        b = data[i:i + block]
        kind = ("zeros" if all(x == 0 for x in b)
                else "erased" if all(x == 0xFF for x in b)
                else "data")
        if kind != prev:
            if prev is not None:
                out.append((start, i - 1, prev))
            prev, start = kind, i
    out.append((start, len(data) - 1, prev))
    return out


def encodings(value_str):
    """The ways a decimal identifier might sit in an EEPROM."""
    digits = re.sub(r"\D", "", value_str)
    out = {}
    if digits:
        out["ascii"] = digits.encode()
        packed = digits if len(digits) % 2 == 0 else "0" + digits
        out["bcd"] = bytes.fromhex(packed)
        n = int(digits)
        for width in (2, 3, 4):
            if n < (1 << (8 * width)):
                out[f"be{width * 8}"] = n.to_bytes(width, "big")
                out[f"le{width * 8}"] = n.to_bytes(width, "little")
    return out


def find_all(data, pattern):
    return [i for i in range(len(data) - len(pattern) + 1)
            if data[i:i + len(pattern)] == pattern]


def codeword_scan(data):
    """Every bit alignment, so a non-byte-aligned bitstream is still found."""
    bits = [(b >> k) & 1 for b in data for k in range(7, -1, -1)]
    hits = []
    for off in range(len(bits) - 32):
        cw = 0
        for k in range(32):
            cw = (cw << 1) | bits[off + k]
        if cw in (0, 0xFFFFFFFF):
            continue
        if bch_encode(cw) == cw:
            hits.append((off, cw))
    return hits, len(bits)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--id", action="append", default=[],
                    help="known identifier (pager number, serial); repeatable")
    args = ap.parse_args()

    data = open(args.dump, "rb").read()
    print(f"{args.dump}: {len(data)} bytes\n")

    print("== region map ==")
    for a, b, kind in region_map(data):
        print(f"  {a:04x}-{b:04x}  {kind}")

    if args.id:
        print("\n== identifier search ==")
        for ident in args.id:
            for name, pat in encodings(ident).items():
                hits = find_all(data, pat)
                where = ", ".join(f"{h:#06x}" for h in hits) if hits else "-"
                print(f"  {ident:>16} {name:<7} {pat.hex():<12} {where}")

    print("\n== POCSAG framing ==")
    framing = False
    for name, word in (("sync", SYNC), ("idle", IDLE)):
        for order in ("big", "little"):
            hits = find_all(data, word.to_bytes(4, order))
            if hits:
                framing = True
                print(f"  {name} {order}-endian at "
                      + ", ".join(f"{h:#06x}" for h in hits))
    if not framing:
        print("  none: raw POCSAG batches are not stored in this image")

    print("\n== valid codeword scan ==")
    hits, nbits = codeword_scan(data)
    expected = nbits // 2048          # an 11 bit check, so 1 in 2048 by chance
    print(f"  {len(hits)} valid codewords, ~{expected} expected from random data")
    if len(hits) <= expected:
        print("  at or below the noise floor: no stored codeword structure")
    addrs = [(o, c) for o, c in hits if not c & 0x80000000]
    if addrs and len(hits) > expected:
        print(f"  {len(addrs)} address codewords:")
        for upper18, n in Counter(((c >> 13) & 0x3FFFF) for _, c in addrs).most_common(16):
            print(f"    RIC {upper18 * 8}..{upper18 * 8 + 7}  seen {n}x")

    print("\n== 21 bit RIC candidates in populated regions ==")
    seen = Counter()
    for a, b, kind in region_map(data):
        if kind != "data":
            continue
        for i in range(a, min(b, len(data) - 3)):
            for order in ("big", "little"):
                v = int.from_bytes(data[i:i + 3], order)
                if 1000 <= v <= MAX_RIC:
                    seen[v] += 1
    print(f"  {len(seen)} distinct values; a 4KB image yields thousands, so this")
    print("  is only useful once a config region has been narrowed down.")


if __name__ == "__main__":
    sys.exit(main())
