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
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/message.h"

// UI_PrintStringSmallNormal()/UI_PrintString() do not clip to [Start,End] --
// a string much longer than the available pixel width makes the centering
// math underflow and wrap, corrupting gFrameBuffer past the row. Anything
// of unbounded/user-controlled length (received message text) must be
// truncated to a safe width (~16 chars in a 2..127 span) before printing.
static void PrintTruncatedLine(const char *text, unsigned int len, uint8_t line)
{
    char buf[17];
    unsigned int n = (len > 16) ? 16 : len;
    memcpy(buf, text, n);
    buf[n] = 0;
    UI_PrintStringSmallNormal(buf, 2, 127, line);
}

void UI_DisplayMessage(void)
{
    char String[32] = { 0 };

    UI_DisplayClear();

    switch (gMsgUiMode) {
        case MSG_UI_INBOX: {
            UI_PrintString("MESSAGES", 2, 127, 0, 8);

            if (gMsgPagedBySenderID != 0) {
                // MESSAGE_HandleDtmfDigit() (app/message.c) just auto-switched
                // us here from a DTMF page -- the actual message is still in
                // flight over FSK, so say who's about to send it.
                sprintf(String, "PAGED BY %u", gMsgPagedBySenderID);
                UI_PrintStringSmallNormal(String, 2, 127, 2);
            } else if (gMsgHistoryCount == 0) {
                UI_PrintString("NO MESSAGES", 2, 127, 3, 8);
            } else {
                const MSG_HistoryEntry_t *e = &gMsgHistory[gMsgHistoryCursor];
                sprintf(String, "%s %u  %u/%u", e->wasBroadcast ? "ALL<-" : "FROM",
                        e->senderID, gMsgHistoryCursor + 1, gMsgHistoryCount);
                UI_PrintStringSmallNormal(String, 2, 127, 2);

                PrintTruncatedLine(e->text, e->textLen, 3);
                if (e->textLen > 16) {
                    PrintTruncatedLine(e->text + 16, e->textLen - 16, 4);
                }
            }

            // Live signal strength + capture activity, so you can tell a
            // real over-the-air attempt from unrelated noise: RSSI moves
            // with any RF energy, "RX.." only lights up while the FSK
            // correlator is actively mid-capture on a frame right now.
            sprintf(String, "%ddBm F%u%s", BK4819_GetRSSI_dBm(), gMsgRxFrames % 100,
                    (gFSKWriteIndex > 0) ? " RX.." : "");
            UI_PrintStringSmallNormal(String, 2, 127, 5);

            sprintf(String, "ID:%u", gEeprom.RADIO_ID);
            UI_PrintStringSmallNormal(String, 2, 60, 6);
            UI_PrintStringSmallNormal("M:NEW *:ID", 62, 127, 6);
            break;
        }

        case MSG_UI_MYID:
            UI_PrintString("MY ID", 2, 127, 0, 8);
            UI_PrintString(INPUTBOX_GetAscii(), 2, 127, 3, 8);
            UI_PrintStringSmallNormal("0-9 THEN MENU", 2, 127, 6);
            break;

        case MSG_UI_COMPOSE_ID:
            UI_PrintString("TO ID?", 2, 127, 0, 8);
            UI_PrintString(INPUTBOX_GetAscii(), 2, 127, 3, 8);
            UI_PrintStringSmallNormal("*=BROADCAST", 2, 127, 6);
            break;

        case MSG_UI_COMPOSE_TEXT: {
            if (gMsgComposeBroadcast) {
                strcpy(String, "MSG TO ALL");
            } else {
                sprintf(String, "MSG TO %u", (unsigned int)gMsgComposeDestID);
            }
            UI_PrintString(String, 2, 127, 0, 8);

            const unsigned int winStart = (gMsgComposeIndex > 31) ? (gMsgComposeIndex - 31) : 0;

            char line[17] = { 0 };
            memcpy(line, &gMsgComposeText[winStart], 16);
            UI_PrintStringSmallNormal(line, 2, 127, 3);

            memset(line, 0, sizeof(line));
            const unsigned int secondStart = winStart + 16;
            if (secondStart < MSG_TEXT_MAX) {
                unsigned int n = MSG_TEXT_MAX - secondStart;
                if (n > 16) {
                    n = 16;
                }
                memcpy(line, &gMsgComposeText[secondStart], n);
            }
            UI_PrintStringSmallNormal(line, 2, 127, 4);

            sprintf(String, "%u/%u %s", gMsgComposeIndex, MSG_TEXT_MAX,
                    gMsgInputMode == MSG_INPUT_UPPER ? "ABC" :
                    gMsgInputMode == MSG_INPUT_LOWER ? "abc" : "123");
            UI_PrintStringSmallNormal(String, 2, 127, 6);
            break;
        }

        case MSG_UI_SENDING:
            if (gMsgTxState == MSG_TX_ACKED) {
                UI_PrintString(gMsgComposeBroadcast ? "SENT" : "DELIVERED", 2, 127, 3, 8);
            } else if (gMsgTxState == MSG_TX_FAILED) {
                UI_PrintString("NO REPLY", 2, 127, 3, 8);
            } else {
                UI_PrintString("SENDING...", 2, 127, 3, 8);
                // Unicast retries now span ~1 minute (paging needs time for a
                // human to react) -- without this it just looks hung.
                if (!gMsgComposeBroadcast) {
                    sprintf(String, "RETRIES LEFT %u", gMsgTxRetriesLeft);
                    UI_PrintStringSmallNormal(String, 2, 127, 5);
                }
            }
            break;

        // Same big-digit frequency layout as ui/aircopy.c's UI_DisplayAircopy():
        // gInputBoxIndex == 0 shows the live VFO frequency, otherwise the
        // digits typed so far.
        case MSG_UI_SET_FREQ:
            UI_PrintString("FREQUENCY", 2, 127, 0, 8);

            if (gInputBoxIndex == 0) {
                uint32_t frequency = gRxVfo->freq_config_RX.Frequency;
                sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
                UI_PrintStringSmallNormal(String + 7, 97, 0, 3);
                String[7] = 0;
                UI_DisplayFrequency(String, 16, 2, false);
            } else {
                const char *ascii = INPUTBOX_GetAscii();
                sprintf(String, "%.3s.%.3s", ascii, ascii + 3);
                UI_DisplayFrequency(String, 16, 2, false);
            }

            UI_PrintStringSmallNormal("0-9=SET EXIT=BACK", 2, 127, 6);
            break;

        case MSG_UI_READ: {
            const MSG_HistoryEntry_t *e = &gMsgHistory[gMsgHistoryCursor];

            sprintf(String, "%s %u", e->wasBroadcast ? "ALL<-" : "FROM", e->senderID);
            UI_PrintString(String, 2, 127, 0, 8);

            const unsigned int shown = (e->textLen > gMsgReadScroll) ? (e->textLen - gMsgReadScroll) : 0;
            const unsigned int last  = (shown > 48) ? 48 : shown;
            sprintf(String, "%u-%u/%u", gMsgReadScroll + 1, gMsgReadScroll + last, e->textLen);
            UI_PrintStringSmallNormal(String, 2, 127, 2);

            PrintTruncatedLine(e->text + gMsgReadScroll, shown, 3);
            if (shown > 16) {
                PrintTruncatedLine(e->text + gMsgReadScroll + 16, shown - 16, 4);
            }
            if (shown > 32) {
                PrintTruncatedLine(e->text + gMsgReadScroll + 32, shown - 32, 5);
            }

            UI_PrintStringSmallNormal("*=DEL EXIT=BACK", 2, 127, 6);
            break;
        }
    }

    ST7565_BlitFullScreen();
}

#endif
