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

#ifndef APP_MESSAGE_H
#define APP_MESSAGE_H

#ifdef ENABLE_FEAT_F4HWN_MESSAGE

#include <stdbool.h>
#include <stdint.h>

#include "driver/keyboard.h"

#define MSG_TYPE_DATA_UNICAST     0x01u
#define MSG_TYPE_DATA_BROADCAST   0x02u
#define MSG_TYPE_ACK              0x03u

#define MSG_BROADCAST_ID          0xFFFFu
#define MSG_TEXT_MAX              58u
#define MSG_HISTORY_SIZE          5u
// Unicast retries: a DTMF page (MESSAGE_SendPage()) now precedes every unicast
// send, and the receiver needs real wall-clock time to notice it and
// auto-switch into Msg mode -- a human, not a radio that's already listening.
// 20 retries * ~3s ACK timeout each keeps the burst going for about a minute
// instead of the ~2.4s that's only enough for an already-armed receiver.
#define MSG_TX_MAX_RETRIES        20u
#define MSG_ACK_TIMEOUT_10MS      300u
#define MSG_BROADCAST_REPEAT_10MS 30u
#define MSG_MULTITAP_TIMEOUT_10MS 90u
// DTMF page burst, sent once per outgoing unicast message ahead of the FSK
// data: "AD" + 5-digit destID + 5-digit senderID, zero-padded. 'A'/'D' are
// deliberately used as the marker -- real handheld mic keypads have no A-D
// buttons, so this can't collide with anything a human actually dials.
#define MSG_PAGE_MARKER           "AD"
#define MSG_PAGE_LEN              12u
// Fixed rather than gEeprom.DTMF_CODE_PERSIST_TIME/DTMF_CODE_INTERVAL_TIME:
// those default to 100ms/100ms and aren't exposed anywhere in this fork's
// menu to retune, and 100ms proved too short to decode reliably on the
// bench (only the mute/unmute click at each digit boundary came through).
#define MSG_PAGE_TONE_MS          100u
#define MSG_PAGE_GAP_MS           100u

typedef struct __attribute__((packed)) {
    uint16_t senderID;
    uint16_t receiverID;   // MSG_BROADCAST_ID for broadcast
    uint8_t  messageID;    // wraps 0-255, constant across retries of the same logical message
    uint8_t  textLen;      // 0..MSG_TEXT_MAX
    uint8_t  text[MSG_TEXT_MAX];
} MSG_DataPacket_t;        // 2+2+1+1+58 = 64 bytes

typedef struct __attribute__((packed)) {
    uint16_t ackFromID;    // = original receiverID (who is ACKing)
    uint16_t ackToID;      // = original senderID (recipient of this ACK)
    uint8_t  ackMessageID; // echoes the messageID being acknowledged
    uint8_t  reserved[59];
} MSG_AckPacket_t;         // 2+2+1+59 = 64 bytes

typedef struct {
    uint16_t senderID;
    uint8_t  messageID;
    uint8_t  textLen;
    char     text[MSG_TEXT_MAX + 1];
    bool     wasBroadcast;
} MSG_HistoryEntry_t;

enum MSG_UiMode_t {
    MSG_UI_INBOX = 0,
    MSG_UI_MYID,
    MSG_UI_COMPOSE_ID,
    MSG_UI_COMPOSE_TEXT,
    MSG_UI_SENDING,
    MSG_UI_SET_FREQ,
    MSG_UI_READ,
};
typedef enum MSG_UiMode_t MSG_UiMode_t;

enum MSG_InputMode_t {
    MSG_INPUT_UPPER = 0,
    MSG_INPUT_LOWER,
    MSG_INPUT_DIGIT,
};
typedef enum MSG_InputMode_t MSG_InputMode_t;

enum MSG_TxState_t {
    MSG_TX_IDLE = 0,
    MSG_TX_SENDING,
    MSG_TX_BC_GAP,
    MSG_TX_WAIT_ACK,
    MSG_TX_ACKED,
    MSG_TX_FAILED,
};
typedef enum MSG_TxState_t MSG_TxState_t;

extern uint16_t           gMsg_FSK_Buffer[36];

extern MSG_UiMode_t       gMsgUiMode;
extern MSG_TxState_t      gMsgTxState;
extern uint8_t            gMsgTxRetriesLeft;  // shown on MSG_UI_SENDING so a ~1min wait doesn't look frozen

extern MSG_HistoryEntry_t gMsgHistory[MSG_HISTORY_SIZE];
extern uint8_t            gMsgHistoryCount;
extern uint8_t            gMsgHistoryCursor;
extern unsigned int       gMsgReadScroll;  // 16-char scroll offset into the message being read

// RX diagnostics: gMsgRxFrames counts every complete 72-byte over-the-air
// capture; gMsgRxSyncFail counts ones that failed the hardware CRC bit or
// sync-word check; gMsgRxCrcFail counts ones that passed sync but failed
// our software CRC. Shown on the inbox screen to help diagnose reception
// issues (e.g. stuck at 0 means the FSK correlator never locks at all).
extern uint16_t           gMsgRxFrames;
extern uint16_t           gMsgRxSyncFail;
extern uint16_t           gMsgRxCrcFail;
extern uint16_t           gMsgRxPartial;  // frames that started arriving but timed out incomplete

extern char               gMsgComposeText[MSG_TEXT_MAX + 1];
extern unsigned int       gMsgComposeIndex;
extern uint32_t           gMsgComposeDestID;
extern bool               gMsgComposeBroadcast;
extern MSG_InputMode_t    gMsgInputMode;  // multi-tap mode; shown in the compose footer

// Set by MESSAGE_HandleDtmfDigit() when a DTMF page addressed to us just
// auto-switched the display into Msg mode; 0 = no page pending display.
// Cleared automatically by MESSAGE_TimeSlice10ms() after a few seconds.
extern uint16_t           gMsgPagedBySenderID;

void MESSAGE_Enter(void);
void MESSAGE_Exit(void);
// Re-applies the FSK-RX modem config. AUDIO_PlayBeep() drives the BK4819
// tone generator through registers MESSAGE_Enter() repurposed for FSK, and
// only restores one of them -- called by AUDIO_PlayBeep() (audio.c) itself
// after any beep played while DISPLAY_MESSAGE is active, so a beep never
// leaves the radio deaf to further frames/ACKs.
void MESSAGE_RearmModem(void);
void MESSAGE_TimeSlice10ms(void);
void MESSAGE_StorePacket(void);
void MESSAGE_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld);
// Fed one decoded DTMF character at a time from app/app.c's CheckRadioInterrupts(),
// independent of ENABLE_DTMF_CALLING/gSetting_live_DTMF_decoder so paging works
// with just this feature compiled in. Matches a trailing MSG_PAGE_MARKER + 10
// digit run against a rolling buffer; on a full match addressed to our own
// Radio ID it auto-switches into Msg mode with no user action.
void MESSAGE_HandleDtmfDigit(char c);

#endif

#endif
