#ifdef ENABLE_FLASHLIGHT

#include "driver/gpio.h"
#include "bsp/dp32g030/gpio.h"

#ifndef ENABLE_FEAT_F4HWN
    enum FlashlightMode_t  gFlashLightState;

    void FlashlightTimeSlice()
    {
        if (gFlashLightState == FLASHLIGHT_BLINK && (gFlashLightBlinkCounter & 15u) == 0) {
            GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
            return;
        }

        if (gFlashLightState == FLASHLIGHT_SOS) {
            const uint16_t u = 15;
            static uint8_t c;
            static uint16_t next;

            if (gFlashLightBlinkCounter - next > 7 * u) {
                c = 0;
                next = gFlashLightBlinkCounter + 1;
                return;
            }

            if (gFlashLightBlinkCounter == next) {
                if (c==0) {
                    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
                } else {
                    GPIO_FlipBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
                }

                if (c >= 18) {
                    next = gFlashLightBlinkCounter + 7 * u;
                    c = 0;
                } else if(c==7 || c==9 || c==11) {
                    next = gFlashLightBlinkCounter + 3 * u;
                } else {
                    next = gFlashLightBlinkCounter + u;
                }
                c++;
            }
        }
    }

    void ACTION_FlashLight(void)
    {
        switch (gFlashLightState) {
            case FLASHLIGHT_OFF:
                gFlashLightState++;
                GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
                break;
            case FLASHLIGHT_ON:
            case FLASHLIGHT_BLINK:
                gFlashLightState++;
                break;
            case FLASHLIGHT_SOS:
            default:
                gFlashLightState = 0;
                GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
        }
    }

#else

/* Copyright 2024 Roberto A. Foglietta
 * https://github.com/robang74
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

#include "flashlight.h"

void ACTION_FlashLight(void)
{
    if(bitchk(BF_FLASHLIGHT)) {
        GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
    } else {
        GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_FLASHLIGHT);
    }
    bitflp(BF_FLASHLIGHT);
}

#endif

#endif
