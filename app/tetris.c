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

#include "app/tetris.h"
#include "app/game.h"

#ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
#include "screenshot.h"
#endif

// --- Game data ---

static uint8_t gameBoard[BOARD_HEIGHT][BOARD_WIDTH];
static Tetromino currentTetromino;
static Tetromino nextTetromino;
static uint32_t score = 0;
static uint16_t level = 1;
static uint16_t linesCleared = 0;
static bool isPaused = false;
static bool isGameOver = false;
static uint32_t fallCounter = 0;
static uint32_t fallSpeed = 30;

static char str[16];
static KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

// --- Tetromino shapes (Bitmasked) ---
const uint16_t aTetromino[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, // I
    {0x0660, 0x0660, 0x0660, 0x0660}, // O
    {0x0E40, 0x4C40, 0x4E00, 0x4640}, // T
    {0x8E00, 0x6440, 0x0E20, 0x44C0}, // L
    {0x2E00, 0x4460, 0x0E80, 0xC440}, // J
    {0x6C00, 0x4620, 0x06C0, 0x8C40}, // S
    {0xC600, 0x2640, 0x0C60, 0x4C80}  // Z
};

// --- Game Logic ---

void drawBlock(int8_t x, int8_t y, bool fill) {
    UI_DrawRectangleBuffer(gFrameBuffer, x, y, x + BLOCK_SIZE - 1, y + BLOCK_SIZE - 1, fill);
}

void drawTetromino(const Tetromino* t, bool fill) {
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < 4; j++) {
            if ((t->shape[t->rotation] >> (i * 4 + j)) & 1) {
                drawBlock(BOARD_X_OFFSET + (t->pos.x + j) * BLOCK_SIZE,
                          BOARD_Y_OFFSET + (t->pos.y + i) * BLOCK_SIZE,
                          fill);
            }
        }
    }
}

void drawBoard() {
    // Draw border
    UI_DrawRectangleBuffer(gFrameBuffer, BOARD_X_OFFSET - 1, BOARD_Y_OFFSET - 1,
                           BOARD_X_OFFSET + BOARD_WIDTH * BLOCK_SIZE,
                           BOARD_Y_OFFSET + BOARD_HEIGHT * BLOCK_SIZE, true);

    // Draw placed blocks
    for (uint8_t y = 0; y < BOARD_HEIGHT; y++) {
        for (uint8_t x = 0; x < BOARD_WIDTH; x++) {
            if (gameBoard[y][x]) {
                drawBlock(BOARD_X_OFFSET + x * BLOCK_SIZE, BOARD_Y_OFFSET + y * BLOCK_SIZE, true);
            }
        }
    }
}

void drawScoreTetris() {
    // Clean status line
    memset(gStatusLine, 0, sizeof(gStatusLine));

    sprintf(str, "Level %02u", level);
    GUI_DisplaySmallest(str, 0, 1, true, true);

    sprintf(str, "Score %04u", score);
    GUI_DisplaySmallest(str, 45, 1, true, true);

    // Draw next piece
    Tetromino tempNext = nextTetromino;
    tempNext.pos.x = (LCD_WIDTH - 80) / BLOCK_SIZE - 4;
    tempNext.pos.y = 2;
    drawTetromino(&tempNext, true);
}

bool checkCollision(const Tetromino* t) {
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < 4; j++) {
            if ((t->shape[t->rotation] >> (i * 4 + j)) & 1) {
                int8_t boardX = t->pos.x + j;
                int8_t boardY = t->pos.y + i;

                if (boardX < 0 || boardX >= BOARD_WIDTH || boardY >= BOARD_HEIGHT) {
                    return true; // Collision with walls or floor
                }
                if (boardY >= 0 && gameBoard[boardY][boardX]) {
                    return true; // Collision with other blocks
                }
            }
        }
    }
    return false;
}

void mergeTetromino() {
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t j = 0; j < 4; j++) {
            if ((currentTetromino.shape[currentTetromino.rotation] >> (i * 4 + j)) & 1) {
                int8_t boardX = currentTetromino.pos.x + j;
                int8_t boardY = currentTetromino.pos.y + i;
                if (boardY >= 0) {
                    gameBoard[boardY][boardX] = currentTetromino.type + 1;
                }
            }
        }
    }
}

void clearLines() {
    uint8_t lines = 0;
    for (uint8_t y = 0; y < BOARD_HEIGHT; y++) {
        bool lineIsFull = true;
        for (uint8_t x = 0; x < BOARD_WIDTH; x++) {
            if (gameBoard[y][x] == 0) {
                lineIsFull = false;
                break;
            }
        }

        if (lineIsFull) {
            lines++;
            for (uint8_t k = y; k > 0; k--) {
                memcpy(gameBoard[k], gameBoard[k - 1], BOARD_WIDTH * sizeof(uint8_t));
            }
            memset(gameBoard[0], 0, BOARD_WIDTH * sizeof(uint8_t));
        }
    }

    if (lines > 0) {
        linesCleared += lines;
        score += (100 * lines * lines); // Bonus for multiple lines
        level = 1 + (linesCleared / 10);
        fallSpeed = 30 - (level * 2);
        if (fallSpeed < 5) fallSpeed = 5;
    }
}

void createNewTetromino() {
    currentTetromino = nextTetromino;
    currentTetromino.pos.x = (BOARD_WIDTH / 2) - 2;
    currentTetromino.pos.y = 0;

    nextTetromino.type = randInt(0, 6);
    nextTetromino.shape = aTetromino[nextTetromino.type];
    nextTetromino.rotation = 0;

    if (checkCollision(&currentTetromino)) {
        isGameOver = true;
    }
}

void resetGame() {
    memset(gameBoard, 0, sizeof(gameBoard));
    score = 0;
    level = 1;
    linesCleared = 0;
    isGameOver = false;
    isPaused = false;
    fallSpeed = 30;

    nextTetromino.type = randInt(0, 6);
    nextTetromino.shape = aTetromino[nextTetromino.type];
    nextTetromino.rotation = 0;
    createNewTetromino();
}

// --- Input and Game Loop ---

// TODO: Refactoring
KEY_Code_t GetKeyTetris() {
    KEY_Code_t btn = KEYBOARD_Poll();
    if (btn == KEY_INVALID && !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT)) {
        btn = KEY_PTT;
    }
    return btn;
}

void HandleUserInputTetris() {
    kbd.prev = kbd.current;
    kbd.current = GetKeyTetris();

    if (kbd.current != KEY_INVALID && kbd.current != kbd.prev) {
        if (isGameOver) {
            if (kbd.current == KEY_MENU || kbd.current == KEY_EXIT) {
                resetGame();
            }
            return;
        }

        if (kbd.current == KEY_MENU) {
            isPaused = !isPaused;
            return;
        }

        if (isPaused) return;

        Tetromino temp = currentTetromino;

        switch (kbd.current) {
            case KEY_2: // Rotate
                temp.rotation = (temp.rotation + 1) % 4;
                break;
            case KEY_4:
                temp.pos.x--;
                break;
            case KEY_6:
                temp.pos.x++;
                break;
            case KEY_8:
                temp.pos.y++;
                break;
            case KEY_PTT: // Drop
                 while(!checkCollision(&temp)) {
                    currentTetromino.pos.y = temp.pos.y;
                    temp.pos.y++;
                }
                fallCounter = fallSpeed; // Force immediate lock
                return;
            case KEY_EXIT:
                break;
            default:
                break;
        }

        if (!checkCollision(&temp)) {
            currentTetromino = temp;
        }
    }
}

void updateGame() {
    if (isGameOver || isPaused) return;

    fallCounter++;
    if (fallCounter >= fallSpeed) {
        fallCounter = 0;
        Tetromino temp = currentTetromino;
        temp.pos.y++;

        if (checkCollision(&temp)) {
            mergeTetromino();
            clearLines();
            createNewTetromino();
        } else {
            currentTetromino = temp;
        }
    }
}

void renderGame() {
    UI_DisplayClear();
    drawBoard();
    drawTetromino(&currentTetromino, true);
    drawScoreTetris();

    if (isGameOver) {
        UI_PrintStringSmallBold("GAME OVER", 20, 0, 4);
    } else if (isPaused) {
        UI_PrintStringSmallBold("PAUSED", 32, 0, 4);
    }

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

void APP_RunTetris(void) {
    srand_custom(BK4819_ReadRegister(BK4819_REG_67) * gBatteryVoltageAverage);

    resetGame();

    while (true) {
        HandleUserInputTetris();
        if (kbd.current == KEY_EXIT && kbd.prev == kbd.current) {
            break;
        }
        updateGame();
        renderGame();
        SYSTEM_DelayMs(10);
    }
}
