#!/usr/bin/env python3
"""Dump a Philips PCD5002 paging decoder's EEPROM over I2C using a CH341A.

    py read_pcd5002_ch341.py pcd5002.bin

Then decode it with:

    py pcd5002.py pcd5002.bin

Why not AsProgrammer
--------------------
AsProgrammer drives chip profiles (24xx / 25xx / 93xx), each implementing that
family's fixed protocol. A 24Cxx read is "slave address, memory address, then
read". The PCD5002 is not a memory: its EEPROM is behind index registers, so
the sequence is "write index 07H with the row/column, write index 0AH, then
read". No 24xx profile can express that, and the PCD5002 answers at 0x27 rather
than in the 0x50-0x57 range those profiles use.

The CH341A hardware is fine though. CH341StreamI2C() performs an arbitrary
write-then-read transaction, which is exactly what is needed, so this talks to
the same dongle through CH341DLL.DLL directly.

READ THIS BEFORE CONNECTING
---------------------------
* Voltage. The PCD5002's absolute maximum on SDA and SCL is VPR + 0.8 V, and a
  pager typically runs VPR around 2.7 V, so roughly 3.5 V. A stock CH341A
  board drives I2C at 5 V and will damage the chip. Use a 3.3 V modified
  board, or a 3.3 V adapter (FT232H, Bus Pirate, Raspberry Pi). Measure VDD on
  pin 11 against VSS on pin 12 first and confirm what you are working with.

* Put the decoder in its OFF state before reading. Datasheet 8.50: the EEPROM
  must not be accessed while the receiver is active. DON (pin 3) low is OFF,
  which is normally where a switched-off pager leaves it.

* Pull-ups to the pager's own rail, not to the adapter's. I2C is open-drain, so
  this keeps the bus inside the chip's limits.

* Reading cannot corrupt anything: writes need the programming enable bit in
  the control register, which nothing here touches.

* The pager's own controller shares this bus. With the pager switched off it
  should be quiet, but a retry on failure is normal rather than alarming.

Untested against real hardware; the transaction sequence follows the datasheet
but the wiring is yours to verify.
"""

import argparse
import ctypes
import sys
import time
from collections import Counter
from textwrap import dedent

PCD5002_ADDR = 0x27          # 7-bit; write byte 0x4E, read byte 0x4F
EEPROM_24C32_ADDR = 0x50     # the message EEPROM, useful as a positive control
IDX_CONTROL = 0x00           # write: see datasheet Table 18
IDX_EEPROM_PTR = 0x07        # bits 5:3 row (0-7), bits 2:0 column (0-5)
IDX_EEPROM_DATA = 0x0A       # auto-increments the pointer on each access
EEPROM_BYTES = 48            # 8 rows x 6 columns

# CH341 stream mode low bits select the I2C clock rate.
I2C_SPEEDS = {20: 0, 100: 1, 400: 2, 750: 3}

# set from --expect; lets scan() compare 0x50 against a known dump
EXPECT_FILE = [None]


def eeprom_addresses():
    """The 48 real addresses. Columns 6 and 7 do not exist in the matrix."""
    return [(row << 3) | col for row in range(8) for col in range(6)]


class CH341:
    # The WCH driver ships a DLL per architecture. Loading the wrong one raises
    # OSError with winerror 193, which Windows words unhelpfully as
    # "%1 is not a valid Win32 application".
    DLLS_64 = ("CH341DLLA64.DLL", "CH341DLLA64.dll", "CH341DLL64.DLL")
    DLLS_32 = ("CH341DLL.DLL", "CH341DLL.dll")

    def __init__(self, index=0, speed_khz=100):
        self.dll = self._load()
        self.index = index

        # Be explicit about the ABI rather than relying on ctypes defaults.
        self.dll.CH341StreamI2C.argtypes = [
            ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
            ctypes.c_ulong, ctypes.c_void_p]
        self.dll.CH341StreamI2C.restype = ctypes.c_int

        self.dll.CH341OpenDevice.restype = ctypes.c_void_p
        if self.dll.CH341OpenDevice(index) in (None, 0, -1, 0xFFFFFFFFFFFFFFFF):
            raise SystemExit(
                "CH341OpenDevice failed: no dongle found, or another program "
                "(AsProgrammer?) still has it open")

        self.dll.CH341SetStream(index, I2C_SPEEDS[speed_khz])

    @classmethod
    def _load(cls):
        bits = ctypes.sizeof(ctypes.c_void_p) * 8
        preferred = cls.DLLS_64 if bits == 64 else cls.DLLS_32
        other = cls.DLLS_32 if bits == 64 else cls.DLLS_64

        mismatched = []

        for name in preferred + other:
            try:
                return ctypes.WinDLL(name)
            except OSError as exc:
                # 193 means the DLL is the wrong architecture for this Python
                if getattr(exc, "winerror", None) == 193:
                    mismatched.append(name)

        msg = [f"could not load the CH341 DLL (this Python is {bits}-bit)."]

        if mismatched:
            msg += [
                f"found {', '.join(mismatched)}, but it is the wrong "
                f"architecture for {bits}-bit Python.",
                "",
                "AsProgrammer is 32-bit, so a DLL copied from its folder is a",
                "32-bit CH341DLL.DLL. Either:",
                f"  - get {cls.DLLS_64[0]} from the WCH CH341 driver package",
                "    (the 64-bit build) and put it beside this script, or",
                "  - run this with 32-bit Python instead.",
            ]
        else:
            msg += [
                "install the WCH CH341 driver, then put the DLL matching your",
                f"Python beside this script: {cls.DLLS_64[0] if bits == 64 else cls.DLLS_32[0]}",
            ]

        raise SystemExit("\n".join(msg))

    def close(self):
        self.dll.CH341CloseDevice(self.index)

    def transfer(self, write_bytes, read_len=0):
        """One I2C transaction: write, then optionally read with repeated start."""
        wbuf = (ctypes.c_ubyte * len(write_bytes))(*write_bytes)
        rbuf = (ctypes.c_ubyte * max(read_len, 1))()

        ok = self.dll.CH341StreamI2C(
            self.index,
            ctypes.c_ulong(len(write_bytes)), ctypes.byref(wbuf),
            ctypes.c_ulong(read_len), ctypes.byref(rbuf))

        if not ok:
            raise IOError("CH341StreamI2C failed (no ACK? check wiring, "
                          "pull-ups and that the pager is powered)")

        return bytes(rbuf[:read_len])



    def select_index(self, addr7, index):
        """Message form (a): S ADDR+W INDEX P."""
        self.transfer([(addr7 << 1) | 0, index])

    def read_data(self, addr7, count, shape="separate"):
        """Message form (b): S ADDR+R DATA.. P, as a transaction of its own.

        The datasheet gives only two message forms, and a read is a separate
        message that begins with the address and the read bit; the index it
        draws from is whichever was written last. A combined write plus
        repeated-start read is neither form, and issuing it was producing bytes
        shifted by one bit.
        """
        if shape == "separate":
            return self.transfer([(addr7 << 1) | 1], count)
        # kept for comparison only
        return self.transfer([(addr7 << 1) | 0, IDX_EEPROM_DATA], count)

    def probe(self, addr7):
        """True if a device acknowledges its address. Harmless: no data moves."""
        try:
            self.transfer([(addr7 << 1) | 0])
            return True
        except IOError:
            return False



def read_24c32(dev, count=16, offset=0):
    """Read from the message EEPROM at 0x50, as an end-to-end control.

    A 24Cxx read is the ordinary kind: slave address, a 2 byte word address,
    then a repeated start and the data. Proving this works separates "I2C reads
    do not work at all" from "the PCD5002 is refusing the access", which look
    identical if you only probe for an address ACK.
    """
    wr = (EEPROM_24C32_ADDR << 1) | 0     # 0xA0
    return dev.transfer([wr, (offset >> 8) & 0xFF, offset & 0xFF], count)


def check_control(dev, expect_file=None):
    """True if the I2C read path demonstrably works."""
    try:
        data = read_24c32(dev, 16)
    except IOError as exc:
        print(f"  reading 0x50 failed outright: {exc}")
        return False

    print("  0x50 first 16 bytes: " + " ".join(f"{b:02X}" for b in data))

    if all(b == data[0] for b in data):
        print(f"  every byte is {data[0]:#04x}, so data is not really moving")
        return False

    if expect_file:
        want = open(expect_file, "rb").read()[:16]
        if bytes(data) == want:
            print("  matches the reference dump exactly: I2C reads are good")
        else:
            print("  does NOT match the reference dump:")
            print("    expected " + " ".join(f"{b:02X}" for b in want))
            return False
    else:
        print("  varied data, so the I2C read path works")

    return True


def scan(dev):
    """Prove the wiring before trusting a dump.

    The 24C32 message EEPROM at 0x50 is the positive control: it is already
    known to work, so if it does not answer here the problem is the wiring or
    the bus, not the PCD5002.
    """
    print("scanning I2C bus...")
    found = [a for a in range(0x08, 0x78) if dev.probe(a)]

    for a in found:
        note = ""
        if a == PCD5002_ADDR:
            note = "  <- PCD5002 paging decoder"
        elif a == EEPROM_24C32_ADDR:
            note = "  <- 24C32 message EEPROM (positive control)"
        elif 0x50 <= a <= 0x57:
            note = "  <- a 24Cxx family EEPROM"
        print(f"  0x{a:02X}{note}")

    if not found:
        print("  nothing responded.")
        print("  check SDA/SCL are not swapped, that GND is common, that the")
        print("  pager has power, and that pull-ups reach the pager's rail.")
        return False

    control = EEPROM_24C32_ADDR in found
    target = PCD5002_ADDR in found

    print()
    if control:
        print("  0x50 answered, so the bus and wiring are good")
    else:
        print("  0x50 did NOT answer. Since that chip is known to read, treat")
        print("  this as a wiring problem rather than a PCD5002 problem")

    if control:
        print()
        print("verifying the read path against the known-good EEPROM at 0x50")
        check_control(dev, EXPECT_FILE[0])

    print()
    if target:
        print("  0x27 answered: the decoder is reachable, go ahead and dump")
    else:
        print("  0x27 did NOT answer. If 0x50 did, then the bus is fine and")
        print("  the decoder is either unpowered or held in reset (RST pin 7)")

    return target



def read_register(dev, wr, index, shape="separate"):
    """Select the index (form a), then read it as its own message (form b)."""
    dev.select_index(PCD5002_ADDR, index)
    return dev.read_data(PCD5002_ADDR, 1, shape)[0]




def probe_pointer(dev, wr, delay):
    """Compare the two read message shapes, and watch the address pointer.

    The datasheet gives only two message forms: a write (address, index, data)
    and a read (address with the read bit, then data). A read is its own
    message and draws from whichever index was written last. Issuing a combined
    write plus repeated-start read is neither form, and was returning bytes
    shifted by one bit, so this measures both shapes rather than assuming.

    Pointer readbacks are masked to 6 bits because D7 and D6 are documented as
    unused and undefined when read.
    """
    print("== address pointer and message shape ==")

    def ptr(shape):
        time.sleep(delay)
        return read_register(dev, wr, IDX_EEPROM_PTR, shape) & 0x3F

    def data(shape):
        time.sleep(delay)
        dev.select_index(PCD5002_ADDR, IDX_EEPROM_DATA)
        time.sleep(delay)
        return dev.read_data(PCD5002_ADDR, 1, shape)[0]

    for shape in ("separate", "combined"):
        label = ("separate read message, as the datasheet documents"
                 if shape == "separate" else
                 "combined write plus repeated start, what we did before")
        print(f"\n-- {shape}: {label}")

        start_addr = 0x10
        dev.transfer([wr, IDX_EEPROM_PTR, start_addr])
        got = ptr(shape)
        print(f"   pointer written 0x{start_addr:02X}, reads back 0x{got:02X}"
              + ("  ok" if got == start_addr else "  <- MISMATCH"))

        print("   pointer walk:", end=" ")
        walk = []
        for _ in range(5):
            data(shape)
            walk.append(ptr(shape))
        print(" ".join(f"{p:02X}" for p in walk))

        stable = []
        for addr in (0x10, 0x11, 0x12):
            vals = []
            for _ in range(4):
                time.sleep(delay)
                dev.transfer([wr, IDX_EEPROM_PTR, addr])
                vals.append(data(shape))
            ok = len(set(vals)) == 1
            stable.append(ok)
            print(f"   0x{addr:02X} x4: " + " ".join(f"{v:02X}" for v in vals)
                  + ("   stable" if ok else "   UNSTABLE"))

        print(f"   verdict: {'USABLE' if all(stable) else 'unreliable'}")

    print(dedent("""
        If "separate" is stable and "combined" is not, the transaction shape was
        the problem all along and the dump will now work. If both are unstable,
        it is the wiring: shorter leads and stronger pull-ups.
        """).strip())

    return True


def stress(dev, delay, rounds=200):
    """Is anything else disturbing this bus?

    A competing master, or marginal signal integrity, shows up as an
    intermittent failure rate under repetition. The 24C32 at 0x50 is the ideal
    probe: it is a simple, fast, known-good chip, so any failure reading it is
    the bus rather than the slow decoder refusing.

    This answers "do I need to silence the pager's controller" by measurement,
    which beats going looking for a reset pin on an undocumented part.
    """
    wr = (PCD5002_ADDR << 1) | 0

    try:
        reference = read_24c32(dev, 16)
    except IOError as exc:
        print(f"could not even start: {exc}")
        return False

    fails = mismatches = status_fails = 0
    statuses = Counter()

    print(f"hammering the bus {rounds} times...")
    for i in range(rounds):
        time.sleep(delay)
        try:
            if read_24c32(dev, 16) != reference:
                mismatches += 1
        except IOError:
            fails += 1

        time.sleep(delay)
        try:
            statuses[read_register(dev, wr, 0x00)] += 1
        except IOError:
            status_fails += 1

    print(f"  0x50 reads:      {rounds - fails - mismatches}/{rounds} clean, "
          f"{fails} failed, {mismatches} wrong data")
    print(f"  0x27 status:     {rounds - status_fails}/{rounds} answered")
    print("  status values:   "
          + ", ".join(f"0x{v:02X} x{n}" for v, n in statuses.most_common(4)))

    # The status register is event driven and self clearing: per Table 17 and
    # its note, D3, D4 and D7 reset on every read, while D5 (out of range) and
    # D6 persist. So varying values are correct behaviour, not corruption, and
    # it would be wrong to treat instability here as a fault.
    bits = {0x01: "call data", 0x04: "RAM data", 0x10: "alert timeout",
            0x20: "out of range", 0x40: "BAT/RXE", 0x80: "periodic timer"}
    seen_bits = {name for v in statuses for m, name in bits.items() if v & m}
    print("  status bits seen: " + ", ".join(sorted(seen_bits)))

    d6 = sum(n for v, n in statuses.items() if v & 0x40)
    print(f"  D6 (BAT/RXE) set in {d6}/{rounds} reads")

    corruption = mismatches / rounds if rounds else 0
    clean = fails == 0 and mismatches == 0 and status_fails == 0

    print()
    if clean:
        print("  The bus is clean over this many transactions, so nothing else")
        print("  is fighting for it, and signal integrity is good.")
    elif fails == 0 and mismatches:
        print(f"  {mismatches}/{rounds} reads of 0x50 returned WRONG DATA with no")
        print("  outright failures. A competing master causes lost arbitration")
        print("  and failed transfers, not silent corruption, so this is signal")
        print(f"  integrity: roughly {100*corruption:.0f}% of bytes are landing wrong.")
        print()
        print("  Over a 48 byte dump that is near certain failure. Fix it at the")
        print("  wiring, since delay cannot help a corrupted byte:")
        print("    - shorter leads, this matters most")
        print("    - stronger pull-ups, try 2k2 to the pager's rail")
        print("    - a short direct ground return next to the data pair")
    else:
        print(f"  {fails}/{rounds} transfers failed outright, which does suggest")
        print("  another master on the bus or a wiring fault.")

    return clean


def diagnose(dev, wr):
    """Work out why reads come back as mostly 1 bits.

    If the decoder acknowledges its address but never drives read data, SDA
    floats high and every byte reads with its top bits set. Datasheet 8.50 says
    the EEPROM cannot be read while the receiver is active, so the first thing
    to establish is whether RXE is still asserted.
    """
    print("== diagnostics ==")

    # Prove reads work at all before believing anything read back. Without
    # this, a corrupt status byte with its top bits set reads as "RXE active"
    # and sends you chasing an interlock that may not be the problem.
    print("  control: reading the known-good EEPROM at 0x50")
    if not check_control(dev, EXPECT_FILE[0]):
        print()
        print("  reads from 0x50 do not work either, so this is the bus or the")
        print("  transfer setup, NOT the decoder. Nothing read from 0x27 means")
        print("  anything until this passes. Try --speed 20, check pull-up")
        print("  strength, and keep the leads short.")
        return False

    print()

    # Control D2 = 1 makes status bit D6 report RXE rather than BAT.
    # D4 = 0 also asks for OFF status; D1 = 0 keeps writes disabled.
    dev.transfer([wr, IDX_CONTROL, 0x04])
    time.sleep(0.05)

    status = read_register(dev, wr, 0x00)
    rxe = bool(status & 0x40)

    print(f"  status register  0x{status:02X}")
    print(f"  RXE (receiver)   {'ACTIVE' if rxe else 'inactive'}")

    if rxe:
        print()
        print("  This is the problem. Per 8.50 the EEPROM cannot be read while")
        print("  the receiver is active, so the chip ACKs its address but does")
        print("  not drive data, and you read a floating bus.")
        print()
        print("  Selecting OFF status over I2C only works while DON (pin 3) is")
        print("  LOW. Measure DON: if the Kappa controller is holding it HIGH,")
        print("  hold that controller in reset, which also stops it competing")
        print("  for the bus as a second master.")
    else:
        print("  receiver is off, so 8.50 is satisfied and reads should work")

    # Does the same address read the same twice?
    a = eeprom_addresses()[0]
    vals = []
    for _ in range(4):
        dev.transfer([wr, IDX_EEPROM_PTR, a])
        vals.append(dev.transfer([wr, IDX_EEPROM_DATA], 1)[0])

    print(f"  address 0x{a:02X} read 4x: " + " ".join(f"{v:02X}" for v in vals))
    if len(set(vals)) == 1:
        print("    stable")
    else:
        print("    UNSTABLE: the bus is unreliable, so no dump can be trusted.")
        print("    Suspect a second master (the pager's controller) or marginal")
        print("    pull-ups / rise time.")

    return not rxe and len(set(vals)) == 1


def force_off(dev, wr):
    """Put the decoder in OFF status so the EEPROM can be read.

    Datasheet 8.50: the EEPROM must not be accessed while the receiver is
    active (RXE = 1), and a returned 0xFF for every byte is what a refused
    access looks like. Table 18, control register at index 00H:

        D4 = 0  decoder in OFF status (while DON = 0)
        D3 = 0  receiver not continuously enabled
        D1 = 0  EEPROM programming disabled, so this cannot write anything

    Note the OFF status only holds while DON (pin 3) is low. If the pager's
    own controller drives DON high the decoder stays ON regardless of this.
    """
    dev.transfer([wr, IDX_CONTROL, 0x00])
    time.sleep(0.05)



def read_consensus(dev, wr, delay, votes=4, tries=9, health_every=8,
                   shape="separate"):
    """Read all 48 bytes, requiring each to read the same several times over.

    Comparing two whole dumps was the wrong granularity: any single glitch
    threw away 48 good bytes and told us nothing about where the trouble was.
    Per byte consensus localises it, and since a single address was observed
    reading stably four times in a row, agreement per byte is achievable.

    The decoder runs from a 76.8 kHz crystal, so it is far slower than the bus.
    Back to back transactions wedge it: the bus was seen to stop acknowledging
    entirely partway through a 96 transaction dump. Hence the pacing, and the
    periodic check that the bus is still alive so a wedge is reported where it
    happens rather than as mystery data.
    """
    out = bytearray()

    for i, addr in enumerate(eeprom_addresses()):
        if i and i % health_every == 0:
            time.sleep(delay * 4)
            if not dev.probe(EEPROM_24C32_ADDR):
                raise IOError(
                    f"the bus stopped responding after {i} bytes: even 0x50 no "
                    f"longer ACKs. The decoder is most likely holding SCL low; "
                    f"try a longer --delay")

        seen = []
        for _ in range(tries):
            time.sleep(delay)
            dev.transfer([wr, IDX_EEPROM_PTR, addr])
            time.sleep(delay)
            dev.select_index(PCD5002_ADDR, IDX_EEPROM_DATA)
            seen.append(dev.read_data(PCD5002_ADDR, 1, shape)[0])

            # A bare majority is not enough. The glitch value is a fixed 0xFF,
            # so a few glitches in a row can outvote the truth: at a 20% glitch
            # rate that was observed returning wrong data. Demand a strong
            # agreement and at most one dissenting read, and otherwise refuse,
            # because a bus that noisy needs fixing rather than voting around.
            best, count = Counter(seen).most_common(1)[0]
            if count >= votes and (len(seen) - count) <= 1:
                break
        else:
            raise IOError(f"address 0x{addr:02X} never settled, read "
                          + " ".join(f"{v:02X}" for v in seen)
                          + " -- the bus is too noisy to trust; raise --delay")

        out.append(best)

    return bytes(out)


def read_block(dev, wr, delay=0.005):
    """One pointer set, then read straight through on auto-increment."""
    dev.transfer([wr, IDX_EEPROM_PTR, 0x00])
    time.sleep(delay)
    dev.select_index(PCD5002_ADDR, IDX_EEPROM_DATA)
    return dev.read_data(PCD5002_ADDR, EEPROM_BYTES)


def read_single(dev, wr, delay=0.005):
    """Set the pointer per byte. Slower, but 8.51 permits single byte reads
    and it does not depend on auto-increment surviving a long transaction."""
    out = bytearray()
    for addr in eeprom_addresses():
        time.sleep(delay)
        dev.transfer([wr, IDX_EEPROM_PTR, addr])
        time.sleep(delay)
        dev.select_index(PCD5002_ADDR, IDX_EEPROM_DATA)
        out += dev.read_data(PCD5002_ADDR, 1)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", help="file to write the 48 byte dump to")
    ap.add_argument("--scan", action="store_true",
                    help="probe the bus and exit; do this first")
    ap.add_argument("--index", type=int, default=0, help="CH341 device index")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--no-off", action="store_true",
                    help="do not command the decoder into OFF status first")
    ap.add_argument("--block", action="store_true",
                    help="also try the fast unverified block read")
    ap.add_argument("--expect", metavar="FILE",
                    help="an earlier 24C32 dump, to verify reads byte for byte")
    ap.add_argument("--diag", action="store_true",
                    help="report RXE state and read stability, then exit")
    ap.add_argument("--stress", type=int, metavar="N", nargs="?", const=200,
                    help="hammer the bus N times to detect a competing master")
    ap.add_argument("--probe", action="store_true",
                    help="report how the EEPROM address pointer behaves")
    ap.add_argument("--speed", type=int, default=20, choices=sorted(I2C_SPEEDS),
                    help="I2C clock in kHz; 20 suits this slow part")
    ap.add_argument("--delay", type=float, default=5.0, metavar="MS",
                    help="pause between transactions; raise if the bus wedges")
    args = ap.parse_args()

    if not args.scan and not args.out:
        ap.error("give an output file, or --scan")

    EXPECT_FILE[0] = args.expect

    dev = CH341(args.index, args.speed)

    if args.scan:
        ok = scan(dev)
        dev.close()
        return 0 if ok else 1

    if args.probe:
        ok = probe_pointer(dev, (PCD5002_ADDR << 1) | 0, args.delay / 1000.0)
        dev.close()
        return 0 if ok else 1

    if args.stress:
        ok = stress(dev, args.delay / 1000.0, args.stress)
        dev.close()
        return 0 if ok else 1

    if args.diag:
        ok = diagnose(dev, (PCD5002_ADDR << 1) | 0)
        dev.close()
        return 0 if ok else 1

    wr = (PCD5002_ADDR << 1) | 0      # 0x4E

    # A block read is one transaction, but if auto-increment does not survive
    # it then per-byte pointer sets will. Try the cheap way first unless told.
    delay = args.delay / 1000.0

    def consensus(dev, wr):
        return read_consensus(dev, wr, delay)

    consensus.__name__ = "read_consensus"

    # read_consensus already sets the address pointer per byte, so it is the
    # single-byte method, just paced and verified. There is no reason to offer
    # an unpaced version of the same thing.
    modes = [consensus] + ([read_block] if args.block else [])

    last = None
    for mode in modes:
        for attempt in range(1, args.retries + 1):
            try:
                if not args.no_off:
                    force_off(dev, wr)

                data = mode(dev, wr) if mode is consensus else mode(dev, wr, delay)

                if len(data) != EEPROM_BYTES:
                    raise IOError(f"short read: {len(data)} of {EEPROM_BYTES}")

                if all(b == data[0] for b in data):
                    raise IOError(f"every byte read back as {data[0]:#04x}")

                # A part-failed read is a few real bytes then 0xFF, which is
                # not uniform and so slips past the check above. Only 20 of the
                # 48 bytes are legitimately unused, so a mostly-0xFF image
                # means the transfer stopped early.
                blank = sum(1 for b in data if b == 0xFF)
                if blank >= 40:
                    raise IOError(f"{blank} of {EEPROM_BYTES} bytes are 0xFF, "
                                  "so the read stopped early")

                # Two reads of the same chip must agree. This is the check that
                # actually caught a bad bus: two dumps differed in byte 0.
                # read_consensus has already agreed with itself per byte.
                again = data if mode is consensus else mode(dev, wr, delay)
                if again != data:
                    diff = sum(1 for x, y in zip(data, again) if x != y)
                    raise IOError(f"two reads disagreed in {diff} of "
                                  f"{EEPROM_BYTES} bytes, so the bus is not "
                                  "reliable")

                open(args.out, "wb").write(data)
                print(f"wrote {len(data)} bytes to {args.out} "
                      f"({mode.__name__})")
                print("\n".join(
                    f"  {(r << 3):02X}  "
                    + " ".join(f"{b:02X}" for b in data[r*6:(r+1)*6])
                    for r in range(8)))
                print(f"\nnow run:  py pcd5002.py {args.out}")
                dev.close()
                return 0

            except IOError as exc:
                last = exc
                print(f"{mode.__name__} attempt {attempt}/{args.retries}: {exc}",
                      file=sys.stderr)

    dev.close()

    print(f"\ngiving up: {last}", file=sys.stderr)

    # Tailor the advice to what actually happened rather than reciting every
    # possibility. Whether 0x50 still answers is the discriminator.
    print("\nchecking whether the bus survived the attempt...", file=sys.stderr)
    alive = dev.probe(EEPROM_24C32_ADDR)

    if not alive:
        print(dedent(f"""
            0x50 no longer even acknowledges, though it did at the start.
            The dump wedged the bus: the decoder is almost certainly holding
            SCL low because it cannot keep up. It runs from a 76.8 kHz
            crystal, so it is far slower than the link.

            Raise the pacing, which is the fix for this:

                --delay 20     (currently {args.delay:g} ms)
                --delay 50

            Power cycle the pager first to clear the wedged state.
            """).strip(), file=sys.stderr)
    else:
        print(dedent("""
            0x50 still answers, so the bus is alive and this is specific to
            reading the decoder's EEPROM. Worth checking, in order:

              1. --stress, to find out whether something else is driving the
                 bus intermittently.
              2. A longer --delay: the part is slow and may need more time
                 between the pointer write and the data read.
              3. DON (pin 3) and RST (pin 7), but only if --diag actually
                 reports RXE active. Earlier runs showed it inactive with a
                 stable status register, so do not assume the interlock.
            """).strip(), file=sys.stderr)

    return 1


if __name__ == "__main__":
    sys.exit(main())
