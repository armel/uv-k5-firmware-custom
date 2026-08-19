/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#ifdef ENABLE_FEAT_F4HWN_MESSAGE

#include <string.h>

#include "app/message.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "frequencies.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

// Same FSK frame shape as AirCopy (app/aircopy.c): 36 x uint16 words,
// word[0]/word[35] sync markers, word[1..34] obfuscated, word[34] = CRC.
// word[1] is repurposed here as a packet-type field instead of AirCopy's
// EEPROM offset. Kept independent of app/aircopy.c so this feature works
// whether or not ENABLE_AIRCOPY is also enabled.
static const uint16_t Obfuscation[8] = { 0x6C16, 0xE614, 0x912E, 0x400D, 0x3521, 0x40D5, 0x0313, 0x80E9 };

uint16_t gMsg_FSK_Buffer[36];

MSG_UiMode_t       gMsgUiMode;
MSG_TxState_t      gMsgTxState;

MSG_HistoryEntry_t gMsgHistory[MSG_HISTORY_SIZE];
uint8_t            gMsgHistoryCount;
uint8_t            gMsgHistoryCursor;
unsigned int       gMsgReadScroll;

char               gMsgComposeText[MSG_TEXT_MAX + 1];
unsigned int       gMsgComposeIndex;
uint32_t           gMsgComposeDestID;
bool               gMsgComposeBroadcast;

static MSG_DataPacket_t gMsgPendingPacket;
static bool              gMsgIsBroadcast;
static uint16_t          gMsgTxCountdown_10ms;
static uint8_t           gMsgTxRetriesLeft;
static uint8_t           gMsgBroadcastRepeatsLeft;
static uint16_t          gMsgResultDisplayCountdown_10ms;

static MSG_AckPacket_t   gMsgPendingAck;
static bool              gMsgPendingAckToSend;

// BK4819_SetupAircopy() is only compiled under ENABLE_AIRCOPY; duplicate the
// same register sequence here so Message works standalone.
static void MESSAGE_SetupModem(void)
{
    BK4819_WriteRegister(BK4819_REG_70, 0x00E0);
    BK4819_WriteRegister(BK4819_REG_72, 0x3065);
    BK4819_WriteRegister(BK4819_REG_58, 0x00C1);
    BK4819_WriteRegister(BK4819_REG_5C, 0x5665);
    BK4819_WriteRegister(BK4819_REG_5D, 0x4700);
}

static uint8_t          gMsgSavedBandwidth;
static ModulationMode_t gMsgSavedModulation;
static uint8_t          gMsgSavedOffsetDir;
static DCS_CodeType_t   gMsgSavedRxCodeType;
static uint8_t          gMsgSavedRxCode;
static DCS_CodeType_t   gMsgSavedTxCodeType;
static uint8_t          gMsgSavedTxCode;
static uint8_t          gMsgSavedOutputPower;
static uint8_t          gMsgSavedDualWatch;
static uint8_t          gMsgSavedCrossBand;
static uint8_t          gMsgSavedBatterySave;

void MESSAGE_Enter(void)
{
    gCurrentVfo = gRxVfo;

    // Dual Watch / Cross-Band alternate the active VFO in the background
    // (DualwatchAlternate(), app/app.c ~line 1062) with no exclusion for
    // this screen, reprogramming the BK4819 out from under our FSK setup
    // every couple hundred ms -- and Battery Save periodically powers the
    // receiver down the same way. AirCopy avoids all three by forcing them
    // off the moment it starts (helper/boot.c); do the same here.
    gMsgSavedDualWatch   = gEeprom.DUAL_WATCH;
    gMsgSavedCrossBand   = gEeprom.CROSS_BAND_RX_TX;
    gMsgSavedBatterySave = gEeprom.BATTERY_SAVE;
    gEeprom.DUAL_WATCH       = DUAL_WATCH_OFF;
    gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
    gEeprom.BATTERY_SAVE     = 0;

    // Force the same "clean slate" RF configuration AirCopy gets from its
    // dedicated fresh VFO, on top of whatever channel the user is currently
    // on: NARROW bandwidth, FM, simplex (no repeater offset), no CTCSS/DCS,
    // and low TX power. Only the frequency itself is left as the user tuned
    // it. Any of these left at the channel's normal values (a lingering
    // repeater shift sending TX and RX to different frequencies, a CTCSS
    // tone riding on top of the FSK deviation, AM demod, or high TX power
    // overloading a nearby receiver on the bench) can garble or misroute
    // the burst in ways the dedicated AirCopy channel never has to deal with.
    gMsgSavedBandwidth   = gRxVfo->CHANNEL_BANDWIDTH;
    gMsgSavedModulation  = gRxVfo->Modulation;
    gMsgSavedOffsetDir   = gRxVfo->TX_OFFSET_FREQUENCY_DIRECTION;
    gMsgSavedRxCodeType  = gRxVfo->freq_config_RX.CodeType;
    gMsgSavedRxCode      = gRxVfo->freq_config_RX.Code;
    gMsgSavedTxCodeType  = gRxVfo->freq_config_TX.CodeType;
    gMsgSavedTxCode      = gRxVfo->freq_config_TX.Code;
    gMsgSavedOutputPower = gRxVfo->OUTPUT_POWER;

    gRxVfo->CHANNEL_BANDWIDTH             = BANDWIDTH_NARROW;
    gRxVfo->Modulation                    = MODULATION_FM;
    gRxVfo->TX_OFFSET_FREQUENCY_DIRECTION = TX_OFFSET_FREQUENCY_DIRECTION_OFF;
    gRxVfo->freq_config_RX.CodeType       = CODE_TYPE_OFF;
    gRxVfo->freq_config_TX.CodeType       = CODE_TYPE_OFF;
    gRxVfo->OUTPUT_POWER                  = OUTPUT_POWER_LOW1;
    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);

    RADIO_SetupRegisters(true);
    MESSAGE_SetupModem();
    BK4819_ResetFSK();
    BK4819_PrepareFSKReceive();

    gFSKWriteIndex = 0;

    gMsgUiMode  = MSG_UI_INBOX;
    gMsgTxState = MSG_TX_IDLE;
    gMsgHistoryCursor = (gMsgHistoryCount > 0) ? (gMsgHistoryCount - 1) : 0;
    gMsgPendingAckToSend = false;
}

void MESSAGE_Exit(void)
{
    BK4819_Idle();

    gRxVfo->CHANNEL_BANDWIDTH             = gMsgSavedBandwidth;
    gRxVfo->Modulation                    = gMsgSavedModulation;
    gRxVfo->TX_OFFSET_FREQUENCY_DIRECTION = gMsgSavedOffsetDir;
    gRxVfo->freq_config_RX.CodeType       = gMsgSavedRxCodeType;
    gRxVfo->freq_config_RX.Code           = gMsgSavedRxCode;
    gRxVfo->freq_config_TX.CodeType       = gMsgSavedTxCodeType;
    gRxVfo->freq_config_TX.Code           = gMsgSavedTxCode;
    gRxVfo->OUTPUT_POWER                  = gMsgSavedOutputPower;
    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);

    gEeprom.DUAL_WATCH       = gMsgSavedDualWatch;
    gEeprom.CROSS_BAND_RX_TX = gMsgSavedCrossBand;
    gEeprom.BATTERY_SAVE     = gMsgSavedBatterySave;

    RADIO_SetupRegisters(true);
}

// See the declaration comment in app/message.h -- called by AUDIO_PlayBeep()
// itself, not from here, so it covers every beep in Message mode (including
// the plain per-keypress ones in MESSAGE_ProcessKeys), not just the ones
// added for TX/RX notifications below.
void MESSAGE_RearmModem(void)
{
    MESSAGE_SetupModem();
    BK4819_ResetFSK();
    BK4819_PrepareFSKReceive();
    gFSKWriteIndex = 0;
}

static void MESSAGE_TransmitFrame(uint8_t type, const void *payload64)
{
    gMsg_FSK_Buffer[0]  = 0xABCD;
    gMsg_FSK_Buffer[1]  = type;
    memcpy(&gMsg_FSK_Buffer[2], payload64, 64);
    gMsg_FSK_Buffer[34] = CRC_Calculate(&gMsg_FSK_Buffer[1], 2 + 64);
    gMsg_FSK_Buffer[35] = 0xDCBA;

    for (unsigned int i = 0; i < 34; i++) {
        gMsg_FSK_Buffer[i + 1] ^= Obfuscation[i % 8];
    }

    RADIO_SetTxParameters();
    BK4819_SendFSKData(gMsg_FSK_Buffer);
    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    BK4819_PrepareFSKReceive();
}

static void MESSAGE_PushHistory(const MSG_DataPacket_t *data, bool wasBroadcast)
{
    if (gMsgHistoryCount >= MSG_HISTORY_SIZE) {
        memmove(&gMsgHistory[0], &gMsgHistory[1], sizeof(gMsgHistory[0]) * (MSG_HISTORY_SIZE - 1));
        gMsgHistoryCount = MSG_HISTORY_SIZE - 1;
    }

    MSG_HistoryEntry_t *e = &gMsgHistory[gMsgHistoryCount++];
    e->senderID   = data->senderID;
    e->messageID  = data->messageID;
    e->textLen    = (data->textLen <= MSG_TEXT_MAX) ? data->textLen : MSG_TEXT_MAX;
    memcpy(e->text, data->text, e->textLen);
    e->text[e->textLen] = 0;
    e->wasBroadcast = wasBroadcast;

    gMsgHistoryCursor = gMsgHistoryCount - 1;
}

static void MESSAGE_BeginSend(uint16_t destID, bool broadcast, const char *text, uint8_t len)
{
    static uint8_t gMsgIdCounter;

    gMsgPendingPacket.senderID   = gEeprom.RADIO_ID;
    gMsgPendingPacket.receiverID = broadcast ? MSG_BROADCAST_ID : destID;
    gMsgPendingPacket.messageID  = gMsgIdCounter++;
    gMsgPendingPacket.textLen    = len;
    memset(gMsgPendingPacket.text, 0, MSG_TEXT_MAX);
    memcpy(gMsgPendingPacket.text, text, len);

    gMsgIsBroadcast          = broadcast;
    gMsgTxRetriesLeft        = MSG_TX_MAX_RETRIES;
    gMsgBroadcastRepeatsLeft = broadcast ? 1 : 0;
    gMsgTxState              = MSG_TX_SENDING;
}

static uint8_t  gMsgLastFSKWriteIndex;
static uint16_t gMsgPartialCountdown_10ms;
static uint8_t  gMsgLiveRefreshCountdown_10ms;

// A frame that starts arriving (gFSKWriteIndex > 0) but never reaches 36
// words (e.g. the signal faded mid-burst) would otherwise sit there forever
// with no visible trace. Time it out and count it so the inbox screen can
// show that *something* was heard, even if it never completed.
static void MESSAGE_CheckPartialFrame(void)
{
    if (gFSKWriteIndex != gMsgLastFSKWriteIndex) {
        gMsgLastFSKWriteIndex    = gFSKWriteIndex;
        gMsgPartialCountdown_10ms = 80; // 800ms of inactivity = abandoned
        return;
    }

    if (gFSKWriteIndex > 0 && gFSKWriteIndex < 36
        && gMsgPartialCountdown_10ms > 0 && --gMsgPartialCountdown_10ms == 0) {
        gFSKWriteIndex        = 0;
        gMsgLastFSKWriteIndex = 0;
        gMsgRxPartial++;
        BK4819_PrepareFSKReceive();
        gUpdateDisplay = true;
    }
}

void MESSAGE_TimeSlice10ms(void)
{
    MESSAGE_CheckPartialFrame();

    // Keep the inbox's live RSSI reading moving even with no other activity.
    if (gMsgUiMode == MSG_UI_INBOX
        && (gMsgLiveRefreshCountdown_10ms == 0 || --gMsgLiveRefreshCountdown_10ms == 0)) {
        gMsgLiveRefreshCountdown_10ms = 20; // 200ms
        gUpdateDisplay = true;
    }

    if (gMsgPendingAckToSend) {
        gMsgPendingAckToSend = false;
        MESSAGE_TransmitFrame(MSG_TYPE_ACK, &gMsgPendingAck);
        return;
    }

    switch (gMsgTxState) {
        case MSG_TX_SENDING:
            MESSAGE_TransmitFrame(
                gMsgIsBroadcast ? MSG_TYPE_DATA_BROADCAST : MSG_TYPE_DATA_UNICAST,
                &gMsgPendingPacket);

            if (gMsgIsBroadcast) {
                if (gMsgBroadcastRepeatsLeft > 0) {
                    gMsgBroadcastRepeatsLeft--;
                    gMsgTxCountdown_10ms = MSG_BROADCAST_REPEAT_10MS;
                    gMsgTxState = MSG_TX_BC_GAP;
                } else {
                    gMsgTxState = MSG_TX_ACKED;
                    gMsgResultDisplayCountdown_10ms = 100;
                    AUDIO_PlayBeep(BEEP_880HZ_60MS_DOUBLE_BEEP);
                }
            } else {
                gMsgTxCountdown_10ms = MSG_ACK_TIMEOUT_10MS;
                gMsgTxState = MSG_TX_WAIT_ACK;
            }
            gUpdateDisplay = true;
            break;

        case MSG_TX_BC_GAP:
            if (gMsgTxCountdown_10ms > 0 && --gMsgTxCountdown_10ms == 0) {
                gMsgTxState = MSG_TX_SENDING;
            }
            break;

        case MSG_TX_WAIT_ACK:
            if (gMsgTxCountdown_10ms > 0 && --gMsgTxCountdown_10ms == 0) {
                if (gMsgTxRetriesLeft > 0) {
                    gMsgTxRetriesLeft--;
                    gMsgTxState = MSG_TX_SENDING;
                } else {
                    gMsgTxState = MSG_TX_FAILED;
                    gMsgResultDisplayCountdown_10ms = 150;
                    AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL);
                }
                gUpdateDisplay = true;
            }
            break;

        case MSG_TX_ACKED:
        case MSG_TX_FAILED:
            if (gMsgUiMode == MSG_UI_SENDING && gMsgResultDisplayCountdown_10ms > 0
                && --gMsgResultDisplayCountdown_10ms == 0) {
                gMsgUiMode     = MSG_UI_INBOX;
                gMsgTxState    = MSG_TX_IDLE;
                gUpdateDisplay = true;
            }
            break;

        default:
            break;
    }
}

uint16_t gMsgRxFrames;
uint16_t gMsgRxSyncFail;
uint16_t gMsgRxCrcFail;
uint16_t gMsgRxPartial;

void MESSAGE_StorePacket(void)
{
    if (gFSKWriteIndex < 36) {
        return;
    }
    gFSKWriteIndex = 0;
    gMsgRxFrames++;
    gUpdateDisplay = true;

    uint16_t Status = BK4819_ReadRegister(BK4819_REG_0B);
    BK4819_PrepareFSKReceive();

    // Doc says bit 4 should be 1 = CRC OK, 0 = CRC FAIL, but original firmware checks for FAIL.
    if ((Status & 0x0010U) != 0 || gMsg_FSK_Buffer[0] != 0xABCD || gMsg_FSK_Buffer[35] != 0xDCBA) {
        gMsgRxSyncFail++;
        return;
    }

    for (unsigned int i = 0; i < 34; i++) {
        gMsg_FSK_Buffer[i + 1] ^= Obfuscation[i % 8];
    }

    uint16_t CRC = CRC_Calculate(&gMsg_FSK_Buffer[1], 2 + 64);
    if (gMsg_FSK_Buffer[34] != CRC) {
        gMsgRxCrcFail++;
        return;
    }

    uint8_t type = gMsg_FSK_Buffer[1] & 0xFF;

    if (type == MSG_TYPE_ACK) {
        MSG_AckPacket_t ack;
        memcpy(&ack, &gMsg_FSK_Buffer[2], sizeof(ack));

        if (gMsgTxState == MSG_TX_WAIT_ACK &&
            ack.ackToID      == gMsgPendingPacket.senderID &&
            ack.ackFromID    == gMsgPendingPacket.receiverID &&
            ack.ackMessageID == gMsgPendingPacket.messageID)
        {
            gMsgTxState = MSG_TX_ACKED;
            gMsgResultDisplayCountdown_10ms = 100;
            gUpdateDisplay = true;
            AUDIO_PlayBeep(BEEP_880HZ_60MS_DOUBLE_BEEP);
        }
        return;
    }

    if (type != MSG_TYPE_DATA_UNICAST && type != MSG_TYPE_DATA_BROADCAST) {
        return;
    }

    MSG_DataPacket_t data;
    memcpy(&data, &gMsg_FSK_Buffer[2], sizeof(data));

    const bool forMe = (type == MSG_TYPE_DATA_BROADCAST) || (data.receiverID == gEeprom.RADIO_ID);
    if (!forMe) {
        return;
    }

    bool isDuplicate = false;
    for (uint8_t i = 0; i < gMsgHistoryCount; i++) {
        if (gMsgHistory[i].senderID == data.senderID && gMsgHistory[i].messageID == data.messageID) {
            isDuplicate = true;
            break;
        }
    }

    if (!isDuplicate) {
        MESSAGE_PushHistory(&data, type == MSG_TYPE_DATA_BROADCAST);
        gUpdateDisplay = true;
        // Not BEEP_880HZ_200MS: with ENABLE_FEAT_F4HWN (always on for this
        // target) that tone has no case of its own in AUDIO_PlayBeep's
        // switches, so it silently falls back to the default 220Hz/500ms
        // tone and honors BEEP_CONTROL like a routine keypress beep. A
        // single 440Hz/500ms tone is unconditionally audible and distinct
        // from the double-beeps used for send results below.
        AUDIO_PlayBeep(BEEP_440HZ_500MS);
    }

    if (type == MSG_TYPE_DATA_UNICAST) {
        // Re-ACK every valid receipt, duplicate or not, in case our previous ACK was lost.
        gMsgPendingAck.ackFromID    = gEeprom.RADIO_ID;
        gMsgPendingAck.ackToID      = data.senderID;
        gMsgPendingAck.ackMessageID = data.messageID;
        memset(gMsgPendingAck.reserved, 0, sizeof(gMsgPendingAck.reserved));
        gMsgPendingAckToSend = true;
    }
}

static void MESSAGE_BeginComposeText(void)
{
    gMsgComposeIndex = 0;
    memset(gMsgComposeText, ' ', MSG_TEXT_MAX);
    gMsgComposeText[MSG_TEXT_MAX] = 0;
    gMsgUiMode = MSG_UI_COMPOSE_TEXT;
}

// Shared by MSG_UI_MYID and MSG_UI_COMPOSE_ID: digit append and
// exit/backspace behave identically in both screens.
static bool MESSAGE_IdEntryDigitOrExit(KEY_Code_t Key)
{
    if (Key <= KEY_9) {
        if (gInputBoxIndex < 5) {
            INPUTBOX_Append(Key);
        }
        return true;
    }
    if (Key == KEY_EXIT) {
        if (gInputBoxIndex == 0) {
            gMsgUiMode = MSG_UI_INBOX;
        } else {
            gInputBox[--gInputBoxIndex] = 10;
        }
        return true;
    }
    return false;
}

// Frequency entry, ported from AIRCOPY_Key_DIGITS (app/aircopy.c): typing 6
// digits validates against the band table / TX lock and applies immediately,
// with no separate confirm key. Unlike AirCopy's dedicated fresh VFO, this
// reprograms the modem afterwards since MESSAGE_Enter() already left it
// running FSK RX on the old frequency.
static void MESSAGE_Key_DIGITS(KEY_Code_t Key)
{
    INPUTBOX_Append(Key);

    if (gInputBoxIndex < 6) {
        return;
    }

    gInputBoxIndex = 0;
    uint32_t Frequency = StrToUL(INPUTBOX_GetAscii()) * 100;

    for (unsigned int i = 0; i < BAND_N_ELEM; i++) {
        if (Frequency < frequencyBandTable[i].lower || Frequency >= frequencyBandTable[i].upper) {
            continue;
        }

        if (TX_freq_check(Frequency)) {
            continue;
        }

        Frequency = FREQUENCY_RoundToStep(Frequency, gRxVfo->StepFrequency);
        gRxVfo->Band = i;
        gRxVfo->freq_config_RX.Frequency = Frequency;
        gRxVfo->freq_config_TX.Frequency = Frequency;
        RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
        gCurrentVfo = gRxVfo;
        RADIO_SetupRegisters(true);
        MESSAGE_SetupModem();
        BK4819_ResetFSK();
        BK4819_PrepareFSKReceive();
        gFSKWriteIndex = 0;
        gMsgUiMode = MSG_UI_INBOX;
        return;
    }
    // Invalid/out-of-band/TX-locked: silently ignored, same as AirCopy --
    // stays on the frequency screen with the digits cleared, ready to retype.
}

void MESSAGE_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (bKeyHeld || !bKeyPressed) {
        return;
    }

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gRequestDisplayScreen = DISPLAY_MESSAGE;

    switch (gMsgUiMode) {
        case MSG_UI_INBOX:
            switch (Key) {
                case KEY_UP:
                    if (gMsgHistoryCursor > 0) {
                        gMsgHistoryCursor--;
                    }
                    break;

                case KEY_DOWN:
                    if (gMsgHistoryCount > 0 && gMsgHistoryCursor + 1 < gMsgHistoryCount) {
                        gMsgHistoryCursor++;
                    }
                    break;

                case KEY_MENU:
                    gInputBoxIndex       = 0;
                    gMsgComposeBroadcast = false;
                    gMsgUiMode           = MSG_UI_COMPOSE_ID;
                    break;

                case KEY_STAR:
                    gInputBoxIndex = 0;
                    gMsgUiMode     = MSG_UI_MYID;
                    break;

                case KEY_F:
                    if (gMsgHistoryCount > 0) {
                        gMsgReadScroll = 0;
                        gMsgUiMode     = MSG_UI_READ;
                    } else {
                        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                    }
                    break;

                case KEY_EXIT:
                    MESSAGE_Exit();
                    GUI_SelectNextDisplay(DISPLAY_MAIN);
                    gRequestDisplayScreen = DISPLAY_INVALID;
                    return;

                default:
                    if (Key <= KEY_9) {
                        gInputBoxIndex = 0;
                        gMsgUiMode     = MSG_UI_SET_FREQ;
                        MESSAGE_Key_DIGITS(Key);
                    }
                    break;
            }
            break;

        case MSG_UI_MYID:
            if (!MESSAGE_IdEntryDigitOrExit(Key) && Key == KEY_MENU) {
                uint32_t id = StrToUL(INPUTBOX_GetAscii());
                if (id >= 1 && id <= 0xFFFE) {
                    gEeprom.RADIO_ID = (uint16_t)id;
                    SETTINGS_SaveRadioID();
                    gMsgUiMode = MSG_UI_INBOX;
                } else {
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                }
            }
            break;

        case MSG_UI_COMPOSE_ID:
            if (MESSAGE_IdEntryDigitOrExit(Key)) {
                break;
            }
            if (Key == KEY_STAR) {
                gMsgComposeBroadcast = true;
                MESSAGE_BeginComposeText();
            } else if (Key == KEY_MENU) {
                uint32_t id = StrToUL(INPUTBOX_GetAscii());
                if (id >= 1 && id <= 0xFFFE) {
                    gMsgComposeDestID    = id;
                    gMsgComposeBroadcast = false;
                    MESSAGE_BeginComposeText();
                } else {
                    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
                }
            }
            break;

        case MSG_UI_COMPOSE_TEXT:
            if (Key <= KEY_9) {
                if (gMsgComposeIndex < MSG_TEXT_MAX) {
                    gMsgComposeText[gMsgComposeIndex++] = '0' + Key - KEY_0;
                }
            } else if (Key == KEY_STAR) {
                if (gMsgComposeIndex < MSG_TEXT_MAX) {
                    gMsgComposeText[gMsgComposeIndex++] = '-';
                }
            } else if (Key == KEY_F) {
                // "next" key: advances the cursor without forcing a space,
                // used to accept a character picked with UP/DOWN.
                if (gMsgComposeIndex < MSG_TEXT_MAX) {
                    gMsgComposeIndex++;
                }
            } else if (Key == KEY_UP || Key == KEY_DOWN) {
                if (gMsgComposeIndex < MSG_TEXT_MAX) {
                    static const char unwanted[] = "$%&!\"':;?^`|{}";
                    const int8_t direction = (Key == KEY_UP) ? 1 : -1;
                    char c = gMsgComposeText[gMsgComposeIndex] + direction;
                    unsigned int i = 0;
                    while (i < sizeof(unwanted) - 1 && c >= 32 && c <= 126) {
                        if (c == unwanted[i++]) {
                            c += direction;
                            i = 0;
                        }
                    }
                    gMsgComposeText[gMsgComposeIndex] = (c < 32) ? 126 : (c > 126) ? 32 : c;
                }
            } else if (Key == KEY_EXIT) {
                if (gMsgComposeIndex == 0) {
                    gMsgUiMode = gMsgComposeBroadcast ? MSG_UI_INBOX : MSG_UI_COMPOSE_ID;
                } else {
                    gMsgComposeIndex--;
                }
            } else if (Key == KEY_MENU) {
                if (gMsgComposeIndex > 0) {
                    MESSAGE_BeginSend((uint16_t)gMsgComposeDestID, gMsgComposeBroadcast,
                                       gMsgComposeText, (uint8_t)gMsgComposeIndex);
                    gMsgUiMode = MSG_UI_SENDING;
                }
            }
            break;

        case MSG_UI_SENDING:
            if (Key == KEY_EXIT) {
                gMsgTxState = MSG_TX_IDLE;
                gMsgUiMode  = MSG_UI_INBOX;
            }
            break;

        case MSG_UI_SET_FREQ:
            if (Key <= KEY_9) {
                MESSAGE_Key_DIGITS(Key);
            } else if (Key == KEY_EXIT) {
                if (gInputBoxIndex == 0) {
                    gMsgUiMode = MSG_UI_INBOX;
                } else {
                    gInputBox[--gInputBoxIndex] = 10;
                }
            }
            break;

        case MSG_UI_READ:
            if (Key == KEY_UP) {
                if (gMsgReadScroll >= 16) {
                    gMsgReadScroll -= 16;
                } else {
                    gMsgReadScroll = 0;
                }
            } else if (Key == KEY_DOWN) {
                if (gMsgHistoryCursor < gMsgHistoryCount) {
                    const uint8_t textLen = gMsgHistory[gMsgHistoryCursor].textLen;
                    if (gMsgReadScroll + 48 < textLen) {
                        gMsgReadScroll += 16;
                    }
                }
            } else if (Key == KEY_STAR) {
                // Delete the message being read: shift everything after it
                // down by one slot and drop back to the inbox.
                if (gMsgHistoryCursor < gMsgHistoryCount) {
                    for (uint8_t i = gMsgHistoryCursor; i + 1 < gMsgHistoryCount; i++) {
                        gMsgHistory[i] = gMsgHistory[i + 1];
                    }
                    gMsgHistoryCount--;
                    if (gMsgHistoryCursor > 0 && gMsgHistoryCursor >= gMsgHistoryCount) {
                        gMsgHistoryCursor--;
                    }
                }
                gMsgUiMode = MSG_UI_INBOX;
            } else if (Key == KEY_EXIT) {
                gMsgUiMode = MSG_UI_INBOX;
            }
            break;
    }
}

#endif
