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

#include "app/breakout.h"

#ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
#include "screenshot.h"
#endif

static uint32_t randSeed = 1;
static uint8_t blockAnim = 0;

static bool isInitialized = false;
bool isPaused = false;
bool isBeep = false;

uint8_t levelCountBreackout = 1;

uint16_t tone = 0;
uint16_t score = 0;

int16_t ballCount = BALL_NUMBER;

char str[12];

static KeyboardState kbd = {KEY_INVALID, KEY_INVALID, 0};

Brick brick[BRICK_NUMBER];
Racket racket;
Ball ball;

const uint8_t BITMAP_block[4][15] =
{
    {
    0b00011110,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00011110,
    },
    {
    0b00011110,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00011110,
    },
    {
    0b00011110,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00011110,
    },
    {
    0b00011110,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00110001,
    0b00101001,
    0b00100101,
    0b00100011,
    0b00011110,
    }
};

const uint8_t BITMAP_blockOn[15] =
{
    0b00011110,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00111111,
    0b00011110,
};

const uint8_t BITMAP_blockEmpty[15] =
{
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
    0b00000000,
};

// Initialise seed
void srand_custom(uint32_t seed) {
    randSeed = seed;
}

// Return pseudo-random from 0 to RAND_MAX (here 32767)
int rand_custom(void) {
    randSeed = randSeed * 1103515245 + 12345;
    return (randSeed >> 16) & 0x7FFF; // 15 bits
}

// Return integer from min to max include
int randInt(int min, int max) {
    return min + (rand_custom() % (max - min + 1));
}

// Reset
void reset(void)
{
    ballCount = BALL_NUMBER;
    levelCountBreackout = 1;
    score = 0;
}

// PlayBeep
void playBeep(uint16_t tone)
{
    BK4819_PlayTone(tone, true);    // 500 Hz ON
    AUDIO_AudioPathOn();
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(100);
    BK4819_EnterTxMute();
    AUDIO_AudioPathOff();
}

// Draw score
void drawScore()
{
    // Clean status line
    memset(gStatusLine,  0, sizeof(gStatusLine));

    // Level
    sprintf(str, "Level %02u", levelCountBreackout);
    GUI_DisplaySmallest(str, 0, 1, true, true);

    // Ball
    sprintf(str, "Ball %02u", (ballCount < 0) ? 0 : ballCount);
    GUI_DisplaySmallest(str, 45, 1, true, true);

    // Score
    sprintf(str, "Score %04u", score);
    GUI_DisplaySmallest(str, 88, 1, true, true);
}

// Init ball
void initBall() {
    ball.x  = 62;
    ball.y  = 30;
    ball.w  = 3;
    ball.h  = 3;
    ball.dx = 0;
    ball.dy = 1;

    UI_DrawRectangleBuffer(gFrameBuffer, ball.x, ball.y, ball.x + ball.w -1, ball.y + ball.h - 1, true);
    UI_DrawLineBuffer(gFrameBuffer, ball.x - 1, ball.y + 1, ball.x + ball.w, ball.y + 1, true);
}

// Draw ball
void drawBall() {
    UI_DrawRectangleBuffer(gFrameBuffer, ball.x, ball.y, ball.x + ball.w -1, ball.y + ball.h -1, false);
    UI_DrawLineBuffer(gFrameBuffer, ball.x - 1, ball.y + 1, ball.x + ball.w, ball.y + 1, false);

    ball.x += ball.dx;
    ball.y += ball.dy;

    if (ball.y <= 0)  // Up
    {
        ball.dx = map(randInt(0, 7), 0, 7, 3, -3);
        ball.dy = 1;
    }
    else if (ball.x <= 2)  // Left
    {
        ball.dx = abs(ball.dx);
    } 
    else if (ball.x >= 124)  // Right
    {
        ball.dx = -abs(ball.dx);
    }
    // And now Down...
    if (ball.y == 47) {
        if (ball.x + 1 >= racket.x && ball.x - 1 <= racket.x + racket.w) {
            ball.dx = map(racket.x + racket.w - ball.x, 0, racket.w, 3, -3);
            ball.dy *= -1;
            isBeep = true;
            tone = 400;
        }
    } 
    else if(ball.y > 49) {
        ballCount--;
        UI_DisplayClear();
        drawScore();
        isBeep = true;
        tone = 800;
        if (ballCount < 0) {
            reset();
            initWall();
            drawWall();

            isPaused = true;
            UI_PrintStringSmallBold("GAME", 32, 0, 4);
            UI_PrintStringSmallBold("OVER", 66, 0, 4);
        }
        initRacket();
        initBall();
    }

    UI_DrawRectangleBuffer(gFrameBuffer, ball.x, ball.y, ball.x + ball.w -1, ball.y + ball.h - 1, true);
    UI_DrawLineBuffer(gFrameBuffer, ball.x - 1, ball.y + 1, ball.x + ball.w, ball.y + 1, true);
}

// Init wall
void initWall() {
    uint8_t offset = 6;
    uint8_t i      = 0;
    uint8_t j      = 0;
    uint8_t k      = 0;

    for (i = 0; i < BRICK_NUMBER; i++) {
        if (i % 6 == 0) {
            j = 0;
            k++;
        }

        brick[i].x       = offset + (20 * j);
        brick[i].y       = -8 + 8 * k;
        brick[i].w       = 14;
        brick[i].h       = 5;
        brick[i].s       = randInt(0, 3);
        brick[i].destroy = false;

        j++;
    }
}

// Draw wall
void drawWall() {
    uint8_t i = 0;

    for (i = 0; i < BRICK_NUMBER; i++) {
        if (brick[i].destroy == false) {
            if ((ball.x + 1 >= brick[i].x &&
                 ball.x - 1 <= brick[i].x + brick[i].w) &&
                ((ball.y + 1 >= brick[i].y && 
                  ball.y - 1 <= brick[i].y + brick[i].h))) {
                brick[i].destroy = true;
                score++;

                ball.dx = map(brick[i].x + brick[i].w - ball.x, 0, brick[i].w, 2, -2);
                ball.dy *= -1;

                BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
                memcpy(gFrameBuffer[brick[i].y / 8] + brick[i].x, BITMAP_blockOn, sizeof(BITMAP_blockOn));
                ST7565_BlitLine(brick[i].y / 8);
                playBeep(600);
                memcpy(gFrameBuffer[brick[i].y / 8] + brick[i].x, BITMAP_blockEmpty, sizeof(BITMAP_blockEmpty));
                ST7565_BlitLine(brick[i].y / 8);
                BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);

                if (score % BRICK_NUMBER == 0) {
                    levelCountBreackout++;
                    ballCount++;
                    initWall();
                    return;
                }
            }

            if (brick[i].destroy == false) {
                memcpy(gFrameBuffer[brick[i].y / 8] + brick[i].x, BITMAP_block[(brick[i].s + blockAnim)  % 4], sizeof(BITMAP_block[(brick[i].s + blockAnim)  % 4]));
            }
        }
    }
}

// Init racket
void initRacket() {
    racket.w = 24;
    racket.x = (64) - (racket.w / 2);
    racket.y = 50;
    racket.h = 2;
    racket.p = racket.x;

    UI_DrawRectangleBuffer(gFrameBuffer, racket.x + 1, racket.y, racket.x + racket.w - 2, racket.y + racket.h, true);
    UI_DrawLineBuffer(gFrameBuffer, racket.x, racket.y + 1, racket.x + racket.w - 1, racket.y + 1, true);
}

// Draw racket
void drawRacket() {
    if (racket.p != racket.x) {
        UI_DrawRectangleBuffer(gFrameBuffer, racket.p + 1, racket.y, racket.p + racket.w - 2, racket.y + racket.h, false);
        UI_DrawLineBuffer(gFrameBuffer, racket.p, racket.y + 1, racket.p + racket.w - 1, racket.y + 1, false);
        racket.p = racket.x;
        UI_DrawRectangleBuffer(gFrameBuffer, racket.x + 1, racket.y, racket.x + racket.w - 2, racket.y + racket.h, true);
        UI_DrawLineBuffer(gFrameBuffer, racket.x, racket.y + 1, racket.x + racket.w - 1, racket.y + 1, true);
    }
}

// OnKeyDown
static void OnKeyDown(uint8_t key)
{
    bool wasPaused = isPaused;
    
    switch (key)
    {
    case KEY_4:
    case KEY_UP:
        if(!isPaused && racket.x > 0)
            racket.x -= 2;
        isPaused = false;
        break;
    case KEY_0:
    case KEY_DOWN:
        if(!isPaused && racket.x < 102)
            racket.x += 2;
        isPaused = false;        
        break;
    case KEY_MENU:
        isPaused = !isPaused;
        kbd.counter = 0;
        if(isPaused)
        {
            UI_PrintStringSmallBold("PAUSE", 0, 128, 4);
        }
        break;
    case KEY_EXIT:
        isPaused = false;
        isInitialized = false;
        break;
    }
    
    if(wasPaused == true && isPaused == false)
    {
        // Clear the pause text
        for(uint8_t i = 0; i < 8; i++)
        {
            UI_DrawLineBuffer(gFrameBuffer, 32, 32 + i, 96, 32 + i, false);
        }
    }
}


// Key 
KEY_Code_t GetKey()
{
    KEY_Code_t btn = KEYBOARD_Poll();
    if (btn == KEY_INVALID && !GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_PTT))
    {
        btn = KEY_PTT;
    }
    return btn;
}

// HandleUserInput 
bool HandleUserInput()
{
    // Store previous key state
    kbd.prev = kbd.current;
    
    // Get the current key
    kbd.current = GetKey();

    if (kbd.current == KEY_INVALID)
        return true;

    // Movement keys: dispatch on every tick while held
    if (kbd.current == KEY_UP   || kbd.current == KEY_DOWN ||
        kbd.current == KEY_4    || kbd.current == KEY_0)
    {
        OnKeyDown(kbd.current);
        return true;
    }

    // Action keys (MENU, EXIT): dispatch only on rising edge
    if (kbd.current != kbd.prev)
    {
        OnKeyDown(kbd.current);
    }

    return true;
}

// Tick
static void Tick()
{
    HandleUserInput();
}

// APP_RunBreakout
void APP_RunBreakout(void) {
        static uint8_t swap = 0;

        // Init seed
        srand_custom(BK4819_ReadRegister(BK4819_REG_67) & 0x01FF * gBatteryVoltageAverage * gEeprom.VfoInfo[0].pRX->Frequency);

        // Init led
        BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);

        // Init game
        UI_DisplayClear();
        reset();
        initWall();
        initRacket();
        initBall();
        memset(gStatusLine,  0, sizeof(gStatusLine));
        isInitialized = true;

        while(isInitialized)
        {
            Tick();
            if(!isPaused)
            {
                if(swap == 0)
                {
                    blockAnim = (blockAnim + 1) % 4;
                }
                
                swap = (swap + 1) % 4;

                drawScore();
                drawWall();
                drawRacket();
                drawBall();
                   
                if(isBeep)
                {
                    playBeep(tone);
                    isBeep = false;
                }
                else
                {
                    SYSTEM_DelayMs(40 - MIN(levelCountBreackout - 1, 20)); // Add more fun...
                }
            }

            ST7565_BlitStatusLine();  // Blank status line
            ST7565_BlitFullScreen();

            #ifdef ENABLE_FEAT_F4HWN_SCREENSHOT
                if(isPaused || swap == 0)
                {
                    SCREENSHOT_Update(false);
                }
            #endif
        }
}