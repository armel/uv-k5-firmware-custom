#!/usr/bin/env python3
"""Measure a UV-K5 POCSAG transmission with an RTL-SDR.

    ./sdr_measure.py carrier          # centre frequency and power only
    ./sdr_measure.py deviation        # also the FSK deviation

Answers the one question host tests cannot: whether the radio actually keys on
frequency, and whether the FSK deviation is the +/-4.5 kHz POCSAG wants. Neither
the hardware modem's deviation registers (REG_40, REG_70) nor the bit rate had
ever been calibrated against a real receiver.

Send a page from the radio while this is waiting. It used to expect the dedicated
CARRIER and DEV TEST modes the transmit screen had while the modem was being
brought up; those are gone, and an ordinary page measures the same things. A
page's preamble is 576 alternating bits, which is the same 1010 pattern the
deviation test used to send, so the deviation figure is measured on exactly the
signal it was before.

Tunes deliberately off-frequency so the RTL-SDR's DC spike does not sit on top
of the signal, then measures two ways:

  spectrum        peak positions, which for a 1010 pattern should be two tones
                  2*deviation apart
  FM discriminator  the derivative of the phase, which gives the instantaneous
                  frequency directly and so reads deviation without assuming
                  anything about the modulation shape

Transmit into a dummy load.
"""

import argparse
import os
import subprocess
import sys
import time
import tempfile

import numpy as np

TARGET_HZ = 153_225_000      # the pager's channel
OFFSET_HZ = 50_000           # tune below the signal, clear of the DC spike
SAMPLE_RATE = 1_024_000


def capture(seconds, gain, tune_hz, device=0):
    n = int(SAMPLE_RATE * seconds)
    path = tempfile.mktemp(suffix=".iq")

    cmd = ["rtl_sdr", "-f", str(tune_hz), "-s", str(SAMPLE_RATE),
           "-n", str(n), "-d", str(device)]
    cmd += ["-g", str(gain)] if gain is not None else ["-g", "0"]
    cmd += [path]

    print(f"capturing {seconds:.1f}s at {tune_hz/1e6:.4f} MHz "
          f"(signal should land at +{OFFSET_HZ/1e3:.0f} kHz)...")

    # "No supported devices found" right after a previous capture is usually the
    # USB device not yet released rather than a real absence, so settle and retry
    # instead of throwing away the operator's timing.
    for attempt in range(3):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(path):
            break
        if "No supported devices" in (r.stderr or "") and attempt < 2:
            print("  device busy, settling then retrying...")
            time.sleep(2)
            continue
        print(r.stderr[-500:], file=sys.stderr)
        raise SystemExit("rtl_sdr failed")

    raw = np.fromfile(path, dtype=np.uint8)
    os.unlink(path)

    # rtl_sdr gives interleaved unsigned 8-bit I/Q centred on 127.5
    iq = (raw[0::2].astype(np.float32) - 127.5) + 1j * (raw[1::2].astype(np.float32) - 127.5)
    return iq


def strongest_window(iq, win):
    """The transmission is a burst inside a longer capture, so find it."""
    power = np.abs(iq) ** 2
    if len(power) <= win:
        return iq
    # coarse energy profile, then take the most energetic window
    step = win // 4
    best, best_p = 0, -1.0
    for start in range(0, len(power) - win, step):
        p = power[start:start + win].mean()
        if p > best_p:
            best, best_p = start, p
    return iq[best:best + win]


def wait_for_burst(mode, gain, tune_hz, device, chunk=6.0, minutes=5.0, margin_db=8.0):
    """Capture repeatedly until a transmission actually shows up.

    Fixed capture windows meant coordinating a keypress with a window that had
    already started, and several windows were spent on captures containing
    nothing at all. This learns the noise floor first, then keeps capturing
    until something rises clearly above it, so the operator can press MENU
    whenever they are ready.
    """
    print("learning the noise floor...")
    base = capture(1.5, gain, tune_hz, device)
    floor = float(np.abs(base).mean())
    print(f"  noise floor RMS {floor:.2f}")

    need = floor * (10 ** (margin_db / 20))
    print(f"  will trigger above RMS {need:.2f} (+{margin_db:.0f} dB)")
    print(f"\nwaiting up to {minutes:.0f} min. Press MENU on the radio now.\n")

    deadline = time.time() + minutes * 60
    n = 0
    while time.time() < deadline:
        n += 1
        iq = capture(chunk, gain, tune_hz, device)
        win = strongest_window(iq, int(SAMPLE_RATE * 0.5))
        rms = float(np.abs(win).mean())
        rise = 20 * np.log10(max(rms, 1e-9) / max(floor, 1e-9))

        print(f"  chunk {n}: peak RMS {rms:6.2f}  ({rise:+.1f} dB vs floor)"
              + ("   <- BURST" if rms > need else ""))

        if rms > need:
            return win

    print("\nnothing detected. Is the radio transmitting on this frequency?")
    return None


def report_spectrum(iq, tune_hz, target_hz=TARGET_HZ):
    # Squelched for the same reason the deviation is: a spectrum of mostly noise
    # finds its peaks in the noise.
    iq = iq[open_region(iq)]

    n = 1 << int(np.floor(np.log2(len(iq))))
    x = iq[:n] * np.hanning(n)
    spec = np.abs(np.fft.fftshift(np.fft.fft(x))) ** 2
    freqs = np.fft.fftshift(np.fft.fftfreq(n, 1 / SAMPLE_RATE))

    db = 10 * np.log10(spec + 1e-12)
    db -= db.max()

    band = (freqs > OFFSET_HZ - 40_000) & (freqs < OFFSET_HZ + 40_000)
    bf, bd = freqs[band], db[band]

    # The two strongest peaks, not the outermost: square-wave FSK throws
    # sidebands well beyond the tones themselves, and taking the outermost
    # over-reads the deviation.
    first = bf[np.argmax(bd)]
    away = np.abs(bf - first) > 2_000
    second = bf[away][np.argmax(bd[away])] if np.any(away) else None
    second_db = bd[away].max() if np.any(away) else -99.0

    if second is not None and second_db > -10.0:
        lo, hi = sorted((first, second))
        centre = (lo + hi) / 2
        print(f"  two tones at {tune_hz+lo:,.0f} and {tune_hz+hi:,.0f} Hz")
        print(f"    separation {hi-lo:,.0f} Hz  ->  deviation +/-{(hi-lo)/2:,.0f} Hz")
    else:
        centre = first
        print(f"  single tone: unmodulated carrier")

    # Against the frequency actually asked for, not the default: --freq used to be
    # ignored here, so the verdict was nonsense on any other channel.
    print(f"  centre {tune_hz+centre:,.0f} Hz "
          f"(error {tune_hz+centre-target_hz:+,.0f} Hz from {target_hz/1e6:.4f} MHz)")

    # noise floor from outside the signal band, indexing the full array
    print(f"  signal is {-np.median(db[~band]):.0f} dB above the noise median")


def open_region(iq, open_at=0.25, pad_s=0.002):
    """The part of a capture that actually has a carrier in it.

    Squelch is not optional here, and leaving it out was a real defect: where there
    is no carrier the discriminator output is not quiet, it is enormous, because
    differentiating the phase of noise gives excursions of hundreds of kHz. Any
    statistic taken across a record where the burst is a fraction of the whole is
    then meaningless, and the DC reference has to come from inside the burst too.

    Found while writing the AFSK equivalent in tools/aprs/sdr_afsk.py, where the
    same shape of code reported a carrier 84kHz off with 28kHz of deviation for a
    signal that turned out to be flawless.

    A fraction of the peak rather than a multiple of the noise floor, so this
    behaves whether the burst fills the capture or is a small part of it. Returns
    the slice holding the longest burst.
    """
    mag  = np.abs(iq)
    mask = mag > (mag.max() * open_at)

    pad  = int(SAMPLE_RATE * pad_s)
    mask = np.convolve(mask, np.ones(2 * pad + 1), mode="same") > 0

    # The longest contiguous run, not every sample above the threshold: boolean
    # indexing would splice separate bursts and invent a transition at the join.
    edges = np.flatnonzero(np.diff(np.concatenate(([False], mask, [False]))))
    starts, ends = edges[0::2], edges[1::2]

    if len(starts) == 0:
        return slice(0, len(iq))

    k = int(np.argmax(ends - starts))

    return slice(int(starts[k]), int(ends[k]))


def report_deviation(iq, baud=1200.0):
    """Instantaneous frequency from the phase derivative.

    Differentiating phase amplifies noise badly, and on an 8-bit RTL-SDR the
    raw result is unusable: percentiles of it get dragged outward by spikes at
    the symbol transitions, which over-read the deviation by nearly 2x. So
    smooth the *frequency* over a fraction of a bit, then take the median of
    the samples either side of the overall median. Two robust cluster centres
    beat percentiles when the signal is genuinely bimodal.
    """
    burst = open_region(iq)

    print(f"  burst is {(burst.stop - burst.start) / SAMPLE_RATE * 1000:.0f} ms of "
          f"the {len(iq) / SAMPLE_RATE * 1000:.0f} ms examined")

    inst = np.diff(np.unwrap(np.angle(iq[burst]))) * SAMPLE_RATE / (2 * np.pi)

    # smooth over a quarter of a bit: long enough to kill the noise, short
    # enough to leave the two levels distinct
    k = max(4, int(SAMPLE_RATE / (baud * 4)))
    inst = np.convolve(inst, np.ones(k) / k, mode="valid")

    # drop the edges, where the smoothing window straddles transitions
    trim = len(inst) // 20
    inst = inst[trim:-trim]

    centre = np.median(inst)
    upper, lower = inst[inst > centre], inst[inst < centre]

    if len(upper) < 10 or len(lower) < 10:
        print(f"  FM discriminator: single level at {centre:+,.0f} Hz "
              f"(unmodulated)")
        return

    hi, lo = np.median(upper), np.median(lower)
    dev = (hi - lo) / 2

    print(f"  FM discriminator: low {lo:+,.0f} Hz  high {hi:+,.0f} Hz")
    print(f"    centre offset {centre:+,.0f} Hz from the tuned point")
    print(f"    separation {hi - lo:,.0f} Hz  ->  deviation +/-{dev:,.0f} Hz")

    want = 4500
    err = abs(dev - want) / want * 100
    verdict = ("close to the POCSAG nominal" if err < 20 else
               "WELL OFF the POCSAG nominal")
    print(f"    POCSAG wants +/-{want} Hz: {err:.0f}% off, {verdict}")

    # a 1010 pattern is a square wave at half the bit rate
    sign = np.sign(inst - centre)
    toggles = np.count_nonzero(np.diff(sign) != 0)
    secs = len(inst) / SAMPLE_RATE
    print(f"    ~{toggles / (2 * secs):,.0f} Hz toggle rate "
          f"(1010 at {baud:.0f} baud would be {baud/2:.0f} Hz)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["carrier", "deviation"])
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="fixed capture window; default 0 waits for a burst instead")
    ap.add_argument("--minutes", type=float, default=5.0,
                    help="how long to wait for a burst")
    ap.add_argument("--gain", default="20",
                    help="tuner gain in dB, or 'auto'. Lower it if overloading")
    ap.add_argument("--baud", type=float, default=1200.0)
    ap.add_argument("--freq", type=float, default=TARGET_HZ)
    ap.add_argument("--device", type=int, default=0)
    args = ap.parse_args()

    tune = int(args.freq - OFFSET_HZ)
    gain = None if args.gain == "auto" else args.gain

    if args.seconds > 0:
        iq = capture(args.seconds, gain, tune, args.device)
        burst = strongest_window(iq, int(SAMPLE_RATE * 0.5))
        rms_all, rms_burst = np.abs(iq).mean(), np.abs(burst).mean()
        print(f"burst is {20*np.log10(rms_burst/max(rms_all,1e-9)):+.1f} dB "
              f"above the capture average")
        if rms_burst < 2.0:
            print("\nWARNING: almost no signal; probably nothing transmitted.")
            return 1
    else:
        burst = wait_for_burst(args.mode, gain, tune, args.device,
                               minutes=args.minutes)
        if burst is None:
            return 1

    print("\n== spectrum ==")
    report_spectrum(burst, tune, args.freq)

    if args.mode == "deviation":
        print("\n== deviation ==")
        report_deviation(burst, args.baud)

    return 0


if __name__ == "__main__":
    sys.exit(main())
