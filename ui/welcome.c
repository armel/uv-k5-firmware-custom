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

#include "driver/eeprom.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "helper/battery.h"
#include "settings.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/welcome.h"
#include "ui/status.h"
#include "version.h"
#include "bitmaps.h"

#ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
    #include "screenshot.h"
#endif

void UI_DisplayReleaseKeys(void)
{
    memset(gStatusLine,  0, sizeof(gStatusLine));
#if defined(ENABLE_FEAT_F4HWN_CTR) || defined(ENABLE_FEAT_F4HWN_INV)
        ST7565_ContrastAndInv();
#endif
    UI_DisplayClear();

    UI_PrintString("RELEASE", 0, 127, 1, 10);
    UI_PrintString("ALL KEYS", 0, 127, 3, 10);

    ST7565_BlitStatusLine();  // blank status line
    ST7565_BlitFullScreen();
}

void UI_DisplayWelcome(void)
{
    char WelcomeString0[16];
    char WelcomeString1[16];
    char WelcomeString2[16];
    char WelcomeString3[20];

    memset(gStatusLine,  0, sizeof(gStatusLine));

#if defined(ENABLE_FEAT_F4HWN_CTR) || defined(ENABLE_FEAT_F4HWN_INV)
        ST7565_ContrastAndInv();
#endif
    UI_DisplayClear();

#ifdef ENABLE_FEAT_F4HWN
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
    
    if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_NONE || gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_SOUND) {
        ST7565_FillScreen(0x00);
#else
    if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_NONE || gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_FULL_SCREEN) {
        ST7565_FillScreen(0xFF);
#endif
    } else {
        memset(WelcomeString0, 0, sizeof(WelcomeString0));
        memset(WelcomeString1, 0, sizeof(WelcomeString1));

        EEPROM_ReadBuffer(0x0EB0, WelcomeString0, 16);
        EEPROM_ReadBuffer(0x0EC0, WelcomeString1, 16);

        sprintf(WelcomeString2, "%u.%02uV %u%%",
                gBatteryVoltageAverage / 100,
                gBatteryVoltageAverage % 100,
                BATTERY_VoltsToPercent(gBatteryVoltageAverage));

        if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_VOLTAGE)
        {
            strcpy(WelcomeString0, "VOLTAGE");
            strcpy(WelcomeString1, WelcomeString2);
        }
        else if(gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_ALL)
        {
            if(strlen(WelcomeString0) == 0 && strlen(WelcomeString1) == 0)
            {
                strcpy(WelcomeString0, "WELCOME");
                strcpy(WelcomeString1, WelcomeString2);
            }
            else if(strlen(WelcomeString0) == 0 || strlen(WelcomeString1) == 0)
            {
                if(strlen(WelcomeString0) == 0)
                {
                    strcpy(WelcomeString0, WelcomeString1);
                }
                strcpy(WelcomeString1, WelcomeString2);
            }
        }
        else if(gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_MESSAGE)
        {
            if(strlen(WelcomeString0) == 0)
            {
                strcpy(WelcomeString0, "WELCOME");
            }

            if(strlen(WelcomeString1) == 0)
            {
                strcpy(WelcomeString1, "BIENVENUE");
            }
        }

        UI_PrintString(WelcomeString0, 0, 127, 0, 10);
        UI_PrintString(WelcomeString1, 0, 127, 2, 10);

#ifdef ENABLE_FEAT_F4HWN
        UI_PrintStringSmallNormal(Version, 0, 128, 4);

        UI_DrawLineBuffer(gFrameBuffer, 0, 31, 127, 31, 1); // Be ware, status zone = 8 lines, the rest = 56 ->total 64

        for (uint8_t i = 18; i < 110; i++)
        {
            gFrameBuffer[4][i] ^= 0xFF;
        }

        sprintf(WelcomeString3, "%s Edition", Edition);
        UI_PrintStringSmallNormal(WelcomeString3, 0, 127, 6);

        /*
        #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            #if ENABLE_FEAT_F4HWN_RESCUE_OPS > 1
                UI_PrintStringSmallNormal(Edition, 18, 0, 6);
                if(gEeprom.MENU_LOCK == true) {
                    memcpy(gFrameBuffer[6] + 103, BITMAP_Ready, sizeof(BITMAP_Ready));
                }
                else
                {
                    memcpy(gFrameBuffer[6] + 103, BITMAP_NotReady, sizeof(BITMAP_NotReady));                    
                }
            #else
                UI_PrintStringSmallNormal(Edition, 18, 0, 5);
                memcpy(gFrameBuffer[5] + 103, BITMAP_Ready, sizeof(BITMAP_Ready));
                
                #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                    UI_PrintStringSmallNormal("RescueOps", 18, 0, 6);
                    if(gEeprom.MENU_LOCK == true) {
                        memcpy(gFrameBuffer[6] + 103, BITMAP_Ready, sizeof(BITMAP_Ready));
                    }
                    else
                    {
                        memcpy(gFrameBuffer[6] + 103, BITMAP_NotReady, sizeof(BITMAP_NotReady));
                    }
                #endif
            #endif
        #else
            UI_PrintStringSmallNormal(Edition, 18, 0, 6);
            memcpy(gFrameBuffer[6] + 103, BITMAP_Ready, sizeof(BITMAP_Ready));                    
        #endif
        */

        /*
        #ifdef ENABLE_SPECTRUM
            #ifdef ENABLE_FMRADIO
                    UI_PrintStringSmallNormal(Based, 0, 127, 5);
                    UI_PrintStringSmallNormal(Credits, 0, 127, 6);
            #else
                    UI_PrintStringSmallNormal("Bandscope  ", 0, 127, 5);
                    memcpy(gFrameBuffer[5] + 95, BITMAP_Ready, sizeof(BITMAP_Ready));

                    #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                        UI_PrintStringSmallNormal("RescueOps  ", 0, 127, 6);
                        if(gEeprom.MENU_LOCK == true) {
                            memcpy(gFrameBuffer[6] + 95, BITMAP_Ready, sizeof(BITMAP_Ready));
                        }
                    #else
                        UI_PrintStringSmallNormal("Broadcast  ", 0, 127, 6);
                    #endif
            #endif
        #else
            #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
                UI_PrintStringSmallNormal("RescueOps  ", 0, 127, 5);
                if(gEeprom.MENU_LOCK == true) {
                    memcpy(gFrameBuffer[5] + 95, BITMAP_Ready, sizeof(BITMAP_Ready));
                }
            #else
                UI_PrintStringSmallNormal("Bandscope  ", 0, 127, 5);
            #endif
            UI_PrintStringSmallNormal("Broadcast  ", 0, 127, 6);
            memcpy(gFrameBuffer[6] + 95, BITMAP_Ready, sizeof(BITMAP_Ready));
        #endif
        */
#else
        UI_PrintStringSmallNormal(Version, 0, 127, 6);
#endif

        //ST7565_BlitStatusLine();  // blank status line : I think it's useless
        ST7565_BlitFullScreen();

        #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
            SCREENSHOT_Update(true);
        #endif
    }
}
