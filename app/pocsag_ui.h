/* POCSAG pager UI: multi-tap text entry and a transmit screen.
 *
 * Reached by holding 0 on the main screen, which is free in the default build
 * because that slot only does anything when the FM radio is compiled in.
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

#ifndef POCSAG_UI_H
#define POCSAG_UI_H

// Takes over the display and keypad until the user presses EXIT, then hands
// the radio back to the main screen.
void APP_RunPocsag(void);

#endif
