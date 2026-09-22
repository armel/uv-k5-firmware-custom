#!/usr/bin/env python3
"""Decode a Philips PCD5002 paging decoder EEPROM dump.

    ./pcd5002.py eeprom.bin

Prints the configured capcodes (RICs), the POCSAG bit rate, the on-air data
polarity and the rest of the special programmed function bits.

Reading the EEPROM out of a live pager
--------------------------------------
The PCD5002 is an I2C slave at 7-bit address 0x27 (write 0x4E, read 0x4F),
SDA on pin 9 and SCL on pin 10. Its EEPROM is not directly addressable; it is
reached through two index registers:

    07H  EEPROM address pointer   (bits 5:3 = row 0-7, bits 2:0 = column 0-5)
    0AH  EEPROM data I/O          (each access auto-increments the pointer)

So to dump all 48 bytes:

    write [0x07, 0x00]        select row 0, column 0
    write [0x0A]              point the index at the data register
    read  48 bytes            pointer auto-increments across the matrix

The pointer walks columns 0..5 then steps to the next row, which is why the
datasheet addresses are non-contiguous: address = (row << 3) | column, so
0x00-0x05, 0x08-0x0D, 0x10-0x15 and so on. A 48 byte sequential read is
therefore row-major and this script expects that; pass a 64 byte image and it
will index by datasheet address instead.

Important: per datasheet 8.50 the EEPROM must not be accessed while the
receiver is active. Put the decoder in the OFF state first. Reading is
otherwise safe; writing needs the programming enable bit, so a read-only
dump cannot corrupt the pager.
"""

import argparse
import sys

BIT_RATES = {0: 512, 1: 1024, 2: 1200, 3: 2400}

# Table 31: identifier type from byte 3 bits D2 and D0
ID_TYPES = {
    (0, 0): "UPSW (user programmable sync word)",
    (0, 1): "CDD sync word (continuous data decoding)",
    (1, 0): "RIC (normal user address)",
    (1, 1): "batch zero identifier",
}


class Eeprom:
    """Addressed the way the datasheet numbers it, whatever the dump layout."""

    def __init__(self, data):
        if len(data) == 48:
            # row-major sequential read: 6 columns per row
            self.get = lambda a: data[((a >> 3) * 6) + (a & 7)]
        elif len(data) >= 64:
            self.get = lambda a: data[a]
        else:
            raise SystemExit(f"expected a 48 or 64 byte dump, got {len(data)}")

        # columns 6 and 7 do not exist in the matrix
        for a in range(0x40):
            if (a & 7) > 5:
                continue
            self.get(a)


def decode_identifiers(e):
    out = []
    for n in range(6):
        b1, b2, b3 = e.get(0x10 + n), e.get(0x18 + n), e.get(0x20 + n)

        # Table 30: byte 1 = codeword bits 2..9, byte 2 = bits 10..17,
        # byte 3 D7,D6 = bits 18,19. Those 18 bits are the transmitted
        # address field, i.e. the top 18 bits of the 21-bit RIC.
        upper18 = (b1 << 10) | (b2 << 2) | ((b3 >> 6) & 3)

        # D5,D4,D3 = FR3,FR2,FR1, the 3 least significant RIC bits (note 3)
        frame = (b3 >> 3) & 7

        # An erased slot reads FF FF FF, whose flag bits happen to decode as
        # "enabled RIC" with the maximum address. Treat it as unprogrammed
        # rather than offering 2097151 as something to transmit to.
        blank = (b1, b2, b3) in ((0xFF, 0xFF, 0xFF), (0x00, 0x00, 0x00))

        out.append({
            "n": n + 1,
            "raw": (b1, b2, b3),
            "blank": blank,
            "ric": (upper18 << 3) | frame,
            "frame": frame,
            "type": ID_TYPES[((b3 >> 2) & 1, b3 & 1)],
            "is_ric": bool((b3 >> 2) & 1),
            "enabled": bool((b3 >> 1) & 1) and not blank,
        })
    return out


def sanity(e, ids):
    """Contradictions that mean the dump is bad rather than the pager odd.

    A partially failed I2C read looks like a few real bytes followed by 0xFF,
    which is easy to mistake for a valid configuration. These checks exist so a
    bad dump announces itself instead of yielding a confident wrong capcode.
    """
    problems = []
    spf0, spf1 = e.get(0x00), e.get(0x01)

    rate_field = (spf1 >> 4) & 3
    if rate_field == 1 and not spf0 & 1:
        problems.append(
            "bit rate field is 01 = 1024 bits/s, which the datasheet says is "
            "not used in POCSAG, yet POCSAG is selected")

    batch = (spf0 >> 2) & 0xF
    limit = 14 if spf0 & 2 else 4
    if batch > limit:
        problems.append(
            f"batch number {batch} is outside the valid range 0 to {limit} "
            f"for a {15 if spf0 & 2 else 5} batch cycle")

    id_bytes = [e.get(a + n) for a in (0x10, 0x18, 0x20) for n in range(6)]
    if all(b == 0xFF for b in id_bytes):
        problems.append(
            "every identifier byte is 0xFF, so no capcode is programmed at all")
    elif all(b == 0x00 for b in id_bytes):
        problems.append("every identifier byte is 0x00")

    used = [i for i in ids if not i["blank"]]
    if len(used) > 1 and len({i["ric"] for i in used}) == 1:
        problems.append("all six identifiers are identical, which real "
                        "configurations are not")

    blank = sum(1 for a in range(0x40) if (a & 7) <= 5 and e.get(a) == 0xFF)
    if blank >= 40:
        problems.append(f"{blank} of 48 bytes are 0xFF; only 20 are legitimately "
                        "unused, so the read probably failed partway")

    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    args = ap.parse_args()

    e = Eeprom(open(args.dump, "rb").read())

    spf0, spf1 = e.get(0x00), e.get(0x01)

    print("== decoder configuration ==")
    print(f"  protocol        {'APOC-1' if spf0 & 1 else 'POCSAG'}")
    rate = BIT_RATES[(spf1 >> 4) & 3]
    print(f"  bit rate        {rate} bits/s")
    print(f"  data inversion  {'ENABLED' if spf0 & 0x80 else 'disabled'}"
          f"   <- on-air polarity")
    print(f"  cycle length    {15 if spf0 & 2 else 5} batches")
    print(f"  batch number    {(spf0 >> 2) & 0xF}")
    print(f"  continuous data {'enabled' if spf0 & 0x40 else 'disabled'}")
    print(f"  synthesizer     {'enabled' if spf1 & 0x40 else 'disabled'}"
          f" (data at 08H-0DH)")
    print(f"  raw SPF         00H={spf0:02X} 01H={spf1:02X} "
          f"02H={e.get(0x02):02X} 03H={e.get(0x03):02X}")

    if spf1 & 0x40:
        blocks = [
            (e.get(0x08) << 16) | (e.get(0x09) << 8) | e.get(0x0A),
            (e.get(0x0B) << 16) | (e.get(0x0C) << 8) | e.get(0x0D),
        ]
        print(f"  synth blocks    {blocks[0]:06X} {blocks[1]:06X}")

    ids = decode_identifiers(e)

    problems = sanity(e, ids)
    if problems:
        print("\n== THIS DUMP LOOKS WRONG ==")
        for p in problems:
            print(f"  - {p}")
        print("\n  A part-failed I2C read gives a few real bytes then 0xFF.")
        print("  Re-read with:  py read_pcd5002_ch341.py out.bin --single")
        print("  Anything below is decoded from suspect data; do not transmit to it.")

    print("\n== identifiers ==")
    live = []
    for i in ids:
        mark = "*" if i["enabled"] else " "
        b1, b2, b3 = i["raw"]
        if i["blank"]:
            print(f"  {i['n']}  {b1:02X} {b2:02X} {b3:02X}  "
                  f"{'-':>14}  unprogrammed")
            continue
        print(f" {mark}{i['n']}  {b1:02X} {b2:02X} {b3:02X}  "
              f"{'RIC ' + str(i['ric']):>14}  frame {i['frame']}  "
              f"{'enabled ' if i['enabled'] else 'disabled'}  {i['type']}")
        if i["enabled"] and i["is_ric"]:
            live.append(i["ric"])

    print("\n== capcodes to transmit to ==")
    if problems:
        print("  withheld: the dump above failed its consistency checks")
    elif live:
        for r in live:
            print(f"  {r}")
        print(f"\n  try these at {rate} baud"
              + (", with inverted polarity" if spf0 & 0x80 else ""))
    else:
        print("  none enabled; the pager was probably deprogrammed")


if __name__ == "__main__":
    sys.exit(main())
