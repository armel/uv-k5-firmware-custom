# UV-K5 firmware emulator (Renode)

Boots the **actual compiled `.elf`** for this firmware (not a recompiled/ported
build) against simulated hardware, so you can test menu navigation, screen
rendering, and general firmware logic in seconds instead of a build-flash-observe
cycle on real hardware. Background and design rationale:
`.claude/plans` history in this repo / ask in-session for the full writeup — the
short version: this firmware is almost entirely polling-driven (the only real
interrupt anywhere is a coarse 10ms `SysTick`), which means an emulator doesn't
need cycle-accurate timing or real interrupt modeling, just a CPU core, a memory
map, and a handful of peripheral models that are mostly simple register
bookkeeping.

**Status: Phase 1 verified working end-to-end**, including finding and fixing
two real bugs using the emulator itself.

1. First test run booted `f4hwn.message` through `BOARD_Init()` and drew a
   real welcome screen into `gFrameBuffer`, but then appeared stuck:
   `gStatusLine` stayed all-zero indefinitely. Using the emulator to pause and
   inspect memory directly (rather than guessing), this traced to a genuine
   bug in `gpioa_bitbang.py`'s keyboard model: `driver/keyboard.c`'s row-0
   (SIDE1/SIDE2) scan mask is `0xffff`, which in C only clears bits 16-31 of a
   32-bit register and leaves the actual row-select bits (10-13) untouched —
   SIDE1 and SIDE2 are wired straight to their columns with no row-select
   dependency at all. The model had this backwards (treating "no row bits
   low" as "no row selected, leave columns floating" rather than "this is row
   0"), which read as a phantom SIDE1 press from the very first GPIOA access
   — exactly enough to make `main.c`'s boot-splash wait loop (`while
   (boot_counter_10ms > 0) if (KEYBOARD_Poll() != KEY_INVALID)
   boot_counter_10ms = 0;`) exit immediately on every single boot, before
   ever reaching the real main loop. Fixed in `gpioa_bitbang.py`.

2. After that fix, `display_viewer.py` still appeared to show only a garbled,
   static "screen" that never changed no matter how long the emulator ran —
   even though PC sampling proved the CPU was genuinely alive and cycling
   normally through the real main loop (`Main` → `HandlePowerSave` →
   `SCANNER_IsScanning` → `CheckForIncoming`, exactly what `APP_Update()`
   calls every iteration), not stuck anywhere. The actual bug was in the
   viewer, not the firmware or emulator: `gStatusLine`/`gFrameBuffer` are the
   ST7565's native **page/column-major** LCD memory (each byte is one
   128-wide column within an 8-pixel-tall "page"; bit 0 = that page's top
   row), but `draw_frame()` had been copied from `k5viewer/k5viewer.py` and
   decoded them as a naive row-major bitstream instead. That's correct for
   `k5viewer.py` itself, but only because it's fed the firmware's UART
   screenshot protocol (`screenshot.c`'s `getScreenShot()`), which explicitly
   transposes page/column-major memory into row-major before sending it over
   the wire. Reading RAM directly (this project's whole point, to avoid
   needing to model/decode that protocol) skips that transpose, so every
   frame rendered as unreadable noise despite the underlying data being
   completely valid and actively updating. Fixed by decoding the native
   page-major layout directly in `draw_frame()`. Reverified: the viewer now
   renders a fully legible, correct live VFO screen (`PS DWR CL 00%`,
   frequency, mode, squelch, etc.), confirming the emulator was working
   correctly the entire time — this was purely a host-side rendering bug.

## What's simulated vs. stubbed

| Peripheral | Treatment |
|---|---|
| CPU | Real `cortex-m0` core (Renode `CPU.CortexM`), matching this firmware's `-mcpu=cortex-m0` build flag exactly |
| FLASH / RAM | Real memory, sized from `firmware.ld` (60K / 16K) |
| SysTick | Real, via Renode's NVIC (`systickFrequency: 48000000`) — the only interrupt this firmware actually uses. Not just an assumption: `driver/spi.c`/`driver/adc.c` do call `NVIC_EnableIRQ` for SPI0/SPI1/SARADC, but only `if (IE != 0)`, and every call site that actually configures those peripherals (`SPI0_Init`, `board.c`'s ADC setup) explicitly sets every interrupt-enable field to 0/disabled — so those `NVIC_EnableIRQ` calls are live code paths that just never execute with this firmware's own config values. |
| SYSCON, PORTCON, GPIOB, UART1 | Trivial scratch registers — store writes, echo on read, no side effects (`peripherals/stub_register_bank.py`) |
| SPI0 | Scratch registers, except `FIFOST`'s TX-FIFO-full bit always reads "not full" (an *unbounded* poll in `driver/st7565.c`'s `ST7565_Init` hangs boot forever otherwise) — see `peripherals/spi0_model.py` |
| SARADC | Scratch registers, except every channel's EOC status bit always reads "conversion complete" (another *unbounded* poll, in `board.c`'s `BOARD_ADC_GetBatteryInfo`) — see `peripherals/saradc_model.py` |
| GPIOA | **Real logic**: reconstructs the bit-banged I2C/EEPROM protocol (`driver/i2c.c`/`driver/eeprom.c`) *and* the keyboard row/column scan (`driver/keyboard.c`) from the GPIO master's perspective — see `peripherals/gpioa_bitbang.py` |
| GPIOC | **Real logic**: reconstructs the bit-banged BK4819 register read/write protocol (`driver/bk4819.c`) as a shallow register-value bookkeeping stub (no RF/DTMF/FSK behavior — that's Phase 2), plus a controllable PTT input (its own socket, same pattern as the keyboard — see `host_tools/keyboard_bridge.py`'s PTT button) — see `peripherals/gpioc_bitbang.py` |
| Everything else (IIC0/IIC1, TIMER, RTC, watchdogs, CMP, PWM_BASE, DMA, CRC, UART0/2, flash controller, PWM_PLUS0, SPI1, AES, PMU) | Tagged as unimplemented in `sysbus: init:` — never touched by this firmware (confirmed by direct source inspection), so real modeling would be wasted effort |

Display output is **not** rendered by decoding SPI0 traffic — `host_tools/display_viewer.py`
reads `gFrameBuffer`/`gStatusLine` directly out of emulated RAM (resolved via
`nm` on the `.elf`), since they're always-present globals regardless of build
flags, unlike the optional UART screenshot feature.

## Setup

```
brew install renode/tap/renode   # if not already installed
pip3 install pygame
```

## Running

### One command (recommended)

```
emulator/scripts/launch_all.sh path/to/firmware.elf
```

Starts Renode, `display_viewer.py`, and `keyboard_bridge.py` together, waits
for the monitor port to come up before starting the viewer, and cleans up all
three on Ctrl+C (including Renode's actual `dotnet Renode.dll` process, which
is a child of Renode's own wrapper script rather than something `exec`'d into
it -- killing just the top-level PID would otherwise leave it running
orphaned). `keyboard_bridge.py` opens a **visual, clickable keypad** laid out
exactly like `driver/keyboard.c`'s row/column table (SIDE1/SIDE2, MENU/UP/
DOWN/EXIT, the numeric pad, STAR, F) — click a button to press it, or use the
real-keyboard shortcuts shown at the bottom of that window (arrows, Enter=MENU,
Esc=EXIT, `*`=STAR, `f`=F, `[`/`]`=SIDE1/SIDE2, 0-9). Held buttons highlight
green, whether pressed by mouse or by the matching keyboard shortcut. Labels
are drawn with a small built-in bitmap font rather than `pygame.font`, so the
keypad renders identically even on pygame builds missing SDL_ttf.

There's also a **PTT button** (or the Space bar) below the keypad grid, on
its own red bar — PTT is a separate GPIO port from the keyboard matrix
(GPIOC, not GPIOA), so it's wired through its own socket
(`emulator/peripherals/gpioc_bitbang.py`'s PTT bridge, default port 9813,
override with `UVK5_PTT_PORT` / `--ptt-port`) rather than reusing the
keyboard one. This only exercises `app/app.c`'s TX-state transitions
(`gPttIsPressed`, `gCurrentFunction`, screen changes) — there's no real
RF/audio behind it, that's still Phase 2 scope. Note it can immediately
trigger a `BAT LOW` warning that blocks the transmit: that's the firmware
correctly reacting to `saradc_model.py`'s fixed placeholder battery reading
(see Known limitations below), not a PTT wiring bug.

**Every press is held for at least 250ms in real time, even if you tap
faster than that.** `app/app.c`'s `CheckKeys()` only treats a key as
genuinely pressed once `KEYBOARD_Poll()` reads it as held across 2
*consecutive* polls, and this emulator runs well over 10x slower than real
time (see the regression test note below), so a fast real-world tap can
easily release before the firmware completes even one poll cycle — the
press just gets silently dropped by the debounce logic, which looks like
"some keypresses don't reach the radio." 250ms was picked empirically: short
taps (~20ms) reliably got dropped without this, while holds long enough to
cross `app/app.c`'s `key_repeat_delay_10ms` (400ms) started firing extra
repeat presses on top of the first (e.g. jumping several menu items instead
of one) — 250ms sits in the middle with margin on both sides.

Options: `--eeprom PATH`, `--monitor-port N`, `--keyboard-port N`,
`--no-keyboard` (view-only, skip key injection), `--test` (run the regression
test against this `.elf` instead of the interactive session — see below).

### Manual (three terminals)

Equivalent to the above, run by hand — useful if you want to restart just one
piece without tearing down the others:

```
emulator/scripts/run.sh path/to/firmware.elf
```

This starts Renode with a monitor on `127.0.0.1:8888` (override with
`UVK5_MONITOR_PORT`) and a keyboard socket on `127.0.0.1:9812` (override with
`UVK5_KEYBOARD_PORT`). In separate terminals:

```
emulator/host_tools/display_viewer.py --elf path/to/firmware.elf
emulator/host_tools/keyboard_bridge.py
```

Note: the compiled `.elf` in this repo is the extensionless file next to each
`.bin` (e.g. `f4hwn.message`, not `f4hwn.message.bin`) — that's the one with
debug symbols `nm`/the display viewer need.

### EEPROM persistence

By default, settings/channels persist to `emulator/.generated/eeprom.bin`
across runs. Pass a second argument to `run.sh` to use a different file, or
delete that file to reset to a blank (all-`0xFF`) EEPROM.

### Manual invocation / poking around

You can always talk to the running emulator directly over its monitor port:

```
nc 127.0.0.1 8888
python "print(','.join(str(b) for b in self.Machine.SystemBus.ReadBytes(0x20001376, 64)))"
```

`gFrameBuffer`/`gStatusLine`/`gScreenToDisplay`/etc.'s addresses vary per build
— resolve them with `nm path/to/firmware.elf | grep gFrameBuffer`.

## Regression test

```
cd emulator/tests
FB_ADDR=$(nm ../../f4hwn.custom | awk '$3=="gFrameBuffer"{print "0x"$1}')
renode-test smoke_boot.robot --variable FIRMWARE_ELF:$(pwd)/../../f4hwn.custom --variable FRAMEBUFFER_ADDR:$FB_ADDR
```

Or via the all-in-one launcher (does the same `nm` lookup for you):

```
emulator/scripts/launch_all.sh path/to/firmware.elf --test
```

Asserts `gFrameBuffer` has at least one nonzero byte after 5 emulated seconds —
a cheap guard against reintroducing one of the boot hangs this Phase 1 build
had to avoid (the two unbounded polls, SPI0 FIFOST/SARADC EOC, plus the
phantom-keypress keyboard bug above). Note "5 emulated seconds" can take well
over a minute of *real* wall-clock time — every bit-banged GPIO access round-
trips through an interpreted Python script, and this firmware does a lot of
them (a single `KEYBOARD_Poll()` call alone is a dozen-plus GPIOA accesses) —
that's expected, not a hang in the test runner itself.

## Known limitations / open items

- **No RF/DTMF/FSK behavior** — BK4819 is a register-bookkeeping stub only
  (explicit Phase 2 scope).
- **No multi-radio bridging** — can't yet test any radio-to-radio feature
  (Message, AirCopy) since that needs Phase 2's BK4819 model to exist first.
- **No audio path** — display + input + boot only, no mic/speaker/companding.
- **Battery/ADC readings are a fixed placeholder** (`saradc_model.py`'s
  `DEFAULT_ADC_VALUE`) — tune it if the emulated UI shows an implausible
  percentage once you're looking at real screen output.
- **I2C model is narrow, not general-purpose** — it only implements this
  firmware's exact fixed EEPROM access pattern (`0xA0`/`0xA1` addressing,
  2-byte memory address), not general I2C semantics (clock stretching,
  multi-master, arbitration).
- **Deviation from the original plan's file layout**: `bk4819_model.py` and
  `eeprom_model.py` were folded directly into `gpioc_bitbang.py`/
  `gpioa_bitbang.py` respectively, rather than kept as separate imported
  modules — simpler and avoids any uncertainty about IronPython's module
  search path for `Python.PythonPeripheral` scripts loaded via `filename:`.
