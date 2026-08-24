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

### Faster: a side-button shortcut

If you'd rather skip the menu entirely, either of the radio's side buttons (SIDE1 or
SIDE2, above the PTT) can be assigned to jump straight to the Message Inbox from
anywhere — the main screen, a menu, mid-scan:

1. Press `MENU`, scroll to **F1Shrt**, **F1Long**, **F2Shrt**, or **F2Long** (whichever
   button/press-length you want to reassign).
2. Press `MENU`, cycle with `UP`/`DOWN` to **MSG**, then `MENU` to confirm.
3. That button now opens Message mode directly, without touching the normal menu.

Pressing it again while already inside Message mode just resets you back to the Inbox
(harmless, but it will drop whatever you were mid-typing) — it doesn't back you out to
the main screen the way `EXIT` does.

## Setting your radio ID

Every radio needs a small numeric ID (1–195) that identifies it to others. Set it
once per radio; it's saved to EEPROM and survives power-off.

1. From the Inbox, press `*`.
2. Type the ID with the number keys (up to 3 digits).
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
4. You'll see **SENDING...** (with a `RETRIES LEFT <n>` counter underneath for
   direct messages, so a long wait doesn't look frozen), then one of:
   - **DELIVERED** — a direct message was acknowledged by the recipient.
   - **SENT** — a broadcast went out (broadcasts are never acknowledged).
   - **NO REPLY** — a direct message got no acknowledgment after all retries were
     used up (up to about a minute — see below for why it's this long).

   This screen returns to the Inbox on its own after a moment, or immediately if
   you press `EXIT`.

Direct messages are automatically retried for up to about **a minute** if no
acknowledgment comes back (20 retries, 3 seconds apart) — long enough to cover
[the automatic page](#getting-paged-automatically-direct-messages-only) below,
since a person (not a radio that's already listening) needs real time to notice it
and switch into Message mode. Broadcasts are sent twice in a row for better odds of
being heard, since there's no acknowledgment to confirm they landed, and they don't
send a page (see below).

## Getting paged automatically (direct messages only)

You don't need the recipient to already be sitting in Message mode. Sending a
**direct** message (not a broadcast) automatically transmits a short DTMF "page" —
a couple of seconds of touch-tone-style beeps — immediately before the message data
itself. Any radio on that frequency decodes DTMF continuously in the background,
even while just sitting on the normal VFO screen doing voice — so if the page is
addressed to your radio ID, your radio automatically jumps into Message mode by
itself, no button press needed, and briefly shows **PAGED BY \<id\>** on the Inbox
screen while the actual message catches up behind it over the next few retries.

A few things worth knowing about this:

- It only happens for **direct** messages — broadcasts have no single recipient to
  page, so they skip this step entirely and behave exactly as before.
- The page is a one-shot: it's sent once, then only the message data itself gets
  retried. If the page itself doesn't get through cleanly (DTMF has no error
  correction of its own), the auto-switch won't happen and the recipient needs to
  already be in Message mode, or you need to tell them some other way (e.g. over
  voice) to switch over.
- It won't interrupt you if you're mid-transmission (talking) when a page for you
  arrives — the radio waits until it's actually listening.
- If you're already in Message mode when paged, nothing visibly changes (you're
  already where you need to be to receive the message).

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

- **Message data itself is foreground only.** The actual message content only sends
  and receives while the Message screen is open — that part hasn't changed. What's
  new is that a **direct** message now automatically pages you out of the normal VFO
  screen first (see [above](#getting-paged-automatically-direct-messages-only)), so
  in practice you often don't have to be in Message mode ahead of time anymore for
  direct messages. **Broadcasts still don't page** — if you're not already watching
  the Inbox, a broadcast sent your way is simply missed, with no background
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
5. **If the automatic page isn't auto-switching a receiving radio out of the VFO
   screen**, check whether DTMF itself is even getting through independent of
   Message: turn on the stock **"D Live"** menu setting (live DTMF decoder) on the
   receiving radio while it sits on the normal VFO screen, and watch for a
   `"DTMF ..."` line to appear when the other radio sends a direct message. If
   nothing shows up at all, DTMF isn't being heard/decoded on that link (an
   RF-level or hardware problem, not specific to Message); if it shows garbled or
   only partial characters, the page timing may need to be tuned for your radios —
   see [`MSG_PAGE_TONE_MS`/`MSG_PAGE_GAP_MS`](message-feature-architecture.md#dtmf-paging)
   in the architecture doc.
