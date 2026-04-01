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

#include <string.h>
#include <stdlib.h>  // abs()

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#ifdef ENABLE_AM_FIX
    #include "am_fix.h"
#endif
#include "bitmaps.h"
#include "board.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/ui.h"
#include "audio.h"

#ifdef ENABLE_FEAT_F4HWN
    #include "driver/system.h"
#endif

center_line_t center_line = CENTER_LINE_NONE;

#ifdef ENABLE_FEAT_F4HWN
    static int8_t RxBlink;
    static int8_t RxBlinkLed = 0;
    static int8_t RxBlinkLedCounter;
    static int8_t RxLine;
    static uint32_t RxOnVfofrequency;

    bool isMainOnlyInputDTMF = false;

    static bool isMainOnly()
    {
        return (gEeprom.DUAL_WATCH == DUAL_WATCH_OFF) && (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF);
    }
#endif

const int8_t dBmCorrTable[7] = {
            -15, // band 1
            -25, // band 2
            -20, // band 3
            -4, // band 4
            -7, // band 5
            -6, // band 6
             -1  // band 7
        };

const char *VfoStateStr[] = {
       [VFO_STATE_NORMAL]="",
       [VFO_STATE_BUSY]="BUSY",
       [VFO_STATE_BAT_LOW]="BAT LOW",
       [VFO_STATE_TX_DISABLE]="TX DISABLE",
       [VFO_STATE_TIMEOUT]="TIMEOUT",
       [VFO_STATE_ALARM]="ALARM",
       [VFO_STATE_VOLTAGE_HIGH]="VOLT HIGH"
};

// ----------------------------------------

static void DrawSmallAntennaAndBars(uint8_t *p, unsigned int level)
{
    if(level>6)
        level = 6;

    memcpy(p, BITMAP_Antenna, ARRAY_SIZE(BITMAP_Antenna));

    for(uint8_t i = 1; i <= level; i++) {
        char bar = (0xff << (6-i)) & 0x7F;
        memset(p + 2 + i*3, bar, 2);
    }
}
#if defined ENABLE_AUDIO_BAR || defined ENABLE_RSSI_BAR

static void DrawLevelBar(uint8_t xpos, uint8_t line, uint8_t level, uint8_t bars)
{
#ifndef ENABLE_FEAT_F4HWN
    const char hollowBar[] = {
        0b01111111,
        0b01000001,
        0b01000001,
        0b01111111
    };
#endif
    
    uint8_t *p_line = gFrameBuffer[line];
    level = MIN(level, bars);

    for(uint8_t i = 0; i < level; i++) {
#ifdef ENABLE_FEAT_F4HWN
        if(gSetting_set_met)
        {
            const char hollowBar[] = {
                0b01111111,
                0b01000001,
                0b01000001,
                0b01111111
            };

            if(i < bars - 4) {
                for(uint8_t j = 0; j < 4; j++)
                    p_line[xpos + i * 5 + j] = (~(0x7F >> (i + 1))) & 0x7F;
            }
            else {
                memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
            }
        }
        else
        {
            const char hollowBar[] = {
                0b00111110,
                0b00100010,
                0b00100010,
                0b00111110
            };

            const char simpleBar[] = {
                0b00111110,
                0b00111110,
                0b00111110,
                0b00111110
            };

            if(i < bars - 4) {
                memcpy(p_line + (xpos + i * 5), &simpleBar, ARRAY_SIZE(simpleBar));
            }
            else {
                memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
            }
        }
#else
        if(i < bars - 4) {
            for(uint8_t j = 0; j < 4; j++)
                p_line[xpos + i * 5 + j] = (~(0x7F >> (i+1))) & 0x7F;
        }
        else {
            memcpy(p_line + (xpos + i * 5), &hollowBar, ARRAY_SIZE(hollowBar));
        }
#endif
    }
}
#endif

#ifdef ENABLE_AUDIO_BAR

// Approximation of a logarithmic scale using integer arithmetic
uint8_t log2_approx(unsigned int value) {
    uint8_t log = 0;
    while (value >>= 1) {
        log++;
    }
    return log;
}

void UI_DisplayAudioBar(void)
{
    if (gSetting_mic_bar)
    {
        if(gLowBattery && !gLowBatteryConfirmed)
            return;

#ifdef ENABLE_FEAT_F4HWN
        RxBlinkLed = 0;
        RxBlinkLedCounter = 0;
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
        unsigned int line;
        if (isMainOnly())
        {
            line = 5;
        }
        else
        {
            line = 3;
        }
#else
        const unsigned int line = 3;
#endif

        if (gCurrentFunction != FUNCTION_TRANSMIT ||
            gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
            || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
            )
        {
            return;  // screen is in use
        }

#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
        if (gAlarmState != ALARM_STATE_OFF)
            return;
#endif
        static uint8_t barsOld = 0;
        const uint8_t thresold = 18; // arbitrary thresold
        //const uint8_t barsList[] = {0, 0, 0, 1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20, 25, 25};
        const uint8_t barsList[] = {0, 0, 0, 1, 2, 3, 5, 7, 9, 12, 15, 18, 21, 25, 25, 25};
        uint8_t logLevel;
        uint8_t bars;

        unsigned int voiceLevel  = BK4819_GetVoiceAmplitudeOut();  // 15:0

        voiceLevel = (voiceLevel >= thresold) ? (voiceLevel - thresold) : 0;
        logLevel = log2_approx(MIN(voiceLevel * 16, 32768u) + 1);
        bars = barsList[logLevel];
        barsOld = (barsOld - bars > 1) ? (barsOld - 1) : bars;

        uint8_t *p_line = gFrameBuffer[line];
        memset(p_line, 0, LCD_WIDTH);

        DrawLevelBar(2, line, barsOld, 25);

        if (gCurrentFunction == FUNCTION_TRANSMIT)
            ST7565_BlitFullScreen();
    }
}
#endif

void DisplayRSSIBar(const bool now)
{
#if defined(ENABLE_RSSI_BAR)

    const unsigned int txt_width    = 7 * 8;                 // 8 text chars
    const unsigned int bar_x        = 2 + txt_width + 4;     // X coord of bar graph

#ifdef ENABLE_FEAT_F4HWN
    /*
    const char empty[] = {
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
    };
    */

    unsigned int line;
    if (isMainOnly())
    {
        line = 5;
    }
    else
    {
        line = 3;
    }

    //char rx[4];
    //sprintf(String, "%d", RxBlink);
    //UI_PrintStringSmallBold(String, 80, 0, RxLine);

    if(RxLine >= 0 && center_line != CENTER_LINE_IN_USE)
    {
        if (RxBlink == 0 || RxBlink == 1) {
            UI_PrintStringSmallBold("RX", 8, 0, RxLine);
            if (RxBlink == 1) RxBlink = 2;
        } else {
            for (uint8_t i = 8; i < 24; i++)
            {
                gFrameBuffer[RxLine][i] = 0x00;
            }
            RxBlink = 1;
        }
        ST7565_BlitLine(RxLine);
    }
#else
    const unsigned int line = 3;
#endif
    uint8_t           *p_line        = gFrameBuffer[line];
    char               str[16];

#ifndef ENABLE_FEAT_F4HWN
    const char plus[] = {
        0b00011000,
        0b00011000,
        0b01111110,
        0b01111110,
        0b01111110,
        0b00011000,
        0b00011000,
    };
#endif

    if ((gEeprom.KEY_LOCK && gKeypadLocked > 0) || center_line != CENTER_LINE_RSSI)
        return;     // display is in use

    if (gCurrentFunction == FUNCTION_TRANSMIT ||
        gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
        || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
        )
        return;     // display is in use

    if (now)
        memset(p_line, 0, LCD_WIDTH);

#ifdef ENABLE_FEAT_F4HWN
    int16_t rssi_dBm =
        BK4819_GetRSSI_dBm()
#ifdef ENABLE_AM_FIX
        + ((gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM) ? AM_fix_get_gain_diff() : 0)
#endif
        + dBmCorrTable[gRxVfo->Band];

    // IARU VHF/UHF S-meter: S9 = -93 dBm, 1 S-unit = 6 dB
    // S(n) threshold = -93 + (n - 9) * 6
    uint8_t s_level    = 0;
    uint8_t overS9dBm  = 0;
    uint8_t overS9Bars = 0;

    // if      (rssi_dBm >= -93)  s_level = 9;  // S9  = -93 dBm
    // else if (rssi_dBm >= -99)  s_level = 8;  // S8  = -99 dBm
    // else if (rssi_dBm >= -105) s_level = 7;  // S7  = -105 dBm
    // else if (rssi_dBm >= -111) s_level = 6;  // S6  = -111 dBm
    // else if (rssi_dBm >= -117) s_level = 5;  // S5  = -117 dBm
    // else if (rssi_dBm >= -123) s_level = 4;  // S4  = -123 dBm
    // else if (rssi_dBm >= -129) s_level = 3;  // S3  = -129 dBm
    // else if (rssi_dBm >= -135) s_level = 2;  // S2  = -135 dBm
    // else if (rssi_dBm >= -141) s_level = 1;  // S1  = -141 dBm
    // else                       s_level = 0;  // S0 (below -141 dBm)

    if (rssi_dBm >= -93)
        s_level = 9;
    else if (rssi_dBm < -141)
        s_level = 0;
    else 
        s_level = (rssi_dBm + 147) / 6;

    if (s_level == 9) {
        // Compute over-S9 dB directly
        overS9dBm  = (uint8_t)MIN(rssi_dBm - (-93), 40);
        overS9Bars = overS9dBm / 10;
    }
#else
    const int16_t s0_dBm   = -gEeprom.S0_LEVEL;                  // S0 .. base level
    const int16_t rssi_dBm =
        BK4819_GetRSSI_dBm()
#ifdef ENABLE_AM_FIX
        + ((gSetting_AM_fix && gRxVfo->Modulation == MODULATION_AM) ? AM_fix_get_gain_diff() : 0)
#endif
        + dBmCorrTable[gRxVfo->Band];

    int s0_9 = gEeprom.S0_LEVEL - gEeprom.S9_LEVEL;
    const uint8_t s_level = MIN(MAX((int32_t)(rssi_dBm - s0_dBm)*100 / (s0_9*100/9), 0), 9); // S0 - S9
    uint8_t overS9dBm = MIN(MAX(rssi_dBm + gEeprom.S9_LEVEL, 0), 99);
    uint8_t overS9Bars = MIN(overS9dBm/10, 4);
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (gSetting_set_gui)
    {
        sprintf(str, "%3d", rssi_dBm);
        UI_PrintStringSmallNormal(str, LCD_WIDTH + 8, 0, line - 1);
    }
    else
    {
        sprintf(str, "% 4d %s", rssi_dBm, "dBm");
        if(isMainOnly())
            GUI_DisplaySmallest(str, 2, 41, false, true);
        else
            GUI_DisplaySmallest(str, 2, 25, false, true);
    }

    if(overS9Bars == 0) {
        sprintf(str, "S%d", s_level);
    }
    else {
        sprintf(str, "+%02d", overS9dBm);
    }

    UI_PrintStringSmallNormal(str, LCD_WIDTH + 38, 0, line - 1);
#else
    if(overS9Bars == 0) {
        sprintf(str, "% 4d S%d", -rssi_dBm, s_level);
    }
    else {
        sprintf(str, "% 4d  %2d", -rssi_dBm, overS9dBm);
        memcpy(p_line + 2 + 7*5, &plus, ARRAY_SIZE(plus));
    }

    UI_PrintStringSmallNormal(str, 2, 0, line);
#endif
    DrawLevelBar(bar_x, line, s_level + overS9Bars, 13);
    if (now)
        ST7565_BlitLine(line);
#else
    int16_t rssi = BK4819_GetRSSI();
    uint8_t Level;

    if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][3]) {
        Level = 6;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][2]) {
        Level = 4;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][1]) {
        Level = 2;
    } else if (rssi >= gEEPROM_RSSI_CALIB[gRxVfo->Band][0]) {
        Level = 1;
    } else {
        Level = 0;
    }

    uint8_t *pLine = (gEeprom.RX_VFO == 0)? gFrameBuffer[2] : gFrameBuffer[6];
    if (now)
        memset(pLine, 0, 23);
    DrawSmallAntennaAndBars(pLine, Level);
    if (now)
        ST7565_BlitFullScreen();
#endif

}

#ifdef ENABLE_AGC_SHOW_DATA
void UI_MAIN_PrintAGC(bool now)
{
    char buf[20];
    memset(gFrameBuffer[3], 0, 128);
    union {
        struct {
            uint16_t _ : 5;
            uint16_t agcSigStrength : 7;
            int16_t gainIdx : 3;
            uint16_t agcEnab : 1;
        };
        uint16_t __raw;
    } reg7e;
    reg7e.__raw = BK4819_ReadRegister(0x7E);
    uint8_t gainAddr = reg7e.gainIdx < 0 ? 0x14 : 0x10 + reg7e.gainIdx;
    union {
        struct {
            uint16_t pga:3;
            uint16_t mixer:2;
            uint16_t lna:3;
            uint16_t lnaS:2;
        };
        uint16_t __raw;
    } agcGainReg;
    agcGainReg.__raw = BK4819_ReadRegister(gainAddr);
    int8_t lnaShortTab[] = {-28, -24, -19, 0};
    int8_t lnaTab[] = {-24, -19, -14, -9, -6, -4, -2, 0};
    int8_t mixerTab[] = {-8, -6, -3, 0};
    int8_t pgaTab[] = {-33, -27, -21, -15, -9, -6, -3, 0};
    int16_t agcGain = lnaShortTab[agcGainReg.lnaS] + lnaTab[agcGainReg.lna] + mixerTab[agcGainReg.mixer] + pgaTab[agcGainReg.pga];

    sprintf(buf, "%d%2d %2d %2d %3d", reg7e.agcEnab, reg7e.gainIdx, -agcGain, reg7e.agcSigStrength, BK4819_GetRSSI());
    UI_PrintStringSmallNormal(buf, 2, 0, 3);
    if(now)
        ST7565_BlitLine(3);
}
#endif

void UI_MAIN_TimeSlice500ms(void)
{
    if(gScreenToDisplay==DISPLAY_MAIN) {
#ifdef ENABLE_AGC_SHOW_DATA
        UI_MAIN_PrintAGC(true);
        return;
#endif

        if(FUNCTION_IsRx()) {
            DisplayRSSIBar(true);
        }
#ifdef ENABLE_FEAT_F4HWN // Blink Green Led for white...
        else if(gSetting_set_eot > 0 && RxBlinkLed == 2)
        {
            if(RxBlinkLedCounter <= 8)
            {
                if(RxBlinkLedCounter % 2 == 0)
                {
                    if(gSetting_set_eot > 1 )
                    {
                        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
                    }
                }
                else
                {
                    if(gSetting_set_eot > 1 )
                    {
                        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
                    }

                    if(gSetting_set_eot == 1 || gSetting_set_eot == 3)
                    {
                        switch(RxBlinkLedCounter)
                        {
                            case 1:
                            AUDIO_PlayBeep(BEEP_400HZ_30MS);
                            break;

                            case 3:
                            AUDIO_PlayBeep(BEEP_400HZ_30MS);
                            break;

                            case 5:
                            AUDIO_PlayBeep(BEEP_500HZ_30MS);
                            break;

                            case 7:
                            AUDIO_PlayBeep(BEEP_600HZ_30MS);
                            break;
                        }
                    }
                }
                RxBlinkLedCounter += 1;
            }
            else
            {
                RxBlinkLed = 0;
            }
        }
#endif
    }
}

// ----------------------------------------

void UI_DisplayMain(void)
{
    char               String[22];

    center_line = CENTER_LINE_NONE;

    // clear the screen
    UI_DisplayClear();

    if(gLowBattery && !gLowBatteryConfirmed) {
        UI_DisplayPopup("LOW BATTERY");
        ST7565_BlitFullScreen();
        return;
    }

#ifndef ENABLE_FEAT_F4HWN
    if (gEeprom.KEY_LOCK && gKeypadLocked > 0)
    {   // tell user how to unlock the keyboard
        UI_PrintString("Long press #", 0, LCD_WIDTH, 1, 8);
        UI_PrintString("to unlock",    0, LCD_WIDTH, 3, 8);
        ST7565_BlitFullScreen();
        return;
    }
#else
    UI_DisplayUnlockKeyboard(isMainOnly() ? 5 : 3);
#endif

    unsigned int activeTxVFO = gRxVfoIsActive ? gEeprom.RX_VFO : gEeprom.TX_VFO;

    for (unsigned int vfo_num = 0; vfo_num < 2; vfo_num++)
    {
#ifdef ENABLE_FEAT_F4HWN
        const unsigned int line0 = 0;  // text screen line
        const unsigned int line1 = 4;
        unsigned int line;
        if (isMainOnly())
        {
            line       = 0;
        }
        else
        {
            line       = (vfo_num == 0) ? line0 : line1;
        }
        const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
        uint8_t           *p_line0    = gFrameBuffer[line + 0];
        uint8_t           *p_line1    = gFrameBuffer[line + 1];
        enum Vfo_txtr_mode mode       = VFO_MODE_NONE;      
#else
        const unsigned int line0 = 0;  // text screen line
        const unsigned int line1 = 4;
        const unsigned int line       = (vfo_num == 0) ? line0 : line1;
        const bool         isMainVFO  = (vfo_num == gEeprom.TX_VFO);
        uint8_t           *p_line0    = gFrameBuffer[line + 0];
        uint8_t           *p_line1    = gFrameBuffer[line + 1];
        enum Vfo_txtr_mode mode       = VFO_MODE_NONE;
#endif

#ifdef ENABLE_FEAT_F4HWN
    if (isMainOnly())
    {
        if (activeTxVFO != vfo_num)
        {
            continue;
        }
    }
#endif

#ifdef ENABLE_FEAT_F4HWN
        if (activeTxVFO != vfo_num || isMainOnly())
#else
        if (activeTxVFO != vfo_num) // this is not active TX VFO
#endif
        {
#ifdef ENABLE_SCAN_RANGES
            if(gScanRangeStart) {

#ifdef ENABLE_FEAT_F4HWN
                //if(IS_FREQ_CHANNEL(gEeprom.ScreenChannel[0]) && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[1])) {
                if(IS_FREQ_CHANNEL(gEeprom.ScreenChannel[activeTxVFO])) {

                    uint8_t shift = 0;

                    if (isMainOnly())
                    {
                        shift = 3;
                    }

                    UI_PrintString("ScnRng", 5, 0, line + shift, 8);
                    sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
                    UI_PrintStringSmallNormal(String, 56, 0, line + shift);
                    sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
                    UI_PrintStringSmallNormal(String, 56, 0, line + shift + 1);

                    if (!isMainOnly())
                        continue;
                }
                else
                {
                    gScanRangeStart = 0;
                }
#else
                UI_PrintString("ScnRng", 5, 0, line, 8);
                sprintf(String, "%3u.%05u", gScanRangeStart / 100000, gScanRangeStart % 100000);
                UI_PrintStringSmallNormal(String, 56, 0, line);
                sprintf(String, "%3u.%05u", gScanRangeStop / 100000, gScanRangeStop % 100000);
                UI_PrintStringSmallNormal(String, 56, 0, line + 1);
                continue;
#endif
            }
#endif


            if (gDTMF_InputMode
#ifdef ENABLE_DTMF_CALLING
                || gDTMF_CallState != DTMF_CALL_STATE_NONE || gDTMF_IsTx
#endif
            ) {
                char *pPrintStr = "";
                // show DTMF stuff
#ifdef ENABLE_DTMF_CALLING
                char Contact[16];
                if (!gDTMF_InputMode) {
                    if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
                        pPrintStr = DTMF_FindContact(gDTMF_String, Contact) ? Contact : gDTMF_String;
                    } else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY){
                        pPrintStr = DTMF_FindContact(gDTMF_Callee, Contact) ? Contact : gDTMF_Callee;
                    }else if (gDTMF_IsTx) {
                        pPrintStr = gDTMF_String;
                    }
                }

                UI_PrintString(pPrintStr, 2, 0, 2 + (vfo_num * 3), 8);

                pPrintStr = "";
                if (!gDTMF_InputMode) {
                    if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT) {
                        pPrintStr = (gDTMF_State == DTMF_STATE_CALL_OUT_RSP) ? "CALL OUT(RSP)" : "CALL OUT";
                    } else if (gDTMF_CallState == DTMF_CALL_STATE_RECEIVED || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY) {
                        sprintf(String, "CALL FRM:%s", (DTMF_FindContact(gDTMF_Caller, Contact)) ? Contact : gDTMF_Caller);
                        pPrintStr = String;
                    } else if (gDTMF_IsTx) {
                        pPrintStr = (gDTMF_State == DTMF_STATE_TX_SUCC) ? "DTMF TX(SUCC)" : "DTMF TX";
                    }
                }
                else
#endif
                {
                    sprintf(String, ">%s", gDTMF_InputBox);
                    pPrintStr = String;
                }

#ifdef ENABLE_FEAT_F4HWN
                if (isMainOnly())
                {
                    UI_PrintString(pPrintStr, 2, 0, 5, 8);
                    isMainOnlyInputDTMF = true;
                    center_line = CENTER_LINE_IN_USE;
                }
                else
                {
                    UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);
                    isMainOnlyInputDTMF = false;
                    center_line = CENTER_LINE_IN_USE;
                    continue;
                }
#else
                UI_PrintString(pPrintStr, 2, 0, 0 + (vfo_num * 3), 8);
                center_line = CENTER_LINE_IN_USE;
                continue;
#endif
            }

            // highlight the selected/used VFO with a marker
            if (isMainVFO)
                memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
        }
        else // active TX VFO
        {   // highlight the selected/used VFO with a marker
            if (isMainVFO)
                memcpy(p_line0 + 0, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
            else
                memcpy(p_line0 + 0, BITMAP_VFO_NotDefault, sizeof(BITMAP_VFO_NotDefault));
        }

        uint32_t frequency = gEeprom.VfoInfo[vfo_num].pRX->Frequency;

        if(TX_freq_check(frequency) != 0 && gEeprom.VfoInfo[vfo_num].TX_LOCK == true && !FUNCTION_IsRx())
        {
            if(isMainOnly())
                memcpy(p_line0 + 14, BITMAP_VFO_Lock, sizeof(BITMAP_VFO_Lock));
            else
                memcpy(p_line0 + 24, BITMAP_VFO_Lock, sizeof(BITMAP_VFO_Lock));
        }

        if (gCurrentFunction == FUNCTION_TRANSMIT)
        {   // transmitting

#ifdef ENABLE_ALARM
            if (gAlarmState == ALARM_STATE_SITE_ALARM)
                mode = VFO_MODE_RX;
            else
#endif
            {
                if (activeTxVFO == vfo_num)
                {   // show the TX symbol
                    mode = VFO_MODE_TX;
                    UI_PrintStringSmallBold("TX", 8, 0, line);
                }
            }
        }
        else
        {   // receiving .. show the RX symbol
            mode = VFO_MODE_RX;
            //if (FUNCTION_IsRx() && gEeprom.RX_VFO == vfo_num) {
            if (FUNCTION_IsRx()) {
                if (gEeprom.RX_VFO == vfo_num && VfoState[vfo_num] == VFO_STATE_NORMAL) {
#ifdef ENABLE_FEAT_F4HWN
                    RxBlinkLed = 1;
                    RxBlinkLedCounter = 0;
                    RxLine = line;
                    RxOnVfofrequency = frequency;

                    RxBlink = !isMainVFO;
#else
                    UI_PrintStringSmallBold("RX", 8, 0, line);
#endif
                }
#ifdef ENABLE_FEAT_F4HWN
                else {
                    if(RxBlinkLed == 1)
                        RxBlinkLed = 2;
                }
            }
            else {
                if(RxOnVfofrequency == frequency && !isMainOnly()) {
                    UI_PrintStringSmallNormal(">>", 8, 0, line);
                    //memcpy(p_line0 + 14, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
                }

                if(RxBlinkLed == 1)
                    RxBlinkLed = 2;
            }
#endif
        }

        if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
        {   // channel mode
            const unsigned int x = 2;
            const bool inputting = gInputBoxIndex != 0 && gEeprom.TX_VFO == vfo_num;
            if (!inputting || gScanStateDir != SCAN_OFF)
                sprintf(String, "M%u", gEeprom.ScreenChannel[vfo_num] + 1);
            else
                sprintf(String, "M%.3s", INPUTBOX_GetAscii());  // show the input text
            UI_PrintStringSmallNormal(String, x, 0, line + 1);
        }
        else if (IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
        {   // frequency mode
            // show the frequency band number
            const unsigned int x = 2;
            char * buf = gEeprom.VfoInfo[vfo_num].pRX->Frequency < _1GHz_in_KHz ? "" : "+";
            sprintf(String, "F%u%s", 1 + gEeprom.ScreenChannel[vfo_num] - FREQ_CHANNEL_FIRST, buf);
            UI_PrintStringSmallNormal(String, x, 0, line + 1);
        }
#ifdef ENABLE_NOAA
        else
        {
            if (gInputBoxIndex == 0 || gEeprom.TX_VFO != vfo_num)
            {   // channel number
                sprintf(String, "N%u", 1 + gEeprom.ScreenChannel[vfo_num] - NOAA_CHANNEL_FIRST);
            }
            else
            {   // user entering channel number
                sprintf(String, "N%u%u", '0' + gInputBox[0], '0' + gInputBox[1]);
            }
            UI_PrintStringSmallNormal(String, 7, 0, line + 1);
        }
#endif

        // ----------------------------------------

        enum VfoState_t state = VfoState[vfo_num];

#ifdef ENABLE_ALARM
        if (gCurrentFunction == FUNCTION_TRANSMIT && gAlarmState == ALARM_STATE_SITE_ALARM) {
            if (activeTxVFO == vfo_num)
                state = VFO_STATE_ALARM;
        }
#endif
        if (state != VFO_STATE_NORMAL)
        {
            if (state < ARRAY_SIZE(VfoStateStr))
                UI_PrintString(VfoStateStr[state], 31, 0, line, 8);
        }
        else if (gInputBoxIndex > 0 && IS_FREQ_CHANNEL(gEeprom.ScreenChannel[vfo_num]) && gEeprom.TX_VFO == vfo_num)
        {   // user entering a frequency
            const char * ascii = INPUTBOX_GetAscii();
            bool isGigaF = frequency>=_1GHz_in_KHz;
            sprintf(String, "%.*s.%.3s", 3 + isGigaF, ascii, ascii + 3 + isGigaF);
#ifdef ENABLE_BIG_FREQ
            if(!isGigaF) {
                // show the remaining 2 small frequency digits
                UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                String[7] = 0;
                // show the main large frequency digits
                UI_DisplayFrequency(String, 32, line, false);
            }
            else
#endif
            {
                // show the frequency in the main font
                UI_PrintString(String, 32, 0, line, 8);
            }

            continue;
        }
        else
        {
            if (gCurrentFunction == FUNCTION_TRANSMIT)
            {   // transmitting
                if (activeTxVFO == vfo_num)
                    frequency = gEeprom.VfoInfo[vfo_num].pTX->Frequency;
            }

            if (IS_MR_CHANNEL(gEeprom.ScreenChannel[vfo_num]))
            {   // it's a channel

                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                    if(gEeprom.MENU_LOCK == false) {
                #endif
                uint8_t countList = 0;
                uint8_t shiftList = 0;

                if(gMR_ChannelExclude[gEeprom.ScreenChannel[vfo_num]] == false)
                {
                    // show the scan list assigment symbols
                    const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];

                    countList = att.scanlist1 + att.scanlist2 + att.scanlist3;

                    if(countList == 0)
                    {
                        memcpy(p_line0 + 127 - (1 * 6), BITMAP_ScanList0, sizeof(BITMAP_ScanList0));
                    }
                    else
                    {
                        shiftList = countList;

                        if (att.scanlist1)
                        {
                            memcpy(p_line0 + 127 - (shiftList * 6), BITMAP_ScanList1, sizeof(BITMAP_ScanList1));
                            shiftList--;
                        }
                        if (att.scanlist2)
                        {
                            memcpy(p_line0 + 127 - (shiftList * 6), BITMAP_ScanList2, sizeof(BITMAP_ScanList2));
                            shiftList--;
                        }
                        if (att.scanlist3)
                        {
                            memcpy(p_line0 + 127 - (shiftList * 6), BITMAP_ScanList3, sizeof(BITMAP_ScanList3));
                        }
                    }
                }
                else
                {
                    memcpy(p_line0 + 127 - (1 * 6), BITMAP_ScanListE, sizeof(BITMAP_ScanListE));
                }

                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                {
                    }
                }
                #endif

                // compander symbol
#ifndef ENABLE_BIG_FREQ
                if (att.compander)
                    memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#else
                // TODO:  // find somewhere else to put the symbol
#endif

                switch (gEeprom.CHANNEL_DISPLAY_MODE)
                {
                    case MDF_FREQUENCY: // show the channel frequency
                        sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
#ifdef ENABLE_BIG_FREQ
                        if(frequency < _1GHz_in_KHz) {
                            // show the remaining 2 small frequency digits
                            UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                            String[7] = 0;
                            // show the main large frequency digits
                            UI_DisplayFrequency(String, 32, line, false);
                        }
                        else
#endif
                        {
                            // show the frequency in the main font
                            UI_PrintString(String, 32, 0, line, 8);
                        }

                        break;

                    case MDF_CHANNEL:   // show the channel number
                        sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
                        UI_PrintString(String, 32, 0, line, 8);
                        break;

                    case MDF_NAME:      // show the channel name
                    case MDF_NAME_FREQ: // show the channel name and frequency

                        SETTINGS_FetchChannelName(String, gEeprom.ScreenChannel[vfo_num]);
                        if (String[0] == 0)
                        {   // no channel name, show the channel number instead
                            sprintf(String, "CH-%03u", gEeprom.ScreenChannel[vfo_num] + 1);
                        }

                        if (gEeprom.CHANNEL_DISPLAY_MODE == MDF_NAME) {
                            UI_PrintString(String, 32, 0, line, 8);
                        }
                        else {
#ifdef ENABLE_FEAT_F4HWN
                            if (isMainOnly())
                            {
                                UI_PrintString(String, 32, 0, line, 8);
                            }
                            else
                            {
                                if(activeTxVFO == vfo_num) {
                                    UI_PrintStringSmallBold(String, 32 + 4, 0, line);
                                }
                                else
                                {
                                    UI_PrintStringSmallNormal(String, 32 + 4, 0, line);     
                                }
                            }
#else
                            UI_PrintStringSmallBold(String, 32 + 4, 0, line);
#endif

#ifdef ENABLE_FEAT_F4HWN
                            if (isMainOnly())
                            {
                                sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);
                                if(frequency < _1GHz_in_KHz) {
                                    // show the remaining 2 small frequency digits
                                    UI_PrintStringSmallNormal(String + 7, 113, 0, line + 4);
                                    String[7] = 0;
                                    // show the main large frequency digits
                                    UI_DisplayFrequency(String, 32, line + 3, false);
                                }
                                else
                                {
                                    // show the frequency in the main font
                                    UI_PrintString(String, 32, 0, line + 3, 8);
                                }
                            }
                            else
                            {
                                sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
                                UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
                            }
#else                           // show the channel frequency below the channel number/name
                            sprintf(String, "%03u.%05u", frequency / 100000, frequency % 100000);
                            UI_PrintStringSmallNormal(String, 32 + 4, 0, line + 1);
#endif
                        }

                        break;
                }
            }
            else
            {   // frequency mode
                sprintf(String, "%3u.%05u", frequency / 100000, frequency % 100000);

#ifdef ENABLE_BIG_FREQ
                if(frequency < _1GHz_in_KHz) {
                    // show the remaining 2 small frequency digits
                    UI_PrintStringSmallNormal(String + 7, 113, 0, line + 1);
                    String[7] = 0;
                    // show the main large frequency digits
                    UI_DisplayFrequency(String, 32, line, false);
                }
                else
#endif
                {
                    // show the frequency in the main font
                    UI_PrintString(String, 32, 0, line, 8);
                }

                // show the channel symbols
                const ChannelAttributes_t att = gMR_ChannelAttributes[gEeprom.ScreenChannel[vfo_num]];
                if (att.compander)
#ifdef ENABLE_BIG_FREQ
                    memcpy(p_line0 + 120, BITMAP_compand, sizeof(BITMAP_compand));
#else
                    memcpy(p_line0 + 120 + LCD_WIDTH, BITMAP_compand, sizeof(BITMAP_compand));
#endif
            }
        }

        // ----------------------------------------

        {   // show the TX/RX level
            int8_t Level = -1;

            if (mode == VFO_MODE_TX)
            {   // TX power level
                /*
                switch (gRxVfo->OUTPUT_POWER)
                {
                    case OUTPUT_POWER_LOW1:     Level = 2; break;
                    case OUTPUT_POWER_LOW2:     Level = 2; break;
                    case OUTPUT_POWER_LOW3:     Level = 2; break;
                    case OUTPUT_POWER_LOW4:     Level = 2; break;
                    case OUTPUT_POWER_LOW5:     Level = 2; break;
                    case OUTPUT_POWER_MID:      Level = 4; break;
                    case OUTPUT_POWER_HIGH:     Level = 6; break;
                }

                if (gRxVfo->OUTPUT_POWER == OUTPUT_POWER_MID) {
                    Level = 4;
                } else if (gRxVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH) {
                    Level = 6;
                } else {
                    Level = 2;
                }
                */

                uint8_t currentPower = gRxVfo->OUTPUT_POWER;

                if(currentPower == OUTPUT_POWER_USER)
                    Level = gSetting_set_pwr;
                else
                    Level = currentPower - 1;
            }
            else 
            if (mode == VFO_MODE_RX)
            {   // RX signal level
                #ifndef ENABLE_RSSI_BAR
                    // bar graph
                    if (gVFO_RSSI_bar_level[vfo_num] > 0)
                        Level = gVFO_RSSI_bar_level[vfo_num];
                #endif
            }
            if(Level >= 0)
                DrawSmallAntennaAndBars(p_line1 + LCD_WIDTH, Level);
        }

        // ----------------------------------------

        String[0] = '\0';
        const VFO_Info_t *vfoInfo = &gEeprom.VfoInfo[vfo_num];

        // show the modulation symbol
        const char * s = "";
#ifdef ENABLE_FEAT_F4HWN
        const char * t = "";
#endif
        const ModulationMode_t mod = vfoInfo->Modulation;
        switch (mod){
            case MODULATION_FM: {
                const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
                const unsigned int code_type = pConfig->CodeType;
#ifdef ENABLE_FEAT_F4HWN
                const char *code_list[] = {"", "CT", "DC", "DC"};
#else
                const char *code_list[] = {"", "CT", "DCS", "DCR"};
#endif
                if (code_type < ARRAY_SIZE(code_list))
                    s = code_list[code_type];
#ifdef ENABLE_FEAT_F4HWN
                if(gCurrentFunction != FUNCTION_TRANSMIT || activeTxVFO != vfo_num)
                    t = gModulationStr[mod];
#endif
                break;
            }
            default:
                t = gModulationStr[mod];
            break;
        }

#if ENABLE_FEAT_F4HWN
        const FREQ_Config_t *pConfig = (mode == VFO_MODE_TX) ? vfoInfo->pTX : vfoInfo->pRX;
        int8_t shift = 0;

        switch((int)pConfig->CodeType)
        {
            case 1:
            sprintf(String, "%u.%u", CTCSS_Options[pConfig->Code] / 10, CTCSS_Options[pConfig->Code] % 10);
            break;

            case 2:
            case 3:
            sprintf(String, (int)pConfig->CodeType == 2 ? "%03oN" : "%03oI", DCS_Options[pConfig->Code]);
            break;

            default:
            sprintf(String, "%d.%02uK", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
            shift = -10;
        }

        if (gSetting_set_gui)
        {
            UI_PrintStringSmallNormal(s, LCD_WIDTH + 22, 0, line + 1);
            UI_PrintStringSmallNormal(t, LCD_WIDTH + 2, 0, line + 1);

            if (isMainOnly() && !gDTMF_InputMode)
            {
                if(shift == 0)
                {
                    UI_PrintStringSmallNormal(String, 2, 0, 6);
                }

                if((vfoInfo->StepFrequency / 100) < 100)
                {
                    sprintf(String, "%d.%02uK", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
                }
                else
                {
                    sprintf(String, "%dK", vfoInfo->StepFrequency / 100);               
                }
                UI_PrintStringSmallNormal(String, 46, 0, 6);
            }
        }
        else
        {
            if ((s != NULL) && (s[0] != '\0')) {
                GUI_DisplaySmallest(s, 58, line == 0 ? 17 : 49, false, true);
            }

            if ((t != NULL) && (t[0] != '\0')) {
                GUI_DisplaySmallest(t, 3, line == 0 ? 17 : 49, false, true);
            }

            GUI_DisplaySmallest(String, 68 + shift, line == 0 ? 17 : 49, false, true);

            //sprintf(String, "%d.%02u", vfoInfo->StepFrequency / 100, vfoInfo->StepFrequency % 100);
            //GUI_DisplaySmallest(String, 91, line == 0 ? 2 : 34, false, true);
        }
#else
        UI_PrintStringSmallNormal(s, LCD_WIDTH + 24, 0, line + 1);
#endif

        if (state == VFO_STATE_NORMAL || state == VFO_STATE_ALARM)
        {   // show the TX power
            uint8_t currentPower = vfoInfo->OUTPUT_POWER % 8;
            uint8_t arrowPos = 19;
            bool userPower = false;

            if(currentPower == OUTPUT_POWER_USER)
            {
                currentPower = gSetting_set_pwr;
                userPower = true;
            }
            else
            {
                currentPower--;
                userPower = false;
            }

            if (gSetting_set_gui)
            {
                const char pwr_short[][3] = {"L1", "L2", "L3", "L4", "L5", "M", "H"};
                //sprintf(String, "%s", pwr_short[currentPower]);
                //UI_PrintStringSmallNormal(String, LCD_WIDTH + 42, 0, line + 1);
                UI_PrintStringSmallNormal(pwr_short[currentPower], LCD_WIDTH + 42, 0, line + 1);

                arrowPos = 38;
            }
            else
            {
                const char pwr_long[][5] = {"LOW1", "LOW2", "LOW3", "LOW4", "LOW5", "MID", "HIGH"};
                //sprintf(String, "%s", pwr_long[currentPower]);
                //GUI_DisplaySmallest(String, 24, line == 0 ? 17 : 49, false, true);
                GUI_DisplaySmallest(pwr_long[currentPower], 24, line == 0 ? 17 : 49, false, true);
            }

            if(userPower == true)
            {
                memcpy(p_line0 + 256 + arrowPos, BITMAP_PowerUser, sizeof(BITMAP_PowerUser));
            }
        }

        if (vfoInfo->freq_config_RX.Frequency != vfoInfo->freq_config_TX.Frequency)
        {   // show the TX offset symbol
            int i = vfoInfo->TX_OFFSET_FREQUENCY_DIRECTION % 3;

            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                const char dir_list[][2] = {"", "+", "-", "D"};

                if(gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION != 0 && gTxVfo->pTX == &gTxVfo->freq_config_RX && !vfoInfo->FrequencyReverse)
                {
                    i = 3;
                }
            #else
                const char dir_list[][2] = {"", "+", "-"};
            #endif

#if ENABLE_FEAT_F4HWN
        if (gSetting_set_gui)
        {
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 60, 0, line + 1);
        }
        else
        {
            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            if(i == 3)
            {
                GUI_DisplaySmallest(dir_list[i], 43, line == 0 ? 17 : 49, false, true);
            }
            else
            {
            #endif
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 41, 0, line + 1);
            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            }
            #endif
        }
#else
            UI_PrintStringSmallNormal(dir_list[i], LCD_WIDTH + 54, 0, line + 1);
#endif
        }

        // show the TX/RX reverse symbol
        if (vfoInfo->FrequencyReverse)
#if ENABLE_FEAT_F4HWN
        {
            if (gSetting_set_gui)
            {
                UI_PrintStringSmallNormal("R", LCD_WIDTH + 68, 0, line + 1);
            }
            else
            {
                GUI_DisplaySmallest("R", 51, line == 0 ? 17 : 49, false, true);
            }
        }
#else
            UI_PrintStringSmallNormal("R", LCD_WIDTH + 62, 0, line + 1);
#endif

#if ENABLE_FEAT_F4HWN
        #ifdef ENABLE_FEAT_F4HWN_NARROWER
            bool narrower = 0;

            if(vfoInfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW && gSetting_set_nfm == 1)
            {
                narrower = 1;
            }

            if (gSetting_set_gui)
            {
                const char *bandWidthNames[] = {"W", "N", "N+"};
                UI_PrintStringSmallNormal(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH + narrower], LCD_WIDTH + 80, 0, line + 1);
            }
            else
            {
                const char *bandWidthNames[] = {"WIDE", "NAR", "NAR+"};
                GUI_DisplaySmallest(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH + narrower], 91, line == 0 ? 17 : 49, false, true);
            }
        #else
            if (gSetting_set_gui)
            {
                const char *bandWidthNames[] = {"W", "N"};
                UI_PrintStringSmallNormal(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH], LCD_WIDTH + 80, 0, line + 1);
            }
            else
            {
                const char *bandWidthNames[] = {"WIDE", "NAR"};
                GUI_DisplaySmallest(bandWidthNames[vfoInfo->CHANNEL_BANDWIDTH], 91, line == 0 ? 17 : 49, false, true);
            }
        #endif
#else
        if (vfoInfo->CHANNEL_BANDWIDTH == BANDWIDTH_NARROW)
            UI_PrintStringSmallNormal("N", LCD_WIDTH + 70, 0, line + 1);
#endif

#ifdef ENABLE_DTMF_CALLING
        // show the DTMF decoding symbol
        if (vfoInfo->DTMF_DECODING_ENABLE || gSetting_KILLED)
            UI_PrintStringSmallNormal("DTMF", LCD_WIDTH + 78, 0, line + 1);
#endif

#ifndef ENABLE_FEAT_F4HWN
        // show the audio scramble symbol
        if (vfoInfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
            UI_PrintStringSmallNormal("SCR", LCD_WIDTH + 106, 0, line + 1);
#endif

#ifdef ENABLE_FEAT_F4HWN
        /*
        if(isMainVFO)   
        {
            if(gMonitor)
            {
                sprintf(String, "%s", "MONI");
            }
            
            if (gSetting_set_gui)
            {
                if(!gMonitor)
                {
                    sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
                }
                UI_PrintStringSmallNormal(String, LCD_WIDTH + 98, 0, line + 1);
            }
            else
            {
                if(!gMonitor)
                {
                    sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
                }
                GUI_DisplaySmallest(String, 110, line == 0 ? 17 : 49, false, true);
            }
        }
        */
        if (isMainVFO) {
           if (gMonitor) {
                strcpy(String, "MONI");
           } else {
                sprintf(String, "SQL%d", gEeprom.SQUELCH_LEVEL);
           }

           if (gSetting_set_gui) {
                UI_PrintStringSmallNormal(String, LCD_WIDTH + 98, 0, line + 1);
           } else {
                GUI_DisplaySmallest(String, 110, line == 0 ? 17 : 49, false, true);
           }
        }
#endif
    }

#ifdef ENABLE_AGC_SHOW_DATA
    center_line = CENTER_LINE_IN_USE;
    UI_MAIN_PrintAGC(false);
#endif

    if (center_line == CENTER_LINE_NONE)
    {   // we're free to use the middle line

        const bool rx = FUNCTION_IsRx();

#ifdef ENABLE_AUDIO_BAR
        if (gSetting_mic_bar && gCurrentFunction == FUNCTION_TRANSMIT) {
            center_line = CENTER_LINE_AUDIO_BAR;
            UI_DisplayAudioBar();
        }
        else
#endif

#if defined(ENABLE_AM_FIX) && defined(ENABLE_AM_FIX_SHOW_DATA)
        if (rx && gEeprom.VfoInfo[gEeprom.RX_VFO].Modulation == MODULATION_AM && gSetting_AM_fix)
        {
            if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                )
                return;

            center_line = CENTER_LINE_AM_FIX_DATA;
            AM_fix_print_data(gEeprom.RX_VFO, String);
            UI_PrintStringSmallNormal(String, 2, 0, 3);
        }
        else
#endif

#ifdef ENABLE_RSSI_BAR
        if (rx) {
            center_line = CENTER_LINE_RSSI;
            DisplayRSSIBar(false);
        }
        else
#endif
        if (rx || gCurrentFunction == FUNCTION_FOREGROUND || gCurrentFunction == FUNCTION_POWER_SAVE)
        {
            #if 1
                if (gSetting_live_DTMF_decoder && gDTMF_RX_live[0] != 0 && gKeypadLocked == 0)
                {   // show live DTMF decode
                    const unsigned int len = strlen(gDTMF_RX_live);
                    const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

                    if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                        || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                        )
                        return;

                    center_line = CENTER_LINE_DTMF_DEC;

                    sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
#ifdef ENABLE_FEAT_F4HWN
                    if (isMainOnly())
                    {
                        UI_PrintStringSmallNormal(String, 2, 0, 5);
                    }
                    else
                    {
                        UI_PrintStringSmallNormal(String, 2, 0, 3);
                    }
#else
                    UI_PrintStringSmallNormal(String, 2, 0, 3);

#endif
                }
            #else
                if (gSetting_live_DTMF_decoder && gDTMF_RX_index > 0)
                {   // show live DTMF decode
                    const unsigned int len = gDTMF_RX_index;
                    const unsigned int idx = (len > (17 - 5)) ? len - (17 - 5) : 0;  // limit to last 'n' chars

                    if (gScreenToDisplay != DISPLAY_MAIN ||
                        gDTMF_CallState != DTMF_CALL_STATE_NONE)
                        return;

                    center_line = CENTER_LINE_DTMF_DEC;

                    sprintf(String, "DTMF %s", gDTMF_RX_live + idx);
                    UI_PrintStringSmallNormal(String, 2, 0, 3);
                }
            #endif

#ifdef ENABLE_SHOW_CHARGE_LEVEL
            else if (gChargingWithTypeC)
            {   // charging .. show the battery state
                if (gScreenToDisplay != DISPLAY_MAIN
#ifdef ENABLE_DTMF_CALLING
                    || gDTMF_CallState != DTMF_CALL_STATE_NONE
#endif
                    )
                    return;

                center_line = CENTER_LINE_CHARGE_DATA;

                sprintf(String, "Charge %u.%02uV %u%%",
                    gBatteryVoltageAverage / 100, gBatteryVoltageAverage % 100,
                    BATTERY_VoltsToPercent(gBatteryVoltageAverage));
                UI_PrintStringSmallNormal(String, 2, 0, 3);
            }
#endif
        }
    }

#ifdef ENABLE_FEAT_F4HWN
    //#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    //if(gEeprom.MENU_LOCK == false)
    //{
    //#endif
    if (isMainOnly() && !gDTMF_InputMode)
    {
        sprintf(String, "VFO %s", activeTxVFO ? "B" : "A");
        UI_PrintStringSmallBold(String, 92, 0, 6);
        for (uint8_t i = 92; i < 128; i++)
        {
            gFrameBuffer[6][i] ^= 0x7F;
        }
    }
    //#ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
    //}
    //#endif
#endif

    ST7565_BlitFullScreen();
}
