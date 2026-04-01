/* Copyright 2024 Armel F4HWN
 * https://github.com/armel
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

#include "debugging.h"
#include "driver/st7565.h"
#include "screenshot.h"
#include "misc.h"

void SCREENSHOT_Line(uint8_t *src, uint8_t *dest, uint16_t *idx) {
    for (uint8_t b = 0; b < 8; b++) {
        for (uint8_t i = 0; i < 128; i += 8) {
            uint8_t acc = 0;
            for (uint8_t k = 0; k < 8; k++) {
                if (src[i + k] & (1 << b)) acc |= (1 << k);
            }
            dest[(*idx)++] = gSetting_set_inv ? ~acc : acc;
        }
    }
}

void SCREENSHOT_Update(bool force)
{
    static uint8_t previousFrame[1024] = {0}; // Last transmitted frame
    static uint8_t forcedBlock = 0;           // Block forced for refresh on each frame
    static uint8_t keepAlive = 3;             // Keepalive counter

    // Use a single buffer to reduce stack usage
    static uint8_t currentFrame[1024];        // Current frame
    static uint8_t deltaFrame[128 * 9];       // Delta frame buffer
    uint16_t index = 0;
    uint8_t acc = 0;
    uint8_t bitCount = 0;

    if (gUART_LockScreenshot > 0) // Lock screenshot if Chirp is in used
    {
        gUART_LockScreenshot--;
        return;
    }

    if (UART_IsCableConnected()) {
        keepAlive = 15;
    }

    if (keepAlive > 0) {
        if (--keepAlive == 0) return;
    }
    else
    {
        return;
    }

    // Build current frame from status line (first 8 lines)
    SCREENSHOT_Line(gStatusLine, currentFrame, &index);

    // Build remaining part of the frame (7 * 8 lines)
    for (uint8_t l = 0; l < 7; l++) {
        SCREENSHOT_Line(gFrameBuffer[l], currentFrame, &index);
    }

    if (bitCount > 0)
        currentFrame[index++] = acc;

    if (index != 1024)
        return; // Frame size mismatch, abort

    // Generate delta frame
    uint16_t deltaLen = 0;

    for (uint8_t block = 0; block < 128; block++) {
        uint8_t *cur = &currentFrame[block * 8];
        uint8_t *prev = &previousFrame[block * 8];

        bool changed    = memcmp(cur, prev, 8) != 0;
        bool isForced   = (block == forcedBlock);
        bool fullUpdate = force;

        if (changed || isForced || fullUpdate) {
            deltaFrame[deltaLen++] = block;
            memcpy(&deltaFrame[deltaLen], cur, 8);
            deltaLen += 8;

            memcpy(prev, cur, 8); // Update stored frame
        }
    }

    forcedBlock = (forcedBlock + 1) % 128;

    if (deltaLen == 0)
        return; // No update needed

    // Send the delta frame over UART
    uint8_t header[5] = {
        0xAA, 0x55, 0x02,
        (uint8_t)(deltaLen >> 8),
        (uint8_t)(deltaLen & 0xFF)
    };

    UART_Send(header, 5);
    UART_Send(deltaFrame, deltaLen);
    uint8_t end = 0x0A;
    UART_Send(&end, 1);
}