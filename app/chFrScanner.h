#ifndef APP_CHFRSCANNER_H
#define APP_CHFRSCANNER_H

#include <stdbool.h>
#include <stdint.h>

// scan direction, if not equal SCAN_OFF indicates 
// that we are in a process of scanning channels/frequencies
extern int8_t            gScanStateDir;
extern bool              gScanKeepResult;
extern bool              gScanPauseMode;

#ifdef ENABLE_SCAN_RANGES
extern uint32_t          gScanRangeStart;
extern uint32_t          gScanRangeStop;
#endif

void CHFRSCANNER_Found(void);
void CHFRSCANNER_Stop(void);
void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction);
void CHFRSCANNER_ContinueScanning(void);

#ifdef ENABLE_FEAT_FAST_SCAN
// true while the chip is in the quick-scan state (squelch interrupts off, retuned
// behind gRxVfo's back); RADIO_SetupRegisters() clears it
extern bool              gFastScanOwnsChip;
#endif

#ifdef ENABLE_FEAT_F4HWN
    extern uint32_t lastFoundFrqOrChan;
    extern uint32_t lastFoundFrqOrChanOld;
#endif

#endif