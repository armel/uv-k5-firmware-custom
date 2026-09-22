#!/usr/bin/env python3
"""Dump a Philips PCD5002 paging decoder's EEPROM using a Raspberry Pi.

    ./read_pcd5002_pi.py pcd5002.bin
    ./pcd5002.py pcd5002.bin

Why a Pi, and why NOT /dev/i2c-1
--------------------------------
The PCD5002 runs from a 76.8 kHz crystal and stretches SCL. A CH341A does not
wait for that, so bytes arrive shifted one bit, which is why it reads the
pager's 24C32 perfectly but cannot read this chip at all.

The Pi's *hardware* I2C has the same weakness: the BCM2835/2837 BSC block has a
long-standing clock stretching bug, so /dev/i2c-1 on GPIO2/GPIO3 would fail the
same way. Use the i2c-gpio overlay instead, which bit-bangs I2C in software and
honours stretching properly. In /boot/config.txt (or /boot/firmware/config.txt):

    dtoverlay=i2c-gpio,bus=3,i2c_gpio_sda=23,i2c_gpio_scl=24,i2c_gpio_delay_us=25

then reboot. That gives /dev/i2c-3 at roughly 20 kHz.

This is its own line. Do not fold it into an existing dtoverlay such as
vc4-kms-v3d: everything after the first comma is parsed as parameters to that
overlay, so you would silently get no I2C bus. Note also that any dtparam=
lines attach to the most recently declared dtoverlay, so keep this line last
in the file, and check it is not inside a conditional section like [pi4] that
does not match. After rebooting, /dev/i2c-3 should exist and

    i2cdetect -y 3

should list both 0x27 and 0x50.

WIRING (Pi 3B 40-pin header)
    PCD5002 pin 9  SDA  -> physical pin 16 (GPIO23)
    PCD5002 pin 10 SCL  -> physical pin 18 (GPIO24)
    PCD5002 pin 12 VSS  -> physical pin 14 or 20 (GND)

PULL-UPS: start with none of your own. The pager board already has I2C
pull-ups to its own rail, since its controller, this decoder and the 24C32
share that bus, so borrowing them is both sufficient and correct.

Do not rely on the Pi's internal pull-ups: at 50-65 kOhm they are far too weak
for I2C and the slow edges are exactly what corrupts bytes. GPIO9-27 also
default to pull-DOWN, which is actively wrong here. The 1k8 pull-ups fitted on
the board are only on GPIO2/GPIO3, which we are avoiding anyway.

Do not add pull-ups to the Pi's 3.3V either. SDA/SCL absolute maximum is
VPR + 0.8 V, so 3.3V is only safe if VPR is around 2.7V; on a single cell pager
with the voltage doubler off it would be over the limit. It also sets two
supplies against each other, pushing current from the Pi through the board's
existing pull-ups into the pager's rail. If edges really are too slow, add 2k2
to the PAGER's rail, or just raise i2c_gpio_delay_us, which software I2C lets
you do without limit.

Measure VPR (pin 8), VDD (pin 11) against VSS (pin 12) before connecting, so
you know your actual voltage ceiling.

Do not connect the Pi's 3.3V or 5V to the pager. Leave it on its own cell,
switched off; ground is the only rail shared. Pi GPIO is 3.3V, which is inside
the PCD5002's SDA/SCL limit of VPR + 0.8 V for a typical 2.7V rail.

Reading cannot corrupt anything: writes need a programming enable bit in the
control register that this never sets.
"""

import argparse
import sys
import time

try:
    from smbus2 import SMBus, i2c_msg
except ImportError:
    raise SystemExit("needs smbus2:  pip3 install smbus2")

PCD5002_ADDR = 0x27
EEPROM_24C32_ADDR = 0x50
IDX_CONTROL = 0x00
IDX_EEPROM_PTR = 0x07
IDX_EEPROM_DATA = 0x0A
EEPROM_BYTES = 48

RUN = 6              # unanimous reads required per byte
LINK_SAMPLES = 20    # reads used to judge the link before trusting it


def eeprom_addresses():
    """Columns 6 and 7 do not exist: address = (row << 3) | column."""
    return [(row << 3) | col for row in range(8) for col in range(6)]


class Decoder:
    def __init__(self, bus, pace):
        self.bus = bus
        self.pace = pace

    def select_index(self, index, data=None):
        """Message form (a): S ADDR+W INDEX [DATA] P, its own transaction."""
        payload = [index] if data is None else [index, data]
        self.bus.i2c_rdwr(i2c_msg.write(PCD5002_ADDR, payload))
        time.sleep(self.pace)

    def read_data(self, count=1):
        """Message form (b): S ADDR+R DATA.. P.

        Deliberately a separate i2c_rdwr call rather than being combined with
        the write above, because the datasheet defines a read as its own
        message that draws from whichever index was written last.
        """
        msg = i2c_msg.read(PCD5002_ADDR, count)
        self.bus.i2c_rdwr(msg)
        time.sleep(self.pace)
        return list(msg)

    def read_once(self, addr):
        self.select_index(IDX_EEPROM_PTR, addr)
        self.select_index(IDX_EEPROM_DATA)
        return self.read_data(1)[0]

    def read_address(self, addr):
        """RUN unanimous reads, or nothing.

        No retry budget on purpose. At the corruption rate a stretching-blind
        adapter produces, extra attempts only give a wrong unanimous run more
        chances to appear; simulation put majority voting at 70 wrong answers
        in 400 and a retried run at 113, while a single unanimous window never
        returned wrong data.
        """
        first = self.read_once(addr)
        for _ in range(RUN - 1):
            if self.read_once(addr) != first:
                return None
        return first


def check_control(bus, expect=None):
    """Read the 24C32 as a positive control: it is simple, fast and known good."""
    write = i2c_msg.write(EEPROM_24C32_ADDR, [0x00, 0x00])
    read = i2c_msg.read(EEPROM_24C32_ADDR, 16)
    bus.i2c_rdwr(write, read)
    data = bytes(list(read))

    print("  0x50 first 16 bytes: " + " ".join(f"{b:02X}" for b in data))

    if expect:
        want = open(expect, "rb").read()[:16]
        if data == want:
            print("  matches the reference dump: the link carries data correctly")
            return True
        print("  does NOT match the reference:")
        print("    expected " + " ".join(f"{b:02X}" for b in want))
        return False

    return len(set(data)) > 1


def assess_link(dev, addr=0x10):
    """One address, many reads. On a sound link every read matches."""
    values = [dev.read_once(addr) for _ in range(LINK_SAMPLES)]
    distinct = sorted(set(values))

    print(f"link check at 0x{addr:02X}: {LINK_SAMPLES} reads, "
          f"{len(distinct)} distinct value(s)")
    print("  saw: " + " ".join(f"{v:02X}" for v in values))

    if len(distinct) == 1:
        print("  stable: the link is sound")
        return True

    print("  UNSTABLE. One address must give one answer, so a dump now would")
    print("  be fiction. Check, in order:")
    print("    - that you are on the i2c-gpio bus, not hardware /dev/i2c-1,")
    print("      whose BSC block does not honour clock stretching")
    print("    - a larger i2c_gpio_delay_us in config.txt, try 50")
    print("    - shorter leads, and 2k2 pull-ups to the pager's own VDD")

    if len(distinct) == 2:
        a, b = distinct
        if a == b >> 1 or b == a >> 1:
            print(f"    the two values differ by a one-bit shift "
                  f"({a:02X}/{b:02X}), which is the clock stretching signature")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", help="file to write the 48 byte dump to")
    ap.add_argument("--bus", type=int, default=3,
                    help="i2c bus number; 3 matches the overlay above")
    ap.add_argument("--pace", type=float, default=2.0, metavar="MS",
                    help="pause after each transaction")
    ap.add_argument("--expect", metavar="FILE",
                    help="an earlier 24C32 dump, to verify the link byte for byte")
    ap.add_argument("--scan", action="store_true", help="probe the bus and exit")
    args = ap.parse_args()

    if args.bus == 1:
        print("WARNING: bus 1 is the hardware I2C whose clock stretching is",
              "broken.\n         Use the i2c-gpio overlay; see the header of",
              "this file.\n", file=sys.stderr)

    with SMBus(args.bus) as bus:
        dev = Decoder(bus, args.pace / 1000.0)

        print("== control: the known-good EEPROM at 0x50 ==")
        try:
            if not check_control(bus, args.expect):
                raise SystemExit("the link cannot even read 0x50 correctly; "
                                 "fix that before going near the decoder")
        except OSError as exc:
            raise SystemExit(f"could not read 0x50 ({exc}). Check wiring and "
                             f"that i2cdetect -y {args.bus} shows 0x50 and 0x27")

        print()

        # Table 18: D4 = 0 selects OFF status while DON is low, D1 = 0 keeps
        # EEPROM writes disabled. 8.50 wants the receiver inactive first.
        dev.select_index(IDX_CONTROL, 0x00)
        time.sleep(0.05)

        if args.scan:
            return 0 if assess_link(dev) else 1

        if not assess_link(dev):
            return 1

        print()
        data = bytearray()
        failed = []

        for addr in eeprom_addresses():
            v = dev.read_address(addr)
            if v is None:
                failed.append(addr)
                data.append(0)
            else:
                data.append(v)

        if failed:
            print(f"FAILED: {len(failed)} of {EEPROM_BYTES} bytes did not give "
                  f"{RUN} identical reads:")
            print("  " + " ".join(f"{a:02X}" for a in failed))
            print("The link passed the initial check but is not good enough.")
            return 1

        if not args.out:
            print("no output file given, so not saving")
            return 0

        open(args.out, "wb").write(bytes(data))
        print(f"wrote {len(data)} bytes to {args.out}")
        for r in range(8):
            print(f"  {(r << 3):02X}  "
                  + " ".join(f"{b:02X}" for b in data[r*6:(r+1)*6]))
        print(f"\nnow run:  ./pcd5002.py {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
