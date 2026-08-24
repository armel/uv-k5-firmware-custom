# UV-K5 Custom Firmware (F4HWN fork) — User Manual

This manual covers the **full/custom build** of this firmware — i.e. every optional
feature turned on. Some of these ship **off by default** in this repo's `Makefile`
even though the code exists (noted individually below); if your radio doesn't show
something described here, it may be running a build/edition without that flag
enabled. See the [README](../README.md#building) for how to build with a given set
of features.

For the radio-to-radio text messaging feature specifically, see the dedicated
[Message user guide](message-feature-user-guide.md) — it's only summarized here.

## Table of Contents

1. [Basic navigation](#basic-navigation)
2. [Status bar icons](#status-bar-icons)
3. [Keyboard shortcuts](#keyboard-shortcuts)
4. [The Menu](#the-menu)
5. [Standalone modes and screens](#standalone-modes-and-screens)
6. [Firmware editions](#firmware-editions)

## Basic navigation

- **UP / DOWN** — steps the VFO frequency (by the configured step size) or steps to
  the next/previous memory channel. While scanning, jumps to the next/previous found
  channel/frequency.
- **Digit keys 0-9** — on a frequency VFO, types a new frequency directly (rounds to
  the nearest valid step); on a memory channel, types a channel number (auto-jumps
  once enough digits are entered); on a NOAA channel, types a NOAA channel number.
- **`*` (STAR), short tap** — opens a DTMF quick-dial entry box on the main screen
  (type a code, then press PTT to transmit it).
- **`*` (STAR), held down** — starts or stops scanning.
- **MENU, short press** — opens the main menu (or, if scanning, stops the scan).
- **EXIT, short press** — backspaces one character of an entry in progress; stops a
  scan and restores your original frequency/channel; exits FM radio mode.
- **F, short tap** — arms the "F" modifier for your next keypress (see
  [Keyboard shortcuts](#keyboard-shortcuts)); shown as an "F" icon in the status bar.
- **F, held down** — toggles the keypad lock.

## Status bar icons

Reading left to right across the top row:

| Icon | Meaning |
|---|---|
| NOAA | NOAA weather-channel background monitoring is active |
| Power-save (replaces NOAA icon) | Radio is in its power-saving sleep/duty-cycle state |
| Solid filled block | Radio has been remotely **killed** via a DTMF kill code |
| Scan-list number (0/1/2/3/[1,2,3]/ALL) | A channel-mode scan is running against this scan list |
| "S" | A frequency-mode scan is running (no scan list applies) |
| Voice-prompt icon | Spoken voice announcements are on |
| RX/TX countdown timer | Live MM:SS timer while transmitting/receiving (if **SetTmr** is on) |
| "RO" | RescueOps menu-lock mode is engaged |
| "DWR" | Dual Watch + Respond, actively watching |
| Hold indicator | Dual watch configured but temporarily paused |
| "XB" | Cross-band repeat/reply is active |
| "MO" | Normal single-VFO mode (no dual watch/cross-band) |
| VOX icon | Voice-activated transmit is on |
| PTT-mode icon | Which PTT style is active: one-push, or classic press-and-hold |
| Padlock | Keypad is locked |
| "F" | F key armed, waiting for the next key |
| Mute icon | Audio output manually muted |
| Backlight/bulb icon | Display backlight is lit |
| USB-C icon | Charging via USB-C |
| Battery text | Voltage or percentage (if **BatTxt** is set to show one) |
| Battery gauge | Bar graph of charge level (far right); blinks empty when critically low |

## Keyboard shortcuts

### F-key combinations (main screen)

Press `F`, release it (it arms, shown by the "F" icon), then press the next key.

| Combo | Effect |
|---|---|
| F + 0 | Toggle FM broadcast radio |
| F + 1 | Frequency VFO: next band. Memory channel: copy it into the VFO and switch to VFO mode |
| F + 2 | Swap VFO A / VFO B |
| F + 3 | Toggle VFO ↔ memory-channel mode |
| F + 4 | Start a blind frequency scan (opens the Scanner screen) |
| F + 5 | Spectrum Analyzer, **or** jump to/from NOAA — whichever one is compiled in (see [Standalone modes](#standalone-modes-and-screens)) |
| F + 6 | Cycle TX power level |
| F + 7 | Toggle VOX |
| F + 8 | Toggle RX/TX frequency reverse |
| F + 9 | Jump to the "1 Call" priority channel |
| F + UP / F + DOWN | Adjust squelch level live |
| F + SIDE1 | Increase frequency step |
| F + SIDE2 | Decrease frequency step |
| F + `*` | Start a CTCSS/DCS tone scan on the current frequency |

Holding a digit key alone (without pressing F first) replicates its F-combo
instantly — e.g. holding `6` cycles power just like `F+6`.

### Long-press behaviors

| Key(s) | Screen | Effect |
|---|---|---|
| `F` | Main screen | Toggle keypad lock |
| `5` (held, no F) | Main screen | Cycle the current channel's scan-list membership, or mark a custom scan range |
| `*` | Main screen | Start/stop scanning; cycles scan list if already scanning |
| `MENU` | Scanning, paused on a signal | Temporarily exclude this channel/frequency from the current scan pass |
| `MENU` | Not scanning | Clear digits being typed, or run the assigned "M Long" action |
| `EXIT` | Mid entry | Cancel the entry in progress |
| SIDE1/SIDE2 | Main screen | Run whichever action is assigned to F1Long/F2Long |

### Assignable side-button actions

Configured via the **F1Shrt / F1Long / F2Shrt / F2Long / M Long** menu items. Each
can be set to any of:

`None`, Flashlight, Power (cycle), Monitor, Scan, Keylock, VFO A/B, VFO/MEM, Mode
(FM/AM/USB), VOX, Alarm, FM Radio, 1750Hz, REGA Alarm/Test (Switzerland-specific),
BLMIN temp-off, RX Mode, Main Only, PTT (mode toggle), Wide/Narrow, Mute, Power High
(RescueOps), Remove Offset (RescueOps), Msg (opens the Message screen).

Side buttons work even while the keypad is locked. Factory defaults: SIDE1-short =
Monitor, SIDE2-short = Scan, everything else = None.

### PTT modes

- **Classic** (default): press and hold to transmit; release to stop.
- **OnePush** (toggle via the PTT action, or set in **SetPTT**): tap once to start
  transmitting and let go immediately, tap again to stop — for hands-busy use.
- While transmitting, digit keys 0-9 send live DTMF tones over the air instead of
  doing anything to the VFO.
- **SetLck** controls whether the keypad lock also blocks PTT itself ("Keys+PTT") or
  leaves PTT working while locked ("Keys" only).

## The Menu

Press `MENU`, then `UP`/`DOWN` to scroll, `MENU` again to open a setting, adjust with
`UP`/`DOWN` or digit keys, `MENU` to confirm or `EXIT` to cancel.

### Per-channel/VFO settings

| Menu | What it sets |
|---|---|
| **Step** | Tuning step size (0.01 kHz–500 kHz) |
| **Power** | TX power for this channel: User, Low1–5, Mid, High |
| **RxDCS** / **RxCTCS** | DCS code / CTCSS tone required to open squelch |
| **TxDCS** / **TxCTCS** | DCS code / CTCSS tone transmitted (for repeater access) |
| **TxODir** | TX offset direction: Off / + / − |
| **TxOffs** | TX offset amount |
| **W/N** | Bandwidth: Wide / Narrow (Narrow itself redefined by **SetNFM**, see below) |
| **BusyCL** | Busy-channel lockout — block TX while the channel is occupied |
| **Compnd** | Audio compander: Off / TX / RX / TX+RX |
| **Mode** | Demodulation: FM / AM / USB |
| **TXLock** | Override to permit TX on a frequency the band-plan (F Lock) would otherwise block |
| **ScAdd1/2/3** | Add/remove this channel from scan lists 1, 2, 3 |
| **ChSave** / **ChDele** / **ChName** | Save current VFO into a channel slot / delete a channel / rename a channel |

### Scan settings

| Menu | What it sets |
|---|---|
| **SList** | Default scan list used when scanning starts: List 0 (none), 1, 2, 3, [1,2,3], or All |
| **SList1/2/3** | Browse (view-only) which channels belong to that list |
| **ScnRev** | What happens when a scan finds a signal: Stop / Carrier-hold (250ms–20s) / Timeout (5s–2min) |

See [Scan lists and scanning](#scan-lists-and-scanning) below for how this fits
together in practice.

### Button customization

**F1Shrt, F1Long, F2Shrt, F2Long, M Long** — see
[Assignable side-button actions](#assignable-side-button-actions) above.

### Timers, battery, display

| Menu | What it sets |
|---|---|
| **KeyLck** | Auto keypad-lock delay: Off, or 15s–10min |
| **TxTOut** | Max continuous TX time: 30s–15min |
| **BatSav** | Battery-save RX duty cycle: Off, or 1:1–1:5 |
| **BatTxt** | Battery info shown in status bar: None / Voltage / Percent |
| **Mic** | Microphone gain: five levels, +1.5 dB to +15.5 dB |
| **MicBar** | Show a live mic level bar while transmitting |
| **ChDisp** | What's shown for a channel: Frequency / Channel number / Name / Name+Frequency |
| **POnMsg** | What's shown briefly at power-on: All / Sound / Message / Voltage / None |
| **BLTime** | Backlight-on duration: Off, 5s–5min, or always On |
| **BLMin** / **BLMax** | Backlight dimming floor/ceiling (0–9 / 1–10) |
| **BLTxRx** | When backlight lights automatically: Off / TX / RX / TX+RX |
| **Beep** | Key-press confirmation beep on/off |
| **Voice** *(off by default)* | Voice prompt language: Off / Chinese / English |
| **Roger** | End-of-transmission "roger beep": Off / Roger / MDC |
| **STE** | Squelch tail elimination for your own squelch closing |
| **RP STE** | Delay before re-opening squelch after a repeater's own tail-tone elimination |
| **1 Call** | Memory channel jumped to via `F+9` |
| **AlarmT** *(off by default)* | Alarm tone type: Site (siren) / Tone |

### DTMF and calling

See [DTMF calling](#dtmf-calling) below for the full picture — these menu items
configure it:

| Menu | What it sets |
|---|---|
| **ANI ID** *(off by default)* | Your own DTMF caller ID string |
| **UPCode** / **DWCode** | DTMF codes sent on PTT-release / PTT-press |
| **PTT ID** | When ID codes are sent: Off / Up / Down / Up+Down / Apollo Quindar |
| **D ST** | Hear your own DTMF tones locally while sending |
| **D Resp** *(off by default)* | Action on receiving a call: Do nothing / Ring / Reply / Both |
| **D Hold** *(off by default)* | How long an incoming-call alert stays up: 5–60s |
| **D Prel** | Lead-in delay before the first DTMF digit: 30–990ms |
| **D Decd** *(off by default)* | Enable DTMF call decoding on this channel |
| **D List** *(off by default)* | Browse/dial one of 16 saved DTMF contacts |
| **D Live** | Show incoming DTMF digits live on screen as they arrive |
| **VOX** | Voice-operated TX: Off, or sensitivity 1–10 |

### F4HWN "Set" menus

These are new menu entries added by this fork, generally near the end of the list:

| Menu | What it sets |
|---|---|
| **SysInf** | Firmware version/author info (replaces the old plain battery-voltage screen) |
| **RxMode** | Combined dual-watch/cross-band mode: Main Only / Dual RX Respond / Cross Band / Main TX Dual RX |
| **Sql** | Squelch sensitivity: 0 (open)–9 (tightest) |
| **SetPwr** | Actual wattage used when Power is set to "User" |
| **SetPTT** | PTT mode: Classic / OnePush |
| **SetTOT** / **SetEOT** | Alert style for TX-timeout / end-of-transmission: Off / Sound / Visual / All |
| **SetCtr** | LCD contrast: 1–15 |
| **SetInv** | Invert LCD colors |
| **SetLck** | What the keypad lock covers: Keys / Keys+PTT |
| **SetMet** | S-meter style: Tiny / Classic |
| **SetGUI** | VFO frequency font size: Tiny / Classic |
| **SetTmr** | Show RX/TX duration timers in the status bar |
| **SetOff** | Delay before deep sleep: Off, or 1 minute–2 hours |
| **SetNFM** | What "Narrow" bandwidth actually means: Narrow (12.5 kHz) / Narrower (6.25 kHz) |
| **SetVol** *(off by default)* | Fine digital audio output gain: Off, or 1–63 |
| **SetKey** *(off by default, RescueOps)* | Which key + PTT at power-on toggles RescueOps menu-lock |
| **SetNWR** *(off by default)* | NOAA weather-channel auto-scan on/off |
| **Msg** *(off by default)* | Opens the [Message](message-feature-user-guide.md) app |

### Hidden menu items

These never appear while scrolling normally. To reach them, power the radio on
while holding **PTT + the upper side button (SIDE1)**.

| Menu | What it sets |
|---|---|
| **F Lock** | TX band-plan restriction: several regional HAM plans, PMR446, GMRS/FRS/MURS, Disable all, or Unlock all (requires re-selecting "Unlock all" 3 times as a confirmation) |
| **350 En** | Enable TX in the 350 MHz band (still subject to F Lock) |
| **FrCali** *(off by default)* | Reference crystal frequency calibration |
| **BatCal** | Battery voltage-reading calibration |
| **BatTyp** | Battery capacity for percentage calc: 1600/2200/3500 mAh |
| **Reset** | Factory reset: VFO settings only, or everything including channels |

## Standalone modes and screens

### Spectrum Analyzer

Enter with **F + 5** (shares this shortcut with NOAA — only one is wired up per
build). Shows a live bar graph of signal strength across a band of frequencies
around your current VFO, with an arrow tracking the strongest signal found.

- **UP/DOWN** — retune the whole window
- **1/7** — scan step size
- **2/8** — how far UP/DOWN moves the window
- **4** — zoom (number of bars)
- **3/9** — vertical scale/sensitivity
- **0** — cycle modulation
- **6** — cycle bandwidth
- **5** — jump to a typed frequency
- **`*`/F** — raise/lower the trigger level that decides what counts as a real signal
- **SIDE1** — blacklist the current peak frequency (skip a noisy spot)
- **SIDE2** — toggle backlight
- **PTT** — freeze on the peak and switch to a single-frequency view you can
  fine-tune and adjust RF front-end settings on
- **EXIT** — back to the normal screen

There's no "save as channel" button in the scope itself — tune to a found signal,
then save it as a normal memory channel from the main screen afterward.

### NOAA Weather Radio

Not a separate screen — a background monitor for the 10 US NOAA weather
frequencies.

1. Tune a VFO to a NOAA channel.
2. Turn on **SetNWR** in the menu.

With both set, the radio periodically sweeps the NOAA channels for activity in the
background; combine with Dual Watch so one VFO keeps watching your normal traffic
while the other watches NOAA. A "NOAA" icon appears in the status bar while active.

### Commercial FM Broadcast Radio

Enter via the menu's **FM RADIO** entry, or an assigned button — running it again
turns it off.

- **Digit keys** — type a frequency (VFO mode) or 2-digit memory slot (MR mode)
- **UP/DOWN** — step frequency or memory channel
- **1** — cycle regional band presets
- **3** — toggle VFO ↔ memory mode
- **`*`** — seek to the next station; with F held, auto-scan and save all found
  stations into the 20 FM memory slots
- **MENU** — save the current frequency (VFO mode) or delete the selected station
  (MR mode)
- **EXIT** — cancel a prompt, stop a scan, or exit FM radio entirely
- **PTT** — transmits on your ham radio as normal (FM audio mutes during TX)

### DTMF Calling

Lets radios in a fleet "call" each other and, in an emergency, remotely disable
("kill") or re-enable ("revive") another radio using DTMF tone bursts — like
commercial selective calling. Off by default (`ENABLE_DTMF_CALLING`).

- **Contacts**: **D List** browses up to 16 pre-programmed contacts (name + 3-digit
  code); contacts themselves are set up via PC programming software, not typed on
  the radio.
- **Placing a call**: pick a contact (or type a code with a quick `*` tap), then
  press PTT to send it — your own **ANI ID** is appended automatically.
- **Receiving a call**: if a code matches your radio's ANI ID, **D Resp** controls
  the reaction: do nothing, ring, auto-reply, or both. **D Hold** sets how long the
  ring alert stays up.
- **D Decd** is per-channel: only channels with it on will act on incoming calling
  codes (kill/revive codes are always honored regardless).
- **Kill/Revive**: special codes (set up via PC software) that disable or re-enable
  a targeted radio's transmitter remotely — useful if a radio is lost or stolen. A
  killed radio shows a solid block in the status bar in place of its usual icons.

### Scan lists and scanning

Hold **`*`** to start/stop scanning. On a free-tuned frequency, this sweeps
continuously by step size looking for any signal (scan lists don't apply). On a
memory channel, it walks your saved channels, filtered by **SList**: list 0 (none),
1, 2, or 3 specifically, all three combined, or every channel regardless of list.

Assign a channel to lists with **ScAdd1/2/3**, or long-press **F+5** to cycle a
channel through all combinations quickly. While scanning, a quick digit tap
(0–5) switches scan list on the fly, and **ScnRev** controls what happens once a
signal is found: stop there permanently, hold as long as carrier is present
(plus a short delay), or hold for a fixed timeout regardless. Long-press **MENU**
while paused on a channel to skip it for the rest of this pass without removing it
from its list.

### RescueOps

A locked-down "appliance" mode aimed at handing a radio to a non-technical field
operator (firefighters, sea/mountain rescue) without risking accidental
misconfiguration. Off by default (`ENABLE_FEAT_F4HWN_RESCUE_OPS`).

Power the radio on while holding **PTT + your configured SetKey** to toggle a
menu-lock flag. While locked:

- The **entire menu becomes inaccessible** — pressing MENU does nothing.
- The status bar's usual mode icon (DWR/XB/MO) is replaced by an **"RO"** icon, and
  the "F" key indicator is hidden too.

Two quick, **momentary** actions remain available via the side buttons even while
locked, since the menu can't be used to adjust anything: **Power High** (forces full
TX power for the next transmission) and **Remove Offset** (forces simplex, bypassing
any repeater shift, for the next transmission) — both reset back to normal the
moment you release PTT, so they're one-shot overrides rather than persistent
setting changes.

### AirCopy

Wirelessly clones your entire EEPROM (all memory channels and settings) from one
radio to another, over the air, no cable needed. On by default
(`ENABLE_AIRCOPY`).

**Entering:** Power the radio on while holding **PTT + the lower side button
(SIDE2)**. The screen shows `AIR COPY(RDY)`.

**On the sending radio:** press **MENU** to begin sending — the screen changes to
`AIR COPY` and a percentage/gauge bar tracks progress as it streams your entire
EEPROM out in blocks.

**On the receiving radio:** it's already listening once in AirCopy mode; it fills in
the gauge bar as blocks arrive and shows `AIR COPY(CMP)` when the transfer
completes.

**Frequency:** both radios need to be on the same one — from the AirCopy screen,
type 6 digits directly to set it (applies as soon as the 6th digit is entered, same
shortcut the Message feature's frequency screen is modeled on).

**EXIT** resets the current transfer and immediately re-arms the radio to listen
for an incoming copy — there's no dedicated "exit AirCopy" action; the only way
back to normal operation is to power-cycle the radio.

### Message (radio-to-radio text)

Send short text messages to a specific radio or broadcast to everyone on the
frequency — see the dedicated [Message user guide](message-feature-user-guide.md)
for full details. Off by default (`ENABLE_FEAT_F4HWN_MESSAGE`).

### Game (Breakout)

Enter with **F + 7** (off by default, `ENABLE_FEAT_F4HWN_GAME`). A built-in
brick-breaking game: **UP/DOWN** (or `4`/`0`) move the paddle, **MENU**
pauses/resumes, **EXIT** quits back to the radio. Clearing all bricks advances a
level (faster, extra ball); losing all balls ends the game and restarts
automatically. Score and level show at the top while playing.

## Firmware editions

This fork ships several pre-configured build "editions," each a different subset of
the features above baked in at compile time (see the [README](../README.md) and
`compile-with-docker.sh`):

- **Bandscope** — includes the Spectrum Analyzer.
- **Broadcast** — includes commercial FM radio support.
- **Basic** — a stripped-down build (no VOX, no AirCopy, etc.) for the smallest
  possible image.
- **RescueOps** — the menu-lock appliance mode described above, for first-responder
  fleets.
- **Game** — includes the Breakout game.

A single custom build (like the one this manual assumes) can combine most of these
freely, flash space permitting — the DP32G030 has a tight 60K budget, so a build
with everything on may need to drop one large feature (the spectrum analyzer is the
biggest) to fit.
