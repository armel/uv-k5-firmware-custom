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

#ifdef ENABLE_FMRADIO

#include <string.h>

#include "app/fm.h"
#include "driver/bk1080.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "misc.h"
#include "settings.h"
#include "ui/fmradio.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/ui.h"

void UI_DisplayFM(void)
{
    char String[16] = {0};
    UI_DisplayClear();

    // 1. Top Left Title
    UI_PrintString("FM", 2, 0, 0, 8);



    // 3. Determine Center Label (VFO or Scanning)
    const char *pPrintStr = "";

    if (gFM_ScanState != FM_SCAN_OFF) {
        // We are currently searching for a station
        pPrintStr = "SCAN";
    } else {
        // Standard tuning mode
        pPrintStr = "VFO";
    }

    // Print the mode label in the middle (Line 3)
    UI_PrintString(pPrintStr, 0, 127, 3, 10);

    // 4. Frequency Display Logic
    memset(String, 0, sizeof(String));

    if (gInputBoxIndex == 0) {
        // Normal display: e.g., "101.9"
        sprintf(String, "%3d.%d", gEeprom.FM_FrequencyPlaying / 10, gEeprom.FM_FrequencyPlaying % 10);
    } else {
        // Digit entry display: Formats the input box buffer
        const char * ascii = INPUTBOX_GetAscii();
        // Assuming 4 digits entered: "1019" -> "101.9"
        sprintf(String, "%.3s.%.1s", ascii, ascii + 3);
    }

    // Render the large frequency numbers
    // Params: String, X-offset (32 centers it well), Line (1), IsLargeFont
    UI_DisplayFrequency(String, 32, 1, true);

    ST7565_BlitFullScreen();
}

#endif
