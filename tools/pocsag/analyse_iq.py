#!/usr/bin/env python3
"""Analyse a saved POCSAG burst I/Q file codeword by codeword.

    ./analyse_iq.py /tmp/tone.iq

Reports the recovered bit rate, the preamble length, and every codeword after
the sync with its BCH validity. Written to compare framing experiments without
needing the radio again: each capture is saved, so a parameter sweep can be
re-analysed offline.

The key number is how many of a tone page's 15 idle codewords survive. A data
field that is being cut short shows as a few good codewords followed by a
repeating constant, which is what prompted making the length encoding
adjustable.
"""

import argparse
import sys

import numpy as np

SAMPLE_RATE = 1_024_000
POLY, SYNC, IDLE = 0x769, 0x7CD215D8, 0x7A89C197


def bch(cw):
    c = cw & 0xFFFFF800
    r = c
    for i in range(31, 10, -1):
        if r & (1 << i):
            r ^= POLY << (i - 10)
    c |= r & 0x7FE
    return c | (bin(c).count("1") & 1)


def ham(a, b):
    return bin(a ^ b).count("1")


def longest_run(mag, thresh):
    on = mag > thresh
    edges = np.diff(on.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    stops = list(np.flatnonzero(edges == -1) + 1)
    if on[0]:
        starts.insert(0, 0)
    if on[-1]:
        stops.append(len(on))
    runs = [(b - a, a, b) for a, b in zip(starts, stops) if b > a]
    return max(runs) if runs else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iq")
    ap.add_argument("--baud", type=float, default=1200.0)
    args = ap.parse_args()

    iq = np.fromfile(args.iq, dtype=np.complex64)
    mag = np.convolve(np.abs(iq), np.ones(1000) / 1000, mode="same")
    run = longest_run(mag, mag.max() * 0.25)
    if run is None:
        raise SystemExit("no burst found")

    length, a, b = run
    seg = iq[a:b]
    print(f"{args.iq}: burst {length/SAMPLE_RATE:.3f}s")

    inst = np.diff(np.unwrap(np.angle(seg))) * SAMPLE_RATE / (2 * np.pi)
    inst -= np.median(inst)

    best = None
    for rate in np.arange(args.baud - 4, args.baud + 4.1, 0.5):
        spb = SAMPLE_RATE / rate
        mf = np.convolve(inst, np.ones(int(spb)) / int(spb), mode="same")
        for phase in range(0, int(spb), 24):
            n = int((len(mf) - phase - spb) / spb)
            idx = (phase + np.arange(n) * spb).astype(int)
            idx = idx[idx < len(mf)]
            v = mf[idx]
            if len(v) < 700:
                continue
            # Lock rate and phase on the preamble, which is polarity
            # independent, so do NOT try to choose polarity here: alternation
            # scores identically either way and picking one arbitrarily then
            # searching for the sync in the wrong sense hides it completely.
            bits = (v > 0).astype(np.uint8)
            alt = int(np.count_nonzero(bits[:560][:-1] != bits[:560][1:]))
            if best is None or alt > best[0]:
                best = (alt, rate, phase, v)

    alt, rate, phase, v = best

    # Now pick the polarity, by which one actually finds the sync codeword.
    # POCSAG sends a 1 as the lower frequency, so inv=True is the correct
    # convention and the other is a sanity check.
    cand = []
    for inv in (True, False):
        bits = ((v < 0) if inv else (v > 0)).astype(np.uint8)
        t = "".join(str(x) for x in bits)
        d = min((ham(int(t[i:i+32], 2), SYNC), i)
                for i in range(400, min(800, len(t) - 32)))
        cand.append((d[0], d[1], inv, bits))
    cand.sort()
    _, _, inv, bits = cand[0]
    s = "".join(str(x) for x in bits)
    print(f"  locked {rate:.1f} baud, preamble alternation {alt}/559 "
          f"({100*alt/559:.0f}%), {len(bits)} bits, invert={inv}")

    cands = [(ham(int(s[i:i+32], 2), SYNC), i)
             for i in range(400, min(800, len(s) - 32))]
    cands.sort()
    if not cands or cands[0][0] > 6:
        print(f"  no sync codeword found (best {cands[0][0] if cands else 99}/32 errors)")
        return 1

    derr, i0 = cands[0]
    print(f"  sync at bit {i0} with {derr} bit errors\n")

    idles = good = 0
    for w in range(17):
        j = i0 + w * 32
        if j + 32 > len(s):
            break
        cw = int(s[j:j+32], 2)
        ok = bch(cw) == cw
        di = ham(cw, IDLE)
        kind = ("SYNC" if ham(cw, SYNC) <= 4 else
                "idle" if di <= 4 else
                ("msg " if cw & 0x80000000 else "ADDR"))
        if w and di <= 4:
            idles += 1
        if w and ok:
            good += 1
        extra = ""
        if kind == "ADDR" and ok:
            extra = f"  RIC {((cw >> 13) & 0x3FFFF) << 3}"
        print(f"   [{w:2d}] {cw:08X} {'BCHok' if ok else '  -  '} "
              f"dIDLE={di:2d} {kind}{extra}")

    print(f"\n  idle codewords recovered: {idles}/16")
    print(f"  codewords passing BCH:    {good}/16")
    if idles >= 14:
        print("  -> the data field is being transmitted in full")
    elif idles >= 1:
        print("  -> data field is truncated: some codewords then nothing")
    else:
        print("  -> no idle codewords survived")
    return 0


if __name__ == "__main__":
    sys.exit(main())
