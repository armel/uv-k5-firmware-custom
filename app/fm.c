/* ============================================================================
 *  Copyright 2023 Dual Tachyon
 *  https://github.com/DualTachyon
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 * ============================================================================
 */

#ifdef ENABLE_FMRADIO


/* ============================================================================
 *                                 INCLUDES
 * ============================================================================
 */

#include <string.h>

#include "app/action.h"
#include "app/fm.h"
#include "app/generic.h"
#include "audio.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/bk1080.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"


/* ============================================================================
 *                              HELPER MACROS
 * ============================================================================
 */

/**
 * @brief Compute the number of elements in a static array.
 */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif


/* ============================================================================
 *                         GLOBAL FM RADIO STATE
 * ============================================================================
 */

/**
 * @brief Stored FM memory channels.
 *
 * Each element holds a frequency in 10 kHz units.
 * Example: 10170 = 101.7 MHz
 * Value 0xFFFF means "empty slot".
 */
uint16_t gFM_Channels[20];

/**
 * @brief True when FM radio mode is active.
 */
bool gFmRadioMode;

/**
 * @brief UI timeout countdown in 500 ms units.
 */
uint8_t gFmRadioCountdown_500ms;

/**
 * @brief Audio resume delay timer in 10 ms units.
 *
 * Marked volatile because it is modified from timer/ISR contexts.
 */
volatile uint16_t gFmPlayCountdown_10ms;

/**
 * @brief Current scan direction/state.
 *
 * Values:
 *   FM_SCAN_OFF = not scanning
 *   +1          = scanning upward
 *   -1          = scanning downward
 */
volatile int8_t gFM_ScanState;

/**
 * @brief True when performing a full automatic scan to populate memory.
 */
bool gFM_AutoScan;

/**
 * @brief Index into gFM_Channels[] for the next stored station.
 */
uint8_t gFM_ChannelPosition;

/**
 * @brief True when the tuner reports a valid locked station.
 */
bool gFM_FoundFrequency;

/**
 * @brief Countdown used when restoring radio state after interruptions.
 */
uint16_t gFM_RestoreCountdown_10ms;


/* ============================================================================
 *                           BUTTON STATE CONSTANTS
 * ============================================================================
 */

/**
 * @brief Raw button state bit flags.
 */
const uint8_t BUTTON_STATE_PRESSED = 1 << 0;
const uint8_t BUTTON_STATE_HELD    = 1 << 1;

/**
 * @brief Logical button events derived from raw states.
 */
const uint8_t BUTTON_EVENT_PRESSED = BUTTON_STATE_PRESSED;
const uint8_t BUTTON_EVENT_HELD    = BUTTON_STATE_PRESSED | BUTTON_STATE_HELD;
const uint8_t BUTTON_EVENT_SHORT   = 0;
const uint8_t BUTTON_EVENT_LONG    = BUTTON_STATE_HELD;


/* ============================================================================
 *                          FORWARD DECLARATIONS
 * ============================================================================
 */

static void Key_FUNC(KEY_Code_t Key, uint8_t state);


/* ============================================================================
 *                        FM MEMORY CHANNEL VALIDATION
 * ============================================================================
 */

/**
 * @brief Check whether a memory channel slot contains a usable station.
 *
 * Conditions:
 *   - Channel index must be within bounds
 *   - Stored frequency must lie within the current FM band limits
 *
 * @param Channel Memory channel index (0–19)
 * @return true if valid, false otherwise
 */
// bool FM_CheckValidChannel(uint8_t Channel)
// {
//     return Channel < ARRAY_SIZE(gFM_Channels) &&
//            gFM_Channels[Channel] >= BK1080_GetFreqLoLimit(gEeprom.FM_Band) &&
//            gFM_Channels[Channel] <  BK1080_GetFreqHiLimit(gEeprom.FM_Band);
// }


/**
 * @brief Find the next valid memory channel in a given direction.
 *
 * The search wraps around and checks all 20 slots at most once.
 *
 * @param Channel   Starting channel index (may be invalid or 0xFF)
 * @param Direction +1 to search upward, -1 to search downward
 *
 * @return Channel index if found, otherwise 0xFF
 */
// uint8_t FM_FindNextChannel(uint8_t Channel, uint8_t Direction)
// {
//     for (unsigned i = 0; i < ARRAY_SIZE(gFM_Channels); i++) {

//         /* Wrap around at array boundaries */
//         if (Channel == 0xFF)
//             Channel = ARRAY_SIZE(gFM_Channels) - 1;
//         else if (Channel >= ARRAY_SIZE(gFM_Channels))
//             Channel = 0;

//         /* Return first valid slot */
//         if (FM_CheckValidChannel(Channel))
//             return Channel;

//         Channel += Direction;
//     }

//     /* No valid channels exist */
//     return 0xFF;
// }


/* ============================================================================
 *                     FM CHANNEL / MODE CONFIGURATION
 * ============================================================================
 */

/**
 * @brief Configure which frequency should currently be playing.
 *
 * Behavior:
 *   - If NOT in memory (MR) mode:
 *       Use gEeprom.FM_SelectedFrequency directly.
 *
 *   - If in memory (MR) mode:
 *       Find the next valid stored channel and tune to it.
 *       If no valid memory channels exist, MR mode is disabled.
 *
 * @return  0 on success
 *         -1 if MR mode failed due to no valid channels
 */
int FM_ConfigureChannelState(void)
{
    /* Default behavior: use the selected VFO frequency */
    gEeprom.FM_FrequencyPlaying = gEeprom.FM_SelectedFrequency;


    return 0;
}


/* ============================================================================
 *                            FM RADIO SHUTDOWN
 * ============================================================================
 */

/**
 * @brief Fully turn off FM radio mode.
 *
 * This function:
 *   - Stops any scanning
 *   - Mutes the audio path and speaker
 *   - Powers down the BK1080 tuner IC
 *   - Forces a UI refresh
 *   - Saves resume state if enabled
 */
void FM_TurnOff(void)
{
    gFmRadioMode              = false;
    gFM_ScanState             = FM_SCAN_OFF;
    gFM_RestoreCountdown_10ms = 0;

    /* Disconnect audio path and speaker */
    AUDIO_AudioPathOff();
    gEnableSpeaker = false;

    /* Reset BK1080 tuner into low-power state */
    BK1080_Init0();

    /* Force UI refresh */
    gUpdateStatus = true;

#ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
    /* Record that we exited FM mode */
    gEeprom.CURRENT_STATE = 0;
    SETTINGS_WriteCurrentState();
#endif
}


/* ============================================================================
 *                           FM MEMORY ERASE (DISABLED)
 * ============================================================================
 */

/*
 * The following function was originally used to erase all FM memory
 * channels in EEPROM and RAM. It has been commented out to remove
 * memory-channel features for a simplified FM radio mode.
 */

// void FM_EraseChannels(void)
// {
//     uint8_t Template[8];
//     memset(Template, 0xFF, sizeof(Template));
//
//     /* Write 5 blocks × 8 bytes = 40 bytes = 20 channels × 2 bytes */
//     for (unsigned i = 0; i < 5; i++)
//         EEPROM_WriteBuffer(0x0E40 + (i * 8), Template);
//
//     /* Clear RAM mirror */
//     memset(gFM_Channels, 0xFF, sizeof(gFM_Channels));
// }


/* ============================================================================
 *                             FREQUENCY TUNING
 * ============================================================================
 */

/**
 * @brief Tune the FM radio to a new frequency.
 *
 * @param Frequency Starting frequency (10 kHz units)
 * @param Step      +1 or -1 step amount (usually 100 kHz or band spacing)
 * @param bFlag     true  → tune exactly to Frequency (ignore Step)
 *                  false → apply Step and wrap around band limits
 *
 * Behavior:
 *   - Mutes audio while tuning
 *   - Resets scan-related flags
 *   - Programs the BK1080 tuner IC
 */
void FM_Tune(uint16_t Frequency, int8_t Step)
{
    /* Mute audio during retune */
    AUDIO_AudioPathOff();
    gEnableSpeaker = false;

    /* Set how long to wait before audio resumes */
    gFmPlayCountdown_10ms =
        (gFM_ScanState == FM_SCAN_OFF) ?
            fm_play_countdown_noscan_10ms :
            fm_play_countdown_scan_10ms;

    /* Reset scan-related flags */
    gScheduleFM        = false;
    gFM_FoundFrequency = false;
    gAskToSave         = false;
    gAskToDelete       = false;

    /* Default behavior: tune exactly to supplied frequency */
    gEeprom.FM_FrequencyPlaying = Frequency;

    if (true) {

        /* Apply step and wrap around band limits */
        Frequency += Step;

        if (Frequency < 640)
            Frequency = 1080;
        else if (Frequency > 1080)
            Frequency = 640;

        gEeprom.FM_FrequencyPlaying = Frequency;
    }

    /* Record scan direction/state */
    gFM_ScanState = Step;

    /* Program tuner IC */
    BK1080_SetFrequency(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band /*, gEeprom.FM_Space*/);
}


/* ============================================================================
 *                     STOP SCANNING AND PLAY STATION
 * ============================================================================
 */

/**
 * @brief Finalize scanning and begin audio playback.
 *
 * This function:
 *   - Stops scanning
 *   - Switches to memory mode if auto-scan was running
 *   - Tunes hardware to the final frequency
 *   - Saves FM settings to EEPROM
 *   - Enables audio output
 */
void FM_PlayAndUpdate(void)
{
    gFM_ScanState = FM_SCAN_OFF;

    /* If auto-scan completed, force memory mode */
    // if (gFM_AutoScan) {
    //     gEeprom.FM_IsMrMode         = true;
    //     gEeprom.FM_SelectedChannel = 0;
    // }

    /* Ensure selected channel/frequency is valid */
    FM_ConfigureChannelState();

    /* Tune hardware */
    BK1080_SetFrequency(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band /*, gEeprom.FM_Space*/);

    /* Persist FM settings */
    SETTINGS_SaveFM();

    /* Reset timing and flags */
    gFmPlayCountdown_10ms = 0;
    gScheduleFM          = false;
    gAskToSave           = false;

    /* Enable audio */
    AUDIO_AudioPathOn();
    gEnableSpeaker = true;
}


/* ============================================================================
 *                   CHECK WHETHER A FREQUENCY IS A STATION
 * ============================================================================
 */

/**
 * @brief Determine whether the tuner has locked onto a real FM station.
 *
 * Uses BK1080 internal metrics:
 *   - SNR (signal-to-noise ratio)
 *   - RSSI (signal strength)
 *   - AFC deviation (how far off-center tuning is)
 *
 * @param Frequency  Frequency being tested
 * @param LowerLimit Lowest valid band frequency
 *
 * @return  0 if a valid station is detected
 *         -1 otherwise
 */
int FM_CheckFrequencyLock(uint16_t Frequency, uint16_t LowerLimit)
{
    int ret = -1;

    /* Read tuner status register (contains SNR and AFC deviation) */
    const uint16_t Test2 = BK1080_ReadRegister(BK1080_REG_07);

    /* Extract AFC frequency deviation (unsigned in driver, actually signed) */
    const uint16_t Deviation = BK1080_REG_07_GET_FREQD(Test2);

    /* Reject very low SNR → likely just noise */
    if (BK1080_REG_07_GET_SNR(Test2) <= 2) {
        BK1080_FrequencyDeviation = Deviation;
        BK1080_BaseFrequency      = Frequency;
        return ret;
    }

    /* Read RSSI and AFC rail status */
    const uint16_t Status = BK1080_ReadRegister(BK1080_REG_10);

    /* Reject if AFC is railed or signal strength is weak */
    if ((Status & BK1080_REG_10_MASK_AFCRL) != BK1080_REG_10_AFCRL_NOT_RAILED ||
        BK1080_REG_10_GET_RSSI(Status) < 10) {

        BK1080_FrequencyDeviation = Deviation;
        BK1080_BaseFrequency      = Frequency;
        return ret;
    }

    /* Reject if AFC deviation is too small or too large */
    if (Deviation >= 280 && Deviation <= 3815) {
        BK1080_FrequencyDeviation = Deviation;
        BK1080_BaseFrequency      = Frequency;
        return ret;
    }

    /*
     * Additional heuristics:
     * If stepping upward by one unit and AFC sign looks wrong,
     * reject this as a real station.
     */
    if (Frequency > LowerLimit && (Frequency - BK1080_BaseFrequency) == 1) {
        if (BK1080_FrequencyDeviation & 0x800 ||
            (BK1080_FrequencyDeviation < 20)) {

            BK1080_FrequencyDeviation = Deviation;
            BK1080_BaseFrequency      = Frequency;
            return ret;
        }
    }

    /*
     * Same logic for stepping downward by one unit.
     */
    if (Frequency >= LowerLimit && (BK1080_BaseFrequency - Frequency) == 1) {
        if ((BK1080_FrequencyDeviation & 0x800) == 0 ||
            (BK1080_FrequencyDeviation > 4075)) {

            BK1080_FrequencyDeviation = Deviation;
            BK1080_BaseFrequency      = Frequency;
            return ret;
        }
    }

    /* Passed all checks → valid station */
    ret = 0;
    BK1080_FrequencyDeviation = Deviation;
    BK1080_BaseFrequency      = Frequency;

    return ret;
}


/* ============================================================================
 *                         DIGIT KEY HANDLER (0–9)
 * ============================================================================
 */

/**
 * @brief Handle numeric key presses while in FM mode.
 *
 * This stripped-down version supports only direct frequency entry
 * (VFO-style), such as:
 *   1017 → 101.7 MHz
 *
 * Memory channel shortcuts are intentionally removed.
 */
static void Key_DIGITS(KEY_Code_t Key, uint8_t state)
{
    /* Only process short presses when F-key wasn't held */
    if (state == BUTTON_EVENT_SHORT && !gWasFKeyPressed) {

        /* Disallow digit entry during scanning */
        if (gFM_ScanState != FM_SCAN_OFF) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        /* Append digit to on-screen input box */
        INPUTBOX_Append(Key);
        gRequestDisplayScreen = DISPLAY_FM;

        /* --------------------------------------------------------------------
         *                     FREQUENCY ENTRY MODE (VFO)
         * --------------------------------------------------------------------
         */

        /*
         * If the first digit is > 1, shift it right so
         * inputs like "875" become "0875".
         */
        if (gInputBoxIndex == 1) {
            if (gInputBox[0] > 1) {
                gInputBox[1] = gInputBox[0];
                gInputBox[0] = 0;
                gInputBoxIndex = 2;
            }
        }

        /*
         * Once 4 digits are entered, parse them into a frequency
         * and immediately tune the radio.
         */
        else if (gInputBoxIndex > 3) {

            uint32_t Frequency;

            gInputBoxIndex = 0;
            Frequency = StrToUL(INPUTBOX_GetAscii());

            /* Reject out-of-band frequencies */
            if (Frequency < 640 || Frequency > 1080) { 

                gBeepToPlay           = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                gRequestDisplayScreen = DISPLAY_FM;
                return;
            }

            /* Accept and apply new frequency */
            gEeprom.FM_SelectedFrequency = (uint16_t)Frequency;
            gEeprom.FM_FrequencyPlaying  = gEeprom.FM_SelectedFrequency;

#ifdef ENABLE_VOICE
            gAnotherVoiceID = (VOICE_ID_t)Key;
#endif

            /* Program tuner immediately */
            BK1080_SetFrequency(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band);

            /* Save and refresh UI */
            gRequestSaveFM = true;
            gRequestDisplayScreen = DISPLAY_FM;
            return;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
    }
    else {
        /* If not handled here, forward to function-key handler */
        Key_FUNC(Key, state);
    }
}


/* ============================================================================
 *                      FUNCTION KEY HANDLER (STAR + DIGITS)
 * ============================================================================
 */

/**
 * @brief Handle function-key combinations (typically STAR + digit).
 *
 * Supported actions:
 *   - Exit FM mode
 *   - Change FM band
 *   - Start scanning (manual or auto)
 */
static void Key_FUNC(KEY_Code_t Key, uint8_t state)
{
    if (state == BUTTON_EVENT_SHORT || state == BUTTON_EVENT_HELD) {

        /* Auto-scan if F was held or key itself is held */
        

        gBeepToPlay           = BEEP_1KHZ_60MS_OPTIONAL;
        gWasFKeyPressed       = false;
        gUpdateStatus         = true;
        gRequestDisplayScreen = DISPLAY_FM;

        switch (Key) {

            case KEY_0:
                /* Exit FM mode */
                ACTION_FM();
                break;

            case KEY_1:
                /* Cycle FM band (e.g., US/EU/Japan) */
                gEeprom.FM_Band++;
                gRequestSaveFM = true;
                break;

            /*
            case KEY_2:
                gEeprom.FM_Space = (gEeprom.FM_Space + 1) % 3;
                gRequestSaveFM = true;
                break;
            */

            /*
            case KEY_3:
                Toggle memory (MR) mode — intentionally removed
                for simplified FM-only implementation.
            */

            case KEY_STAR:
                /* Start scan (auto-scan if held) */
                ACTION_Scan();
                break;

            default:
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                break;
        }
    }
}


/* ============================================================================
 *                             EXIT KEY HANDLER
 * ============================================================================
 */

/**
 * @brief Handle EXIT key behavior.
 *
 * Behavior:
 *   - Cancels save/delete prompts
 *   - Backs out of digit entry
 *   - Stops scanning
 *   - Exits FM mode if idle
 */
static void Key_EXIT(uint8_t state)
{
    if (state != BUTTON_EVENT_SHORT)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    if (gFM_ScanState == FM_SCAN_OFF) {

        /* Not scanning */
        if (gInputBoxIndex == 0) {

            /* No digit entry active */
            if (!gAskToSave && !gAskToDelete) {
                ACTION_FM();  /* Exit FM mode */
                return;
            }

            /* Cancel save/delete prompt */
            gAskToSave   = false;
            gAskToDelete = false;
        }
        else {

            /* Backspace one digit */
            gInputBox[--gInputBoxIndex] = 10;

            /*
             * If only one digit remains and it's zero,
             * clear the input box entirely.
             */
            if (gInputBoxIndex) {
                if (gInputBoxIndex != 1) {
                    gRequestDisplayScreen = DISPLAY_FM;
                    return;
                }

                if (gInputBox[0] != 0) {
                    gRequestDisplayScreen = DISPLAY_FM;
                    return;
                }
            }

            gInputBoxIndex = 0;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = VOICE_ID_CANCEL;
#endif
    }
    else {

        /* If scanning, stop and play current frequency */
        FM_PlayAndUpdate();

#ifdef ENABLE_VOICE
        gAnotherVoiceID = VOICE_ID_SCANNING_STOP;
#endif
    }

    gRequestDisplayScreen = DISPLAY_FM;
}


/* ============================================================================
 *                             MENU KEY HANDLER
 * ============================================================================
 */

/**
 * @brief Handle MENU key presses.
 *
 * Behavior:
 *   - In frequency mode → toggle "save current frequency" prompt
 *   - While scanning → commit scan results (if valid)
 *
 * Memory deletion logic has been removed for FM-only simplicity.
 */
static void Key_MENU(uint8_t state)
{
    if (state != BUTTON_EVENT_SHORT)
        return;

    gRequestDisplayScreen = DISPLAY_FM;
    gBeepToPlay           = BEEP_1KHZ_60MS_OPTIONAL;

    
}


/* ============================================================================
 *                         UP / DOWN KEY HANDLER
 * ============================================================================
 */

/**
 * @brief Handle UP and DOWN arrow keys.
 *
 * Behavior:
 *   - While scanning → manually step tuning
 *   - While idle → step frequency in VFO mode
 *
 * @param state Button state (pressed/held)
 * @param Step  +1 for UP, -1 for DOWN
 */
static void Key_UP_DOWN(uint8_t state, int8_t Step)
{
    if (state == BUTTON_EVENT_PRESSED) {

        /* Disallow stepping while entering digits */
        if (gInputBoxIndex) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    }
    else if (gInputBoxIndex || state != BUTTON_EVENT_HELD) {
        return;
    }

    /* --------------------------------------------------------------------
     *                         SCANNING MODE
     * --------------------------------------------------------------------
     */
    if (gFM_ScanState != FM_SCAN_OFF) {

        if (gFM_AutoScan) {
            gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            return;
        }

        /* Manual tuning during scan */
        FM_Tune(gEeprom.FM_FrequencyPlaying, Step);
        gRequestDisplayScreen = DISPLAY_FM;
        return;
    }

    /* --------------------------------------------------------------------
     *                         FREQUENCY MODE (VFO)
     * --------------------------------------------------------------------
     */
    else {

        uint16_t Frequency = gEeprom.FM_SelectedFrequency + Step;

        /* Wrap around band limits */
        if (Frequency < 640) Frequency = 1080;
        else if (Frequency > 1080) Frequency = 640;

        gEeprom.FM_FrequencyPlaying  = Frequency;
        gEeprom.FM_SelectedFrequency = gEeprom.FM_FrequencyPlaying;

        /* Skip EEPROM write and jump straight to tuning */
        goto Bail;
    }

    /* If execution falls through, request save */
    gRequestSaveFM = true;

Bail:
    /* Program tuner and refresh UI */
    BK1080_SetFrequency(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band);
    gRequestDisplayScreen = DISPLAY_FM;
}


/* ============================================================================
 *                         TOP-LEVEL FM KEY DISPATCH
 * ============================================================================
 */

/**
 * @brief Main FM-mode key dispatcher.
 *
 * Converts raw key press/hold signals into logical events and routes
 * them to the appropriate handler function.
 *
 * @param Key         Physical key code
 * @param bKeyPressed True if key was pressed
 * @param bKeyHeld    True if key is being held
 */
void FM_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    uint8_t state = bKeyPressed + 2 * bKeyHeld;

    switch (Key) {

        case KEY_0 ... KEY_9:
            Key_DIGITS(Key, state);
            break;

        case KEY_STAR:
            Key_FUNC(Key, state);
            break;

        case KEY_MENU:
            Key_MENU(state);
            break;

        case KEY_UP:
            Key_UP_DOWN(state, 1);
            break;

        case KEY_DOWN:
            Key_UP_DOWN(state, -1);
            break;

        case KEY_EXIT:
            Key_EXIT(state);
            break;

        case KEY_F:
            GENERIC_Key_F(bKeyPressed, bKeyHeld);
            break;

        case KEY_PTT:
            GENERIC_Key_PTT(bKeyPressed);
            break;

        default:
            if (!bKeyHeld && bKeyPressed)
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            break;
    }
}


/* ============================================================================
 *                          SCAN STEP PROCESSING
 * ============================================================================
 */

/**
 * @brief Called periodically while scanning.
 *
 * Logic:
 *   1. Check if current frequency is a valid station.
 *   2. If yes:
 *        - Manual scan → stop and play it.
 *        - Auto-scan → (memory logic removed) continue scanning.
 *   3. If not valid → step to next frequency and continue.
 */
void FM_Play(void)
{
    if (!FM_CheckFrequencyLock(gEeprom.FM_FrequencyPlaying,
                               BK1080_GetFreqLoLimit(gEeprom.FM_Band))) {

        /* Valid station found */
        if (!gFM_AutoScan) {

            gFmPlayCountdown_10ms = 0;
            gFM_FoundFrequency    = true;

            if (!gEeprom.FM_IsMrMode)
                gEeprom.FM_SelectedFrequency = gEeprom.FM_FrequencyPlaying;

            AUDIO_AudioPathOn();
            gEnableSpeaker = true;

            GUI_SelectNextDisplay(DISPLAY_FM);
            return;
        }

        /*
         * Auto-scan memory storage logic intentionally removed.
         */
    }

    /* If reached top of band during auto-scan, stop */
    if (gFM_AutoScan &&
        gEeprom.FM_FrequencyPlaying >= BK1080_GetFreqHiLimit(1)) {

        FM_PlayAndUpdate();
    }
    else {
        FM_Tune(gEeprom.FM_FrequencyPlaying, gFM_ScanState);
    }

    GUI_SelectNextDisplay(DISPLAY_FM);
}


/* ============================================================================
 *                             START FM MODE
 * ============================================================================
 */

/**
 * @brief Enter FM radio mode.
 *
 * This function:
 *   - Disables dual-watch
 *   - Powers up the BK1080 tuner IC
 *   - Tunes to last-used frequency
 *   - Enables speaker/audio path
 *   - Updates UI and resume state
 */
void FM_Start(void)
{
    gDualWatchActive          = false;
    gFmRadioMode              = true;
    gFM_ScanState             = FM_SCAN_OFF;
    gFM_RestoreCountdown_10ms = 0;

    /* Initialize FM tuner to last-used frequency */
    BK1080_Init(gEeprom.FM_FrequencyPlaying, gEeprom.FM_Band /*, gEeprom.FM_Space*/);

    /* Enable audio output */
    AUDIO_AudioPathOn();
    gEnableSpeaker = true;

    gUpdateStatus = true;

#ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
    /* Record current state as FM mode */
    gEeprom.CURRENT_STATE = 3;
    SETTINGS_WriteCurrentState();
#endif
}


#endif /* ENABLE_FMRADIO */
