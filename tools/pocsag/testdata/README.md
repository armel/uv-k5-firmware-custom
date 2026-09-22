# Test fixtures

`pcd5002_philips53d.bin` — 48 bytes read from the PCD5002 in a dismantled
Philips 53D pager, over software I2C on a Raspberry Pi. Read with 20 identical
reads of one address to prove the link, then six unanimous reads per byte.

It is the only fixture here taken from real hardware, which is what makes it
worth keeping: everything else `pcd5002.py` is checked against is synthetic, so
this is the one case that proves the identifier bit packing against a
configuration written by a real network rather than by the same arithmetic the
decoder uses.

It self-validates. Byte 3 of each identifier stores the frame number in D5:D3,
a field separate from the 18 transmitted address bits, and on all five
programmed slots the two agree with RIC mod 8.

Expected decode:

    POCSAG, 1200 bits/s, data inversion disabled, 5 batch cycle, batch 0
    identifier 1   RIC 1578624  frame 0  enabled
    identifier 2   RIC  683607  frame 7  enabled
    identifier 3   RIC  683615  frame 7  enabled
    identifier 4   RIC 1000000  frame 0  disabled
    identifier 5   RIC 1000000  frame 0  disabled
    identifier 6   unprogrammed
