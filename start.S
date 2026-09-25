# Copyright 2023 Dual Tachyon
# https://github.com/DualTachyon
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

	.cpu cortex-m0
	.fpu softvfp
	.thumb

	.global Stack
	.global HandlerReset
	.global Main

	.global OVERLAY_Install
	.global BOARD_FLASH_Init
	.global BSS_Init

	.global SystickHandler
	.weak SystickHandler

	.section .text.isr

Stack:
	.long	0x20003FF0
Reset:
	.long	HandlerReset + 1
NMI:
	.long	HandlerNMI + 1
HardFault:
	.long	HandlerHardFault + 1
Reserved_10:
	.long	0
Reserved_14:
	.long	0
Reserved_18:
	.long	0
Reserved_1C:
	.long	0
Reserved_20:
	.long	0
Reserved_24:
	.long	0
Reserved_28:
	.long	0
SVCall:
	.long	HandlerSVCall + 1
Reserved_30:
	.long	0
Reserved_34:
	.long	0
PendSV:
	.long	HandlerPendSV + 1
SysTick:
	.long	SystickHandler + 1
WWDT:
	.long	HandlerWWDT + 1
IWDT:
	.long	HandlerIWDT + 1
RTC:
	.long	HandlerRTC + 1
DMA:
	.long	HandlerDMA + 1
SARADC:
	.long	HandlerSARADC + 1
TIMER_BASE0:
	.long	HandlerTIMER_BASE0 + 1
TIMER_BASE1:
	.long	HandlerTIMER_BASE1 + 1
TIMER_PLUS0:
	.long	HandlerTIMER_PLUS0 + 1
TIMER_PLUS1:
	.long	HandlerTIMER_PLUS1 + 1
PWM_BASE0:
	.long	HandlerPWM_BASE0 + 1
PWM_BASE1:
	.long	HandlerPWM_BASE1 + 1
PWM_PLUS0:
	.long	HandlerPWM_PLUS0 + 1
PWM_PLUS1:
	.long	HandlerPWM_PLUS1 + 1
UART0:
	.long	HandlerUART0 + 1
UART1:
	.long	HandlerUART1 + 1
UART2:
	.long	HandlerUART2 + 1
SPI0:
	.long	HandlerSPI0 + 1
SPI1:
	.long	HandlerSPI1 + 1
IIC0:
	.long	HandlerIIC0 + 1
IIC1:
	.long	HandlerIIC1 + 1
CMP:
	.long	HandlerCMP + 1
TIMER_BASE2:
	.long	HandlerTIMER_BASE2 + 1
GPIOA5:
	.long	HandlerGPIOA5 + 1
GPIOA6:
	.long	HandlerGPIOA6 + 1
GPIOA7:
	.long	HandlerGPIOA7 + 1
GPIOB0:
	.long	HandlerGPIOB0 + 1
GPIOB1:
	.long	HandlerGPIOB1 + 1
GPIOC0:
	.long	HandlerGPIOC0 + 1
GPIOC1:
	.long	HandlerGPIOC1 + 1
GPIOA:
	.long	HandlerGPIOA + 1
GPIOB:
	.long	HandlerGPIOB + 1
GPIOC:
	.long	HandlerGPIOC + 1

	.section .text

# All unused exceptions/interrupts share one endless loop (was one per vector)
HandlerNMI:
HandlerHardFault:
HandlerSVCall:
HandlerPendSV:
HandlerWWDT:
HandlerIWDT:
HandlerRTC:
HandlerDMA:
HandlerSARADC:
HandlerTIMER_BASE0:
HandlerTIMER_BASE1:
HandlerTIMER_PLUS0:
HandlerTIMER_PLUS1:
HandlerPWM_BASE0:
HandlerPWM_BASE1:
HandlerPWM_PLUS0:
HandlerPWM_PLUS1:
HandlerUART0:
HandlerUART1:
HandlerUART2:
HandlerSPI0:
HandlerSPI1:
HandlerIIC0:
HandlerIIC1:
HandlerCMP:
HandlerTIMER_BASE2:
HandlerGPIOA5:
HandlerGPIOA6:
HandlerGPIOA7:
HandlerGPIOB0:
HandlerGPIOB1:
HandlerGPIOC0:
HandlerGPIOC1:
HandlerGPIOA:
HandlerGPIOB:
HandlerGPIOC:
	b	.

SystickHandler:
	bx	lr

HandlerReset:
	ldr	r0, =0x20003FF0
	mov	sp, r0
	bl	DATA_Init
	bl	BSS_Init
#if defined(ENABLE_OVERLAY)
	bl	BOARD_FLASH_Init
#endif
	bl	Main
	b 	.

