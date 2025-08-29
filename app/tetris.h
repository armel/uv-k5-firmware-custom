/* Copyright 2025 Armel F4HWN
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

#ifndef APP_TETRIS_H
#define APP_TETRIS_H

#include "../bitmaps.h"
#include "../board.h"
#include "../bsp/dp32g030/gpio.h"
#include "../driver/bk4819-regs.h"
#include "../driver/bk4819.h"
#include "../driver/gpio.h"
#include "../driver/keyboard.h"
#include "../driver/st7565.h"
#include "../driver/system.h"
#include "../driver/systick.h"
#include "../external/printf/printf.h"
#include "../font.h"
#include "../helper/battery.h"
#include "../misc.h"
#include "../radio.h"
#include "../settings.h"
#include "../ui/helper.h"
#include "../audio.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define BOARD_WIDTH 10
#define BOARD_HEIGHT 18
#define BLOCK_SIZE 3 // Adjusted for screen size
#define BOARD_X_OFFSET ((LCD_WIDTH - (BOARD_WIDTH * BLOCK_SIZE)) / 2)
#define BOARD_Y_OFFSET 1

typedef struct {
    int8_t x;
    int8_t y;
} Position;

typedef struct {
    const uint8_t (*shape)[4][4];
    Position pos;
    uint8_t rotation;
    uint8_t type;
} Tetromino;

typedef struct KeyboardState
{
    KEY_Code_t current;
    KEY_Code_t prev;
    uint8_t counter;
} KeyboardState;

void APP_RunTetris(void);

#endif // APP_TETRIS_H
