#!/usr/bin/env python3
"""Capture a POCSAG page off air with an RTL-SDR and decode it with multimon-ng.

    ./sdr_decode.py --expect-ric 1578624
    ./sdr_decode.py --selftest          # prove the chain without a radio

This closes the loop. roundtrip.sh proves the encoder produces decodable POCSAG
in software, and hwfsk_test proves the modem is handed those same bits, but
neither shows what actually leaves the antenna. Feeding a real capture back
through the same decoder separates "our signal is malformed" from "the signal
is fine and the capcode or the pager is the problem".

The chain is: I/Q -> FM discriminator -> low pass -> resample to 22050 Hz ->
WAV -> multimon-ng. Run --selftest first if a decode fails, so a broken
pipeline is not mistaken for a broken transmitter.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time
import wave

import numpy as np

TARGET_HZ = 153_225_000
OFFSET_HZ = 50_000
SAMPLE_RATE = 1_024_000
AUDIO_RATE = 22_050           # multimon-ng accepts only this


def find_multimon():
    for c in ("multimon-ng", "/tmp/multimon-ng/build/multimon-ng"):
        if subprocess.run(["which", c], capture_output=True).returncode == 0:
            return c
        if os.path.exists(c):
            return c
    raise SystemExit("multimon-ng not found; build it and put it on PATH")


def capture(seconds, gain, tune_hz, device=0):
    n = int(SAMPLE_RATE * seconds)
    path = tempfile.mktemp(suffix=".iq")
    cmd = ["rtl_sdr", "-f", str(tune_hz), "-s", str(SAMPLE_RATE),
           "-n", str(n), "-d", str(device), "-g", str(gain or 0), path]

    for attempt in range(3):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(path):
            break
        if "No supported devices" in (r.stderr or "") and attempt < 2:
            time.sleep(2)
            continue
        print(r.stderr[-400:], file=sys.stderr)
        raise SystemExit("rtl_sdr failed")

    raw = np.fromfile(path, dtype=np.uint8)
    os.unlink(path)
    return ((raw[0::2].astype(np.float32) - 127.5)
            + 1j * (raw[1::2].astype(np.float32) - 127.5))


def extract_burst(iq, floor, margin=0.05):
    """Cut the transmission out of the capture using the I/Q envelope.

    This must key off |iq|, not the demodulated audio: an FM discriminator puts
    out large random noise when no carrier is present, so gating on the audio
    envelope selects the silence as readily as the signal. Demodulating a
    window that is mostly noise and then normalising against it buries the
    actual page, which is what made a good capture fail to decode.
    """
    mag = np.convolve(np.abs(iq), np.ones(1000) / 1000, mode="same")
    thresh = max(floor * 3.0, mag.max() * 0.25)
    on = mag > thresh

    # The LONGEST CONTIGUOUS run, not first-rise to last-fall. Spanning from the
    # first to the last active sample swallows the silence between two separate
    # bursts, and since an FM discriminator produces large noise where there is
    # no carrier, that noise then dominates the audio and the decode fails on a
    # perfectly good transmission. This cost two captures before being spotted.
    edges = np.diff(on.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    stops = list(np.flatnonzero(edges == -1) + 1)
    if on[0]:
        starts.insert(0, 0)
    if on[-1]:
        stops.append(len(on))

    if not starts or not stops:
        return None, 0.0

    runs = [(b - a, a, b) for a, b in zip(starts, stops) if b > a]
    if not runs:
        return None, 0.0

    length, a, b = max(runs)
    if length < SAMPLE_RATE // 50:
        return None, 0.0

    pad = int(SAMPLE_RATE * margin)
    a = max(0, a - pad)
    b = min(len(iq), b + pad)
    return iq[a:b], (b - a) / SAMPLE_RATE


def demodulate(iq):
    """FM discriminator, then shape it into something multimon-ng can read."""
    inst = np.diff(np.unwrap(np.angle(iq)))          # rad/sample

    # Anti-alias BEFORE resampling, and mind the cutoff. This used to filter at
    # SAMPLE_RATE/(AUDIO_RATE*2), about 44 kHz, which is four times too wide for
    # decimation to 22050: the discriminator's noise, measured at 19 kHz RMS on
    # a real capture, aliased straight into the audio band and buried the 600 Hz
    # square wave. POCSAG at 2400 baud needs only a few kHz, so cut at ~5 kHz.
    cutoff = 5000.0
    k = max(2, int(SAMPLE_RATE / cutoff))
    inst = np.convolve(inst, np.ones(k) / k, mode="same")

    # resample to exactly 22050 Hz; linear is ample for a 1200 baud square wave
    n_out = int(len(inst) * AUDIO_RATE / SAMPLE_RATE)
    audio = np.interp(np.linspace(0, len(inst) - 1, n_out),
                      np.arange(len(inst)), inst)

    # centre it: a residual carrier offset would otherwise sit as a DC bias and
    # bias the slicer one way
    audio -= np.median(audio)

    peak = np.percentile(np.abs(audio), 99.5)
    if peak <= 0:
        return np.zeros(n_out, dtype=np.int16)

    return np.clip(audio / peak * 12000, -32767, 32767).astype(np.int16)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(AUDIO_RATE)
        w.writeframes(samples.tobytes())


def decode(path, mm):
    """Try every rate and both polarities; report whatever decodes."""
    hits = []
    for baud in (512, 1200, 2400):
        for invert in (False, True):
            src = path
            if invert:
                with wave.open(path, "rb") as w:
                    n = w.getnframes()
                    data = np.frombuffer(w.readframes(n), dtype=np.int16)
                src = path.replace(".wav", "_inv.wav")
                write_wav(src, (-data.astype(np.int32)).astype(np.int16))

            r = subprocess.run([mm, "-t", "wav", "-a", f"POCSAG{baud}", "-q", src],
                               capture_output=True, text=True)
            for line in r.stdout.splitlines():
                if "POCSAG" in line and "Address" in line:
                    hits.append((baud, invert, line.strip()))
    return hits


def selftest(mm):
    """Push a known-good POCSAG signal through the whole chain.

    If this passes and a real capture fails, the transmitter is at fault rather
    than the analysis.
    """
    print("== self test: synthetic POCSAG through the same chain ==")
    ric, baud, dev = 1578624, 1200, 3600.0    # dev as actually measured on air

    enc = subprocess.run(["./pocsag_wav", "-r", str(ric), "-b", str(baud),
                          "-t", "alpha", "-f", "3", "-o", "/tmp/st.wav", "TEST"],
                         capture_output=True, text=True)
    if enc.returncode != 0:
        print("  could not run ./pocsag_wav; build it with make first")
        return False

    with wave.open("/tmp/st.wav", "rb") as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)

    # that wav is the discriminator output, so re-modulate it onto I/Q at the
    # measured deviation and let the chain recover it
    bits = np.repeat(np.sign(a.astype(np.float32)), SAMPLE_RATE // AUDIO_RATE)
    inst = OFFSET_HZ + dev * bits
    phase = 2 * np.pi * np.cumsum(inst) / SAMPLE_RATE
    iq = 60 * np.exp(1j * phase)
    iq += (np.random.randn(len(iq)) + 1j * np.random.randn(len(iq))) * 2.0

    write_wav("/tmp/st_demod.wav", demodulate(iq))
    hits = decode("/tmp/st_demod.wav", mm)

    for b, inv, line in hits:
        print(f"  POCSAG{b}{' inverted' if inv else ''}: {line}")

    ok = any(str(ric) in line for _, _, line in hits)
    print("  chain is sound" if ok else "  CHAIN IS BROKEN: fix this before "
                                        "trusting a real capture")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--minutes", type=float, default=6.0)
    ap.add_argument("--chunk", type=float, default=6.0,
                    help="seconds per capture; a page is ~1.9s so keep it long")
    ap.add_argument("--gain", default="20")
    ap.add_argument("--freq", type=float, default=TARGET_HZ)
    ap.add_argument("--device", type=int, default=0)
    ap.add_argument("--expect-ric", type=int)
    ap.add_argument("--keep", metavar="FILE", help="also save the demodulated wav")
    ap.add_argument("--save-iq", metavar="FILE",
                    help="save the raw burst I/Q, so the demodulation can be "
                         "reworked without transmitting again")
    args = ap.parse_args()

    mm = find_multimon()
    print(f"using {mm}\n")

    if args.selftest:
        return 0 if selftest(mm) else 1

    tune = int(args.freq - OFFSET_HZ)
    gain = None if args.gain == "auto" else args.gain

    print("learning the noise floor...")
    floor = float(np.abs(capture(1.5, gain, tune, args.device)).mean())
    need = floor * (10 ** (8 / 20))
    print(f"  floor RMS {floor:.2f}, trigger above {need:.2f}")
    print(f"\nwaiting up to {args.minutes:.0f} min. Send the page now.\n")

    deadline = time.time() + args.minutes * 60
    n = 0
    while time.time() < deadline:
        n += 1
        iq = capture(args.chunk, gain, tune, args.device)
        rms = float(np.abs(iq).max())
        # peak, not mean: a 1.9s page inside a 6s chunk barely moves the mean
        loud = np.abs(iq)
        best = loud.argmax()
        window = iq[max(0, best - SAMPLE_RATE * 2):best + SAMPLE_RATE * 2]
        wrms = float(np.abs(window).mean())

        print(f"  chunk {n}: peak {rms:6.1f}  window RMS {wrms:6.2f}"
              + ("   <- BURST" if wrms > need else ""))

        if wrms <= need:
            continue

        burst, dur = extract_burst(iq, floor)
        if burst is None:
            print("    burst too short to analyse, ignoring")
            continue
        print(f"    burst is {dur:.2f}s long "
              f"(a 40 char page at 1200 baud is about 1.4s)")

        if args.save_iq:
            burst.astype(np.complex64).tofile(args.save_iq)
            print(f"    raw I/Q saved to {args.save_iq}")

        path = "/tmp/offair.wav"
        write_wav(path, demodulate(burst))
        if args.keep:
            write_wav(args.keep, demodulate(burst))
            print(f"    saved {args.keep}")

        print("\n== decoding the captured page ==")
        hits = decode(path, mm)

        if not hits:
            print("  nothing decoded from the off-air signal.")
            print("  Run --selftest to confirm the chain works, then suspect")
            print("  the transmitted framing, bit rate or deviation.")
            return 1

        for b, inv, line in hits:
            print(f"  POCSAG{b}{' inverted' if inv else ''}: {line}")

        if args.expect_ric:
            if any(str(args.expect_ric) in line for _, _, line in hits):
                print(f"\n  RIC {args.expect_ric} decoded off air: the "
                      f"transmission is valid POCSAG.")
            else:
                print(f"\n  decoded, but not RIC {args.expect_ric}. The signal "
                      f"is sound; the address is wrong.")
        return 0

    print("\nno burst detected.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
