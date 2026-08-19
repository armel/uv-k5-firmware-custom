# Message (radio-to-radio SMS) — User Guide

Message is a short-text, radio-to-radio messaging feature built on top of the same
BK4819 FSK data modem the stock **AirCopy** feature uses. It lets you send a short
text message to a specific radio (by a small numeric ID) or broadcast it to everyone
listening, with delivery confirmation and automatic retry for direct messages.

It is not a replacement for voice — it's a lightweight, best-effort data channel that
piggybacks on the same hardware, for things like "I'm at the trailhead" or a channel
change without keying up.

## Requirements

- Both radios must be flashed with a build that has `ENABLE_FEAT_F4HWN_MESSAGE=1`
  (off by default — see the main [README](../README.md#building) for build
  instructions). Message adds a few KB to the firmware image; on a full-featured
  build you may need to drop another optional feature (the spectrum analyzer is the
  biggest single one) to fit it in the DP32G030's 60K flash. See the
  [architecture doc](message-feature-architecture.md#flash-footprint) for details.
- Both radios must be tuned to the **same frequency**. Message does not change your
  frequency for you — see [Setting the frequency](#setting-the-frequency-without-leaving-message-mode)
  below for a shortcut that helps with this.
- Each radio needs its own **radio ID** set once (see [below](#setting-your-radio-id)).

Message automatically takes care of bandwidth, modulation, CTCSS/DCS, repeater
offset, TX power, and disabling Dual Watch/Cross-Band/Battery Save for the duration
of the session — you don't need to configure any of that yourself, and your normal
channel settings are restored the moment you leave the Message screen.

## Opening Message mode

1. Press `MENU`, scroll to the **Msg** entry (in the normal, always-visible part of
   the menu — no special key combo needed, unlike AirCopy).
2. Press `MENU` to open it, then `MENU` again to confirm — this drops you into the
   **Inbox** screen.

## Setting your radio ID

Every radio needs a small numeric ID (1–65534) that identifies it to others. Set it
once per radio; it's saved to EEPROM and survives power-off.

1. From the Inbox, press `*`.
2. Type the ID with the number keys (up to 5 digits).
3. Press `MENU` to save, or `EXIT` to cancel.

Your current ID is always shown in the bottom-left of the Inbox screen (`ID:<n>`).
Give each radio in your group a different ID.

## The Inbox screen

The Inbox is the home screen of Message mode. It shows your most recently selected
message (sender, a preview of the text, and its position in your history — e.g.
`2/5`), or **NO MESSAGES** if you haven't received anything yet.

Below that is a live diagnostics line: `<RSSI>dBm F<n> [RX..]` — signal strength,
a running count of frames the radio has locked onto, and an `RX..` marker that only
appears while a frame is actively being captured right now. This is mainly useful
for confirming a transmission is actually reaching you (see
[Reading the diagnostics](#reading-the-diagnostics)).

## Composing and sending a message

1. From the Inbox, press `MENU`.
2. You're asked **TO ID?** — type the destination radio's ID with the number keys,
   or press `*` instead to send a **broadcast** to every listening radio (no ID
   needed). Press `MENU` to confirm, or `EXIT` to back out.
3. Type your message (up to 58 characters) using **multi-tap**, the same style as
   texting on an old phone keypad — the number keys are grouped into letters just
   like a phone dial pad:

   | Key | Letters |
   |---|---|
   | `0` | space |
   | `1` | `. , ! ? ' -` |
   | `2` | A B C |
   | `3` | D E F |
   | `4` | G H I |
   | `5` | J K L |
   | `6` | M N O |
   | `7` | P Q R S |
   | `8` | T U V |
   | `9` | W X Y Z |

   Press a key repeatedly to cycle through its letters (e.g. `4` `4` `4` types "I").
   After about a second of no further presses on that key, the letter is
   locked in and the cursor moves on automatically — or press `#` to lock it in
   immediately without waiting. Pressing a *different* key always locks in
   whatever you were just typing first.

   - `*` **switches input mode**: `ABC` → `abc` → `123` → back to `ABC`. The
     current mode is shown at the bottom of the screen (e.g. `12/58 abc`). In
     `123` mode, number keys type digits directly instead of cycling letters —
     use this for phone numbers, coordinates, etc.
   - `UP` / `DOWN` fine-tune the character at the cursor one step at a time
     through the full symbol set — a fallback for anything not on a letter
     group above.
   - `EXIT` cancels a letter that's still mid-cycle (before it locks in), or
     otherwise backspaces one character; at the very start, backs out to the
     destination screen.
   - `MENU` sends the message.
4. You'll see **SENDING...**, then one of:
   - **DELIVERED** — a direct message was acknowledged by the recipient.
   - **SENT** — a broadcast went out (broadcasts are never acknowledged).
   - **NO REPLY** — a direct message got no acknowledgment after 3 retries
     (roughly 3.5 seconds total).

   This screen returns to the Inbox on its own after a moment, or immediately if
   you press `EXIT`.

Direct messages are automatically retried up to 3 times if no acknowledgment comes
back. Broadcasts are sent twice in a row for better odds of being heard, since
there's no acknowledgment to confirm they landed.

## Reading a message

1. From the Inbox, use `UP` / `DOWN` to select a message.
2. Press `F` (the `#` key) to open it in the full-screen reader.
3. The reader shows up to 48 characters at once across three lines, with a
   `<from>-<to>/<total>` character-range indicator at the top. If the message is
   longer than what's visible, use `UP` / `DOWN` to scroll in 16-character steps.
4. Press `EXIT` to go back to the Inbox.

## Deleting a message

While reading a message (see above), press `*` to delete it. It's removed
immediately — there's no confirmation prompt — and you're returned to the Inbox.
Later messages shift up to fill the gap.

## Setting the frequency without leaving Message mode

Since both radios need to be on the same frequency, there's a shortcut so you don't
have to back out to the main screen to fix a mismatch: from the Inbox, just start
typing a frequency with the number keys (6 digits, e.g. `434000` for 434.000 MHz).
It applies as soon as the 6th digit is entered — no confirm key needed. `EXIT`
backspaces a digit, or cancels back to the Inbox if nothing's been typed yet.

## Reading the diagnostics

The `<RSSI>dBm F<n> [RX..]` line on the Inbox is there to help you tell a real
message attempt apart from unrelated radio noise:

- **RSSI** moves with any RF energy on the frequency, message-related or not.
- **`RX..`** appears only while the radio's FSK demodulator is actively capturing a
  frame right now — if you hear noise and `RX..` lights up at the same time, that
  noise is a real transmission attempt (yours or someone else's), even if it doesn't
  end up decoding successfully. If `RX..` never appears, whatever you're hearing
  isn't a framed transmission at all.
- **`F<n>`** is a running count (wraps at 100) of complete frames the radio has
  locked onto this session, decoded or not — useful for confirming activity is
  happening at all during troubleshooting.

## Known limitations

- **Foreground only.** Message only sends and receives while its screen is actually
  open. If you're back on the main VFO screen, or in the middle of a voice
  transmission, incoming messages are simply missed — there's no background
  listening or queuing.
- **No encryption.** Message content is lightly obfuscated (the same scheme AirCopy
  uses) to avoid it looking like a plain analog signal, but this is not security —
  anyone with a compatible radio and firmware can read it.
- **History is temporary.** The last 5 received messages are kept in RAM only and
  are lost when the radio powers off.
- **Not interoperable.** Message only talks to other radios running this same
  firmware feature — it won't work with stock firmware, other custom forks, or
  AirCopy itself (they share the same underlying radio chip trick, but use
  different, incompatible framing).

## Troubleshooting checklist

If messages aren't getting through:

1. Confirm both radios are actually flashed with a Message-enabled build, and both
   have a **different** radio ID set.
2. Confirm both radios are on the exact same frequency — use the
   [frequency shortcut](#setting-the-frequency-without-leaving-message-mode) on both
   ends and compare digit-for-digit.
3. Leave the receiving radio sitting on the Inbox screen (not the main screen, not a
   sub-menu) and watch the `RX..` indicator while the other radio sends. If it never
   lights up, the issue is at the radio-frequency level, not the messaging feature
   itself.
4. If you have a build with `ENABLE_AIRCOPY=1` handy, AirCopy is a good way to
   sanity-check the underlying radio link independent of Message — see the
   [architecture doc](message-feature-architecture.md#relationship-to-aircopy) for
   why the two share so much plumbing.
