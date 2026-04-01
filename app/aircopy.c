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

#ifdef ENABLE_AIRCOPY

//#if !defined(ENABLE_OVERLAY)
//  #include "ARMCM0.h"
//#endif

#include "app/aircopy.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "driver/crc.h"
#include "driver/eeprom.h"
#include "frequencies.h"
#include "misc.h"
#include "radio.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

#ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
#include "screenshot.h"
#endif

static const uint16_t Obfuscation[8] = { 0x6C16, 0xE614, 0x912E, 0x400D, 0x3521, 0x40D5, 0x0313, 0x80E9 };

AIRCOPY_State_t gAircopyState;
uint16_t gAirCopyBlockNumber;
uint16_t gErrorsDuringAirCopy;
bool     gAirCopyIsSendMode;

uint16_t g_FSK_Buffer[36];

static void AIRCOPY_clear()
{
    for (uint8_t i = 0; i < 15; i++)
    {
        crc[i] = 0;
    }
    #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
        SCREENSHOT_Update(true);
    #endif
}

static inline void AIRCOPY_Obfuscation(void)
{
    for (unsigned int i = 0; i < 34; i++) {
        g_FSK_Buffer[i + 1] ^= Obfuscation[i % 8];
    }
}

// ============================================================================
// Send/Receive Functions
// ============================================================================

bool AIRCOPY_SendMessage(void)
{
    static uint8_t gAircopySendCountdown = 1;

    if (gAircopyState != AIRCOPY_TRANSFER) {
        return 1;
    }

    if (--gAircopySendCountdown) {
        return 1;
    }

    g_FSK_Buffer[1] = (gAirCopyBlockNumber & 0x3FF) << 6;

    EEPROM_ReadBuffer(g_FSK_Buffer[1], &g_FSK_Buffer[2], 64);

    g_FSK_Buffer[34] = CRC_Calculate(&g_FSK_Buffer[1], 2 + 64);

    AIRCOPY_Obfuscation();

    if (++gAirCopyBlockNumber >= 0x78) {
        gAircopyState = AIRCOPY_COMPLETE;
        #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
            SCREENSHOT_Update(false);
        #endif
        //NVIC_SystemReset();
    }

    RADIO_SetTxParameters();

    BK4819_SendFSKData(g_FSK_Buffer);
    BK4819_SetupPowerAmplifier(0, 0);
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);

    gAircopySendCountdown = 30;

    return 0;
}

void AIRCOPY_StorePacket(void)
{
    if (gFSKWriteIndex < 36) {
        return;
    }

    gFSKWriteIndex = 0;
    gUpdateDisplay = true;
    uint16_t Status = BK4819_ReadRegister(BK4819_REG_0B);
    BK4819_PrepareFSKReceive();

    // Doc says bit 4 should be 1 = CRC OK, 0 = CRC FAIL, but original firmware checks for FAIL.

    if ((Status & 0x0010U) != 0 || g_FSK_Buffer[0] != 0xABCD || g_FSK_Buffer[35] != 0xDCBA) {
        gErrorsDuringAirCopy++;
        return;
    }

    AIRCOPY_Obfuscation();

    uint16_t Crc = CRC_Calculate(&g_FSK_Buffer[1], 2 + 64);
    if (g_FSK_Buffer[34] != Crc) {
        gErrorsDuringAirCopy++;
        return;
    }

    uint16_t Offset = g_FSK_Buffer[1];

    if (Offset >= 0x1E00) {
        gErrorsDuringAirCopy++;
        return;
    }

    const uint16_t *pData = &g_FSK_Buffer[2];
    for (unsigned int i = 0; i < 8; i++) {
        EEPROM_WriteBuffer(Offset, pData);
        pData += 4;
        Offset += 8;
    }

    if (Offset == 0x1E00) {
        gAircopyState = AIRCOPY_COMPLETE;
        #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
            SCREENSHOT_Update(false);
        #endif
    }

    gAirCopyBlockNumber++;
}

static void AIRCOPY_InitTransfer(bool isSendMode)
{
    gAircopyStep = 1;
    gFSKWriteIndex = 0;
    gAirCopyBlockNumber = 0;
    gInputBoxIndex = 0;
    gAirCopyIsSendMode = isSendMode;

    AIRCOPY_clear();
    
    gAircopyState = AIRCOPY_TRANSFER;
}

// ============================================================================
// Key Processing
// ============================================================================

static void AIRCOPY_Key_DIGITS(KEY_Code_t Key)
{
    INPUTBOX_Append(Key);

    if (gInputBoxIndex < 6) {
#ifdef ENABLE_VOICE
        gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
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

#ifdef ENABLE_VOICE
        gAnotherVoiceID = (VOICE_ID_t)Key;
#endif

        Frequency = FREQUENCY_RoundToStep(Frequency, gRxVfo->StepFrequency);
        gRxVfo->Band = i;
        gRxVfo->freq_config_RX.Frequency = Frequency;
        gRxVfo->freq_config_TX.Frequency = Frequency;
        RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
        gCurrentVfo = gRxVfo;
        RADIO_SetupRegisters(true);
        BK4819_SetupAircopy();
        BK4819_ResetFSK();
        return;
    }
}

static void AIRCOPY_Key_EXIT()
{
    if (gInputBoxIndex == 0) {
        AIRCOPY_InitTransfer(0); // Mode: Receive
        gErrorsDuringAirCopy = lErrorsDuringAirCopy = 0;

        BK4819_PrepareFSKReceive();
        
    } else {
        gInputBox[--gInputBoxIndex] = 10;
    }
}

static void AIRCOPY_Key_MENU()
{
    AIRCOPY_InitTransfer(1); // Mode: Send
    
    g_FSK_Buffer[0] = 0xABCD;
    g_FSK_Buffer[1] = 0;
    g_FSK_Buffer[35] = 0xDCBA;
}

void AIRCOPY_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    if (bKeyHeld || !bKeyPressed) {
        return;
    }

    if (Key != KEY_PTT) {
        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    }

    switch (Key) {
    case KEY_0...KEY_9:
        AIRCOPY_Key_DIGITS(Key);
        break;
    case KEY_MENU:
        AIRCOPY_Key_MENU();
        break;
    case KEY_EXIT:
        AIRCOPY_Key_EXIT();
        break;
    case KEY_PTT:
        break;
    default:
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        break;
    }

    gRequestDisplayScreen = DISPLAY_AIRCOPY;
}

#endif
