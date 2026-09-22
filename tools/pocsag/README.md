# POCSAG on a UV-K5: encoder, hardware FSK modem, receiver, and host tests

Enabled in the firmware build with `ENABLE_POCSAG=1`.

| file | what it does |
| --- | --- |
| `app/pocsag.c` | encodes a page into a packed bit buffer |
| `app/pocsag_rx.c` | streaming decoder, and the receive session that feeds it |
| `driver/bk4819-hwfsk.c` | the BK4819's FSK modem, both directions |
| `app/pocsag_tx.c` | keying and transmit |
| `app/pocsag_ui.c` | the screen, on `F` + `0` |

The whole feature costs **5640 bytes of flash and 556 bytes of RAM**, measured as
the difference between `ENABLE_POCSAG=0` and `=1`. That leaves about 2.3 kB before
the 60 kB limit in `firmware.ld`, so anything added here has to pay for itself.

## The screen

`F` then `0`, which is the FM radio's key, so this is only reachable in a build
with `ENABLE_FMRADIO=0`. Five fields, `UP`/`DOWN` to move between them:

| field | |
| --- | --- |
| `OWN` | the address this radio answers to, or `ALL` to keep every page |
| `FRQ` | the channel, in MHz |
| `RIC` | the address a page is sent to |
| `BPS` | 512, 1200 or 2400 |
| `MSG` | the text, typed Nokia style on the number keys |

Digits type into the selected field, the side keys step it, `*` backspaces, `#`
clears, `MENU` transmits, `EXIT` leaves. The bottom two rows hold the last page
received, and `RX` in the corner becomes `RX*` while a transmission is being
tracked. An empty `MSG` sends a tone-only page, which is all a beeper needs.

On the `OWN` row `#` toggles monitor mode: `ALL` and back to the address that was
in use before it, so there is a way out of monitoring again. An address typed in to
match a real network or another radio is remembered rather than discarded, so the
trip through `ALL` is lossless.

On the `FRQ` row `#` steps through presets rather than clearing, since zero is not
a frequency anything can do: **153.22500** (the Philips 53D pager, read off its
crystal) then **439.98750** (DAPNET, the amateur paging network — live POCSAG 1200
traffic, and so the only way to test a receiver against a transmitter nobody here
built). A frequency typed or stepped by hand is not a preset, so the next `#`
always lands on the first one. Adding more is one line in `gFreqPreset`.

On the `RIC` row `#` steps destination addresses the same way — the two radios
this was developed with, and the Philips 53D pager — **skipping whichever one
matches this radio's own `OWN`**, since paging yourself is never the intention. On
a pair of radios that leaves the other radio and the pager, which is the set worth
cycling. Note those two radio addresses are whatever this particular pair of chip
IDs hashed to, so they mean nothing on anyone else's hardware; `gRicPreset` is the
line to change.

`OWN` defaults to a hash of the MCU's 128 bit chip ID (`SYSCON_CHIP_ID0..3`),
because there is no paging authority to allocate an address and two radios
running this firmware should not answer to the same one. It is constant across
reboots, so the default needs no storage; a value typed over it lasts only until
the radio is switched off.

### Monitoring a channel

Press `#` on `OWN` so it reads `ALL`; `#` again returns to your own address. The address comparison is then skipped and
every page decoded is kept, each shown with the address it was sent to in front of
the text. There is no alert in this mode: a busy channel would beep continuously,
and each alert blocks for the best part of half a second, which would lose
whatever arrived meanwhile.

Zero is a safe sentinel for this, because addressing a pager as 0 would mean an
all-zero address codeword in frame 0 and no real system uses it. One consequence
is worth knowing: an all-zero codeword is also exactly what a dead carrier decodes
as, and monitoring no longer rejects it by address, so a signal that drops
mid-batch can show up as a page from address 0 to 7.

## Transmit: verified on air

A page was captured off air with an RTL-SDR and decoded by multimon-ng, an
independent decoder:

    POCSAG1200: Address: 1578624  Function: 3  Alpha:   HELLO

and a real Philips 53D pager alerts. Measured on the air:

| property            | measured                          |
| ------------------- | --------------------------------- |
| frequency           | 153.225750 MHz, +750 Hz           |
| bit rate            | 1199.5 baud against nominal 1200  |
| preamble            | 576 bits, 98-99% alternation      |
| deviation           | about +/-3.9 kHz vs nominal 4.5   |
| sync and codewords  | decode, BCH valid                 |

So the datasheet's `REG_5D` length encoding of bytes-1 is right, and the chip
defaults for `REG_40` and `REG_70` give a deviation a pager accepts.

## Receive: how it works

POCSAG turns out to look almost exactly like the BK4819's own FSK format: an
alternating `0xAA` preamble, a 4 byte sync word, then NRZ data. So the chip's FSK
receiver is pointed at POCSAG's frame sync codeword `0x7CD215D8` in `REG_5A` and
`REG_5B`, and it does the clock recovery, the sync hunt and the byte framing.
Everything above that - codewords, BCH, batches, addresses, text - is done in
`app/pocsag_rx.c`.

Three things are worth knowing about the shape of it:

- **The RX FIFO is 8 words where the TX one is 128.** It has to be drained while
  the packet is still arriving, and the almost-full interrupt is the only
  indication of how much is waiting, so exactly 4 words are read per interrupt.
- **Batches run back to back with no fresh preamble between them,** so the chip
  only ever reports the first sync word. The data length is therefore set long
  (2040 bytes) and the inter-batch sync words arrive as ordinary data, where the
  decoder uses them to stay in step and to get back in step after a noise burst.
- **Past the end of a transmission the demodulator slices noise into bytes
  forever,** and the chip goes on filling the FIFO until its programmed length
  runs out. So the receiver re-arms as soon as the signal falls 10 dB below where
  it was when the sync word matched, checked after reading out whatever the FIFO
  already held. One two-radio test delivered 352 bytes from a transmission
  carrying 64, and enough of that garbage survived BCH to be decoded as addresses
  that were never sent. The fallbacks behind that are a fruitless hunt lasting
  longer than two batches, and the data stopping altogether.

Two details of that interface the datasheet does not settle, both now measured
rather than assumed:

- **The data arrives inverted.** POCSAG calls the lower frequency a one; the
  chip's slicer calls the higher one a one, so every bit comes out flipped. It is
  a property of the two conventions meeting, not of any transmitter, so it is a
  constant. `REG_0B<7:6>` are documented as reporting whether the sync word
  matched positive or negative, and the polarity was taken from them at first:
  they do not give a usable answer, because the reading that worked is the one
  those bits said was not needed.
- **The FIFO byte order is the same in both directions.** Transmit proved the TX
  FIFO takes each word low byte first, and receive turned out to match, so that
  inference held. It was worth checking: get it wrong and every byte pair arrives
  transposed and nothing decodes.

Both were settled at once rather than one reflash at a time. The radio counted
exact idle codewords for all four readings - bytes as they come or transposed,
data as it comes or inverted - against a single real transmission. Only the
inverted reading found any: nine, against none for the other three. An idle is
`0x7A89C197` or it is not, and nothing but a real batch produces it, which is what
makes that test decisive rather than suggestive.

Also worth knowing: **the preamble type has to be stated, not inferred.**
`REG_58<5:4>`=00 means "0xAA or 0x55 decided by the MSB of sync byte 0", and
POCSAG's sync byte 0 is `0x7C`, whose MSB is zero. That setting appears to leave
the chip expecting the `0x55` phase, a byte boundary one bit out from the real
one, so the sync word never matches however strong the signal. Setting the
preamble type to `0xAA` explicitly is what made sync detection start working.

### Verified radio to radio

A page typed on one UV-K5 arrives on another addressed to the receiving radio's
`OWN`, so both directions of the feature work on real hardware.

### What the counters could not say

Three readings in a row were misread as progress during that bring-up, and all
three had the same cause: **sixteen valid codewords means nothing.** Sixteen is
the ceiling for any stream, because the decoder gives up framing at slot 16 and
hunts from there. And both `0x00000000` and `0xFFFFFFFF` are valid BCH
codewords - one looking like an address, the other like a message - so a
demodulator slicing receiver noise into long runs produces sixteen valid
codewords and a handful of plausible addresses, from nothing at all.

The idle count is the honest test, for the reason above. `tools/pocsag/` has no
copy of the simulation that established this; it was a throwaway, but the table it
produced is worth recording:

| stream fed to the decoder after a sync | codewords | addresses | failed BCH |
| --- | --- | --- | --- |
| constant 0 | 16 | 16 | 0 |
| constant 1 | 16 | 0 | 0 |
| long runs, mean 20 bits | 4 | 1 | 12 |
| independent random bits | 0 | 0 | 16 |
| a real batch | 16 | 1 | 0 |

## What the host tests cover

    make
    MULTIMON=/path/to/multimon-ng/build/multimon-ng make test

| suite | what it proves |
| --- | --- |
| `roundtrip.sh` | 97 checks: the encoder's output decodes in multimon-ng, across every frame position, bit rate, message length, function code and both polarities |
| `hwfsk_test` | the bit stream the modem would put on air is the encoder's, bit for bit, reconstructed from the register writes |
| `rx_test` | the decoder reads back what the encoder wrote, plus the cases that only matter on air |
| `fixture_test.sh` | the PCD5002 decoder against a real EEPROM dump |

`rx_test` drives one half of the format with the other: it encodes a page, hands
the decoder the stream from the point where the modem would hand over, and checks
what comes out. Since `roundtrip.sh` already proves that stream is real POCSAG,
agreement means the decoder reads real POCSAG. Beyond that it covers the frame
number being part of the address, single bit error correction on every codeword
in turn, recovery after a wrecked sync word, inverted data, noise decoding as
nothing, a page ended by another page to the same address, and both halves of the
idle-count claim: a real batch gives at least four idles, constant runs give
none.

### What the host tests could not catch

The last transmit defect was the FIFO byte order: each 16 bit word goes out low
byte first, and packing the earlier byte into the high half swapped every pair on
air. `hwfsk_test` passed throughout because it reconstructed the on-air stream
with the same assumption as the driver. **A synthetic test that shares the code's
misreading proves nothing.** That is why the fixture from real hardware and the
off-air capture both matter, and why the receive conventions were settled against
a real signal rather than against a test. With the old packing restored,
`hwfsk_test` now fails at byte 72, exactly where the sync codeword begins:
everything before it is `0xAA` preamble, and swapping identical bytes changes
nothing.

### Why there is no bit-banged path

An earlier version also had a backend that shifted the synthesiser by rewriting
the frequency register once per bit. It was removed because it cannot work:
writing those registers during transmission does not change the output
frequency. Measured +/-70 Hz where +/-4500 was wanted, both with `REG_38` alone
and with both halves written, i.e. a clean unmodulated carrier. The chip appears
to latch them only on a `REG_30` re-trigger, which is what the firmware's own
spectrum analyser does after retuning, and doing that per bit would mean four
register writes plus a VCO recalibration inside every bit period.

That is worth recording because it is not obvious from the datasheet, which says
only that "in TX mode, FM modulation is realized in the RF frequency
synthesizer".

### Still unverified

512 baud. `REG_58<15:13>`=000 is documented as "FSK1.2K and FSK2.4K Tx" and
`REG_72` sets the rate, but nothing says 512 works, and this pager is 1200 so it
was never needed.

## Building

multimon-ng is not in homebrew. Build it from source:

    git clone --depth 1 https://github.com/EliasOenal/multimon-ng.git
    cd multimon-ng && mkdir build && cd build && cmake .. && make

The firmware's Makefile uses `-Oz` and `[[fallthrough]]`, which need GCC 12 or
newer; on an older `arm-none-eabi-gcc` the whole repo fails to build, with or
without this feature. On this machine that means homebrew's `arm-none-eabi-gcc`
16.2.0, which ships no libc, borrowing newlib from the old `gcc-arm-none-eabi`
9.3.1 install:

    G16=$(brew --prefix)/Cellar/arm-none-eabi-gcc/16.2.0/bin/arm-none-eabi-gcc
    SR=$(brew --prefix)/Cellar/gcc-arm-none-eabi/20200630/arm-none-eabi
    make CC="$G16 -isystem $SR/include" AS="$G16" \
         LD="$G16 -B$SR/lib/thumb/v6-m/nofp -L$SR/lib/thumb/v6-m/nofp" \
         ENABLE_POCSAG=1

`make ENABLE_CLANG=1 ARM_SYSROOT=$SR ENABLE_POCSAG=1` also builds and warns about
more, but produces enough extra code that it now overflows the flash region by
about 2 kB. It is still worth running for the warnings.

## Generating a single file

    ./pocsag_wav -r 1234567 -b 1200 -t alpha -f 3 -d -o page.wav "HELLO"

`-d` dumps the codewords. `-i` inverts the on-air polarity, `-p` low-passes the
waveform instead of emitting raw NRZ. `./pocsag_wav -h` lists the rest.

The audio is what an FM discriminator would produce: a binary `0` is the positive
frequency deviation and so a positive level. If a real off-air recording decodes
only with `-i`, the receive chain is inverting.

## Notes on the format

- The low 3 bits of the RIC are not transmitted. They select which of the 8
  frames in the batch carries the address codeword, and both the encoder and the
  decoder derive them from that position.
- A message is delimited by the next address codeword or by an idle, so the
  encoder always emits at least one trailing idle. Without it a message that
  exactly fills a batch is silently dropped by the receiver.
- Partial final message codewords are padded with EOT (alpha) or the numeric
  space code, not zero bits, which would decode as trailing NULs. The decoder
  stops at the EOT and trims trailing blanks.
- The idle codeword decodes as a real address, so that block is not available to
  any pager. `POCSAG_IDLE_RIC` is it, and the `OWN` default steps around it.
