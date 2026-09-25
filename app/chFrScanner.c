
#include "app/app.h"
#include "app/chFrScanner.h"
#include "functions.h"
#include "misc.h"
#include "settings.h"
#ifdef ENABLE_FEAT_FAST_SCAN
    #include <string.h>
    #include "audio.h"
    #include "driver/bk4819.h"
    #include "driver/eeprom.h"
    #include "frequencies.h"
    #include "radio.h"
#endif
//#include "debugging.h"

int8_t            gScanStateDir;
bool              gScanKeepResult;
bool              gScanPauseMode;

#ifdef ENABLE_SCAN_RANGES
uint32_t          gScanRangeStart;
uint32_t          gScanRangeStop;
#endif

typedef enum {
    SCAN_NEXT_CHAN_SCANLIST1 = 0,
    SCAN_NEXT_CHAN_SCANLIST2,
    SCAN_NEXT_CHAN_DUAL_WATCH,
    SCAN_NEXT_CHAN_MR,
    SCAN_NEXT_NUM
} scan_next_chan_t;

scan_next_chan_t    currentScanList;
uint32_t            initialFrqOrChan;
uint8_t             initialCROSS_BAND_RX_TX;

#ifndef ENABLE_FEAT_F4HWN
    uint32_t lastFoundFrqOrChan;
#else
    uint32_t lastFoundFrqOrChan;
    uint32_t lastFoundFrqOrChanOld;
#endif

static void NextFreqChannel(void);
static void NextMemChannel(void);
static void NextMemChannelOnce(void);

#ifdef ENABLE_FEAT_FAST_SCAN
#define FAST_SCAN_RSSI_MARGIN   12    // 0.5 dB steps, 6 dB
#define FAST_SCAN_NOISE_MARGIN  16    // ex-noise units, 0 ~ 127
#define FAST_SCAN_GLITCH_MARGIN 10    // glitch units, 0 ~ 255
#define FAST_SCAN_DISPLAY_10MS  20    // show the channel being checked every 200 ms

static uint32_t fs_cache[MR_CHANNEL_LAST + 1];   // RX freq, bit 31 = full look only, 0 = not read yet
bool            gFastScanOwnsChip;               // see chFrScanner.h
static int8_t   fs_path = -1;                    // VHF (1) / UHF (0) filter path set by the quick path
static uint16_t fs_reg30;                        // REG_30 as the last full setup left it
static uint8_t  fs_list = 0xFF;                  // scan list the screen last showed
static bool     fs_show_now;                     // show the next channel without waiting

// Quick look at a memory channel, in two halves so the ~10 ms wait for the chip's
// glitch result does not block the main loop:
//  FastScanStart()  retune only (as spectrum.c SetF() does); FS_WAIT = result pending,
//                   FS_LOOK = give this channel the full stock look (squelch 0, and
//                   channels the quick retune would tune wrong - see the cache)
//  FastScanResult() one glitch read per call; FS_WAIT until it is valid, then FS_QUIET
//                   only when the stock squelch clearly could not open (one of its three
//                   gates - RSSI above, noise below, glitch below the open threshold -
//                   fails by a margin), else FS_LOOK. The stock squelch has the final say.
enum { FS_WAIT, FS_QUIET, FS_LOOK };

static bool     fs_pending;        // a quick look is under way on gNextMrChannel
static uint16_t fs_pending_prev;   // gNextMrChannel before that channel was picked
static uint16_t fs_pending_t10;    // gFlashLightBlinkCounter at the retune
static uint8_t  fs_pending_band;   // 0 VHF, 1 UHF thresholds
static uint8_t  fs_rssi_open[2];   // squelch-open thresholds per band [VHF, UHF], as
static uint8_t  fs_noise_open[2];  // RADIO_ConfigureSquelchAndOutputPower() gives them
static uint8_t  fs_glitch_open[2]; // to the chip

static int8_t FastScanStart(const uint8_t chan)
{
    static uint8_t sql_level;
    uint8_t        data[13];

    if (gEeprom.SQUELCH_LEVEL == 0)
        return FS_LOOK;

    // re-read the thresholds only when the squelch level changes
    if (sql_level != gEeprom.SQUELCH_LEVEL) {
        sql_level = gEeprom.SQUELCH_LEVEL;
        for (uint8_t i = 0; i < 2; i++) {
            const uint16_t base = (i ? 0x1E00 : 0x1E60) + sql_level;
            EEPROM_ReadBuffer(base + 0x00, &fs_rssi_open[i],   1);
            EEPROM_ReadBuffer(base + 0x20, &fs_noise_open[i],  1);
            EEPROM_ReadBuffer(base + 0x50, &fs_glitch_open[i], 1);
#ifdef ENABLE_SQUELCH_MORE_SENSITIVE
            fs_rssi_open[i]   = fs_rssi_open[i] / 2;
            fs_noise_open[i]  = (fs_noise_open[i] * 2 > 127) ? 127 : fs_noise_open[i] * 2;
            fs_glitch_open[i] = (fs_glitch_open[i] * 2 > 255) ? 255 : fs_glitch_open[i] * 2;
#endif
        }
    }

    uint32_t freq = fs_cache[chan];
    if (freq == 0) {
        // one read: RX frequency in [0..3], modulation in the high nibble of [11],
        // reverse in bit 0 of [12] (0xFF = unset). Full look only where the quick retune
        // would not match RADIO_ConfigureChannel(): not FM, reverse (RX on the TX
        // frequency), 350-400 MHz while disabled (it moves those to 433 MHz)
        EEPROM_ReadBuffer(chan * 16, data, sizeof(data));
        freq = data[0] | (data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        if ((data[11] >> 4) != MODULATION_FM || (data[12] != 0xFF && (data[12] & 1u)) ||
            (!gSetting_350EN && freq >= 35000000 && freq < 40000000))
            freq |= 1u << 31;
        fs_cache[chan] = freq;
    }
    if (freq >> 31)
        return FS_LOOK;
    fs_pending_band = FREQUENCY_GetBand(freq) >= BAND4_174MHz;

    // retune and restart the RX chain (as spectrum.c SetF() does)
    BK4819_SetFrequency(freq);
    const int8_t path = freq < 28000000;
    if (path != fs_path) {
        BK4819_PickRXFilterPathBasedOnFrequency(freq);
        fs_path = path;
    }
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30, fs_reg30);

    fs_pending_t10 = gFlashLightBlinkCounter;
    return FS_WAIT;
}

// Take the chip over from the stock setup (scan start, full look, monitor, end of RX)
// before the first quick retune. Leaves things as RADIO_SetupRegisters(true) would,
// but with no squelch interrupts while the chip hops
static void FastScanTakeOver(void)
{
    // the speaker and RX LED may still be on from a receive or monitor, which the stock
    // step would switch off; left on, the hopping is heard as a loud buzz
    AUDIO_AudioPathOff();
    gEnableSpeaker = false;
    BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, false);
    if (gCurrentFunction != FUNCTION_FOREGROUND)
        FUNCTION_Select(FUNCTION_FOREGROUND);
    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
    BK4819_WriteRegister(BK4819_REG_3F, 0);  // no squelch interrupts while the chip hops
    // drop anything the chip latched before the mask, and clear the squelch/CSS flags
    for (uint8_t i = 0; i < 10 && (BK4819_ReadRegister(BK4819_REG_0C) & 1u); i++)
        BK4819_WriteRegister(BK4819_REG_02, 0);
    FUNCTION_Init();
    fs_reg30 = BK4819_ReadRegister(BK4819_REG_30);
    fs_path  = -1;   // the stock setup picked the filter path for its own channel
    gFastScanOwnsChip = true;
}

static int8_t FastScanResult(void)
{
    // glitch reads 255 until its first measurement window after the retune ends
    // (~10.5 ms on radio 1); RSSI and noise have settled by then
    const uint8_t glitch = BK4819_GetGlitchIndicator();
    if (glitch >= 255) {
        // never became valid within 50 ms: let the stock look decide
        return ((uint16_t)(gFlashLightBlinkCounter - fs_pending_t10) > 5) ? FS_LOOK : FS_WAIT;
    }
    const uint16_t rssi  = BK4819_GetRSSI();
    const uint8_t  noise = BK4819_GetExNoiceIndicator();
    const uint8_t  b     = fs_pending_band;

    const bool quiet = rssi + FAST_SCAN_RSSI_MARGIN < fs_rssi_open[b] ||
        noise  >= fs_noise_open[b]  + FAST_SCAN_NOISE_MARGIN ||
        glitch >= fs_glitch_open[b] + FAST_SCAN_GLITCH_MARGIN;

    return quiet ? FS_QUIET : FS_LOOK;
}
#endif

void CHFRSCANNER_Start(const bool storeBackupSettings, const int8_t scan_direction)
{
#ifdef ENABLE_FEAT_FAST_SCAN
    memset(fs_cache, 0, sizeof(fs_cache));   // channels may have been edited since the last scan
    fs_pending = false;
#endif
    if (storeBackupSettings) {
        initialCROSS_BAND_RX_TX = gEeprom.CROSS_BAND_RX_TX;
        gEeprom.CROSS_BAND_RX_TX = CROSS_BAND_OFF;
        gScanKeepResult = false;
    }
    
    RADIO_SelectVfos();

    gNextMrChannel   = gRxVfo->CHANNEL_SAVE;
    currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
    gScanStateDir    = scan_direction;

    if (IS_MR_CHANNEL(gNextMrChannel))
    {   // channel mode
        if (storeBackupSettings) {
            initialFrqOrChan = gRxVfo->CHANNEL_SAVE;
            lastFoundFrqOrChan = initialFrqOrChan;
        }
        NextMemChannel();
    }
    else
    {   // frequency mode
        if (storeBackupSettings) {
            initialFrqOrChan = gRxVfo->freq_config_RX.Frequency;
            lastFoundFrqOrChan = initialFrqOrChan;
        }
        NextFreqChannel();
    }

#ifdef ENABLE_FEAT_F4HWN
    lastFoundFrqOrChanOld = lastFoundFrqOrChan;
#endif

    gScanPauseDelayIn_10ms = scan_pause_delay_in_2_10ms;
    gScheduleScanListen    = false;
    gRxReceptionMode       = RX_MODE_NONE;
    gScanPauseMode         = false;
}

/*
void CHFRSCANNER_ContinueScanning(void)
{
    if (IS_FREQ_CHANNEL(gNextMrChannel))
    {
        if (gCurrentFunction == FUNCTION_INCOMING)
            APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
        else
            NextFreqChannel();  // switch to next frequency
    }
    else
    {
        if (gCurrentCodeType == CODE_TYPE_OFF && gCurrentFunction == FUNCTION_INCOMING)
            APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
        else
            NextMemChannel();    // switch to next channel
    }
    
    gScanPauseMode      = false;
    gRxReceptionMode    = RX_MODE_NONE;
    gScheduleScanListen = false;
}
*/

void CHFRSCANNER_ContinueScanning(void)
{
    // clear before stepping: the step may set a pause as short as 1 tick, and
    // clearing afterwards could erase the flag that pause already raised
    gScheduleScanListen = false;

    if (gCurrentFunction == FUNCTION_INCOMING &&
        (IS_FREQ_CHANNEL(gNextMrChannel) || gCurrentCodeType == CODE_TYPE_OFF))
    {
        APP_StartListening(gMonitor ? FUNCTION_MONITOR : FUNCTION_RECEIVE);
    }
    else
    {
        IS_FREQ_CHANNEL(gNextMrChannel) ? NextFreqChannel() : NextMemChannel();
    }

    gScanPauseMode      = false;
    gRxReceptionMode    = RX_MODE_NONE;
}

void CHFRSCANNER_Found(void)
{
    if (gEeprom.SCAN_RESUME_MODE > 80) {
        if (!gScanPauseMode) {
            gScanPauseDelayIn_10ms = scan_pause_delay_in_5_10ms * (gEeprom.SCAN_RESUME_MODE - 80) * 5;
            gScanPauseMode = true;
        }
    } else {
        gScanPauseDelayIn_10ms = 0;
    }

    // gScheduleScanListen is always false...
    gScheduleScanListen = false;

    /*
    if(gEeprom.SCAN_RESUME_MODE > 1 && gEeprom.SCAN_RESUME_MODE < 26)
    {
        if (!gScanPauseMode)
        {
            gScanPauseDelayIn_10ms = scan_pause_delay_in_5_10ms * (gEeprom.SCAN_RESUME_MODE - 1) * 5;
            gScheduleScanListen    = false;
            gScanPauseMode         = true;
        }
    }
    else
    {
        gScanPauseDelayIn_10ms = 0;
        gScheduleScanListen    = false;
    }
    */

    /*
    switch (gEeprom.SCAN_RESUME_MODE)
    {
        case SCAN_RESUME_TO:
            if (!gScanPauseMode)
            {
                gScanPauseDelayIn_10ms = scan_pause_delay_in_1_10ms;
                gScheduleScanListen    = false;
                gScanPauseMode         = true;
            }
            break;

        case SCAN_RESUME_CO:
        case SCAN_RESUME_SE:
            gScanPauseDelayIn_10ms = 0;
            gScheduleScanListen    = false;
            break;
    }
    */

#ifdef ENABLE_FEAT_F4HWN
    lastFoundFrqOrChanOld = lastFoundFrqOrChan;
#endif

    if (IS_MR_CHANNEL(gRxVfo->CHANNEL_SAVE)) { //memory scan
        lastFoundFrqOrChan = gRxVfo->CHANNEL_SAVE;
    }
    else { // frequency scan
        lastFoundFrqOrChan = gRxVfo->freq_config_RX.Frequency;
    }

    gScanKeepResult = true;
}

void CHFRSCANNER_Stop(void)
{
    if(initialCROSS_BAND_RX_TX != CROSS_BAND_OFF) {
        gEeprom.CROSS_BAND_RX_TX = initialCROSS_BAND_RX_TX;
        initialCROSS_BAND_RX_TX = CROSS_BAND_OFF;
    }
    
    gScanStateDir = SCAN_OFF;

    const uint32_t chFr = gScanKeepResult ? lastFoundFrqOrChan : initialFrqOrChan;
    const bool channelChanged = chFr != initialFrqOrChan;
    if (IS_MR_CHANNEL(gNextMrChannel)) {
        gEeprom.MrChannel[gEeprom.RX_VFO]     = chFr;
        gEeprom.ScreenChannel[gEeprom.RX_VFO] = chFr;
        RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);

        if(channelChanged) {
            SETTINGS_SaveVfoIndices();
            gUpdateStatus = true;
        }
    }
    else {
        gRxVfo->freq_config_RX.Frequency = chFr;
        RADIO_ApplyOffset(gRxVfo);
        RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
        if(channelChanged) {
            SETTINGS_SaveChannel(gRxVfo->CHANNEL_SAVE, gEeprom.RX_VFO, gRxVfo, 1);
        }
    }

    #ifdef ENABLE_FEAT_F4HWN_RESUME_STATE
        gEeprom.CURRENT_STATE = 0;
        SETTINGS_WriteCurrentState();
    #endif

    RADIO_SetupRegisters(true);
    gUpdateDisplay = true;
}

static void NextFreqChannel(void)
{
#ifdef ENABLE_SCAN_RANGES
    if(gScanRangeStart) {
        gRxVfo->freq_config_RX.Frequency = APP_SetFreqByStepAndLimits(gRxVfo, gScanStateDir, gScanRangeStart, gScanRangeStop);
    }
    else
#endif
        gRxVfo->freq_config_RX.Frequency = APP_SetFrequencyByStep(gRxVfo, gScanStateDir);

    RADIO_ApplyOffset(gRxVfo);
    RADIO_ConfigureSquelchAndOutputPower(gRxVfo);
    RADIO_SetupRegisters(true);

#ifdef ENABLE_FASTER_CHANNEL_SCAN
    gScanPauseDelayIn_10ms = 9;   // 90ms
#else
    gScanPauseDelayIn_10ms = scan_pause_delay_in_6_10ms;
#endif

    gUpdateDisplay     = true;
}

static void NextMemChannel(void)
{
#ifdef ENABLE_FEAT_FAST_SCAN
    if (gEeprom.SCAN_LIST_DEFAULT != fs_list) {
        // list switched (number key): the indicator is in the status bar
        fs_list       = gEeprom.SCAN_LIST_DEFAULT;
        gUpdateStatus = true;
        fs_show_now   = true;
    }
#endif
    NextMemChannelOnce();
}

static void NextMemChannelOnce(void)
{
    static unsigned int prev_mr_chan = 0;
    const bool          enabled      = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT < 4) ? gEeprom.SCAN_LIST_ENABLED[gEeprom.SCAN_LIST_DEFAULT - 1] : true;
    const int           chan1        = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT < 4) ? gEeprom.SCANLIST_PRIORITY_CH1[gEeprom.SCAN_LIST_DEFAULT - 1] : -1;
    const int           chan2        = (gEeprom.SCAN_LIST_DEFAULT > 0 && gEeprom.SCAN_LIST_DEFAULT < 4) ? gEeprom.SCANLIST_PRIORITY_CH2[gEeprom.SCAN_LIST_DEFAULT - 1] : -1;
    unsigned int        prev_chan    = gNextMrChannel;
    unsigned int        chan         = 0;
#ifdef ENABLE_FEAT_FAST_SCAN
    int8_t              fs           = FS_LOOK;

    if (fs_pending) {
        // a quick look is under way on gNextMrChannel: collect its result, unless
        // something set the chip up for another channel meanwhile (then look in full)
        fs = gFastScanOwnsChip ? FastScanResult() : FS_LOOK;
        if (fs == FS_WAIT) {
            gScanPauseDelayIn_10ms = 0;   // look again on the next main-loop pass
            gScheduleScanListen    = true;
            return;
        }
        fs_pending = false;
        prev_chan  = fs_pending_prev;
        goto fs_decided;
    }
#endif

    //char str[64] = "";

    if (enabled)
    {
        switch (currentScanList)
        {
            case SCAN_NEXT_CHAN_SCANLIST1:
                prev_mr_chan = gNextMrChannel;
    
                //sprintf(str, "-> Chan1 %d\n", chan1 + 1);
                //LogUart(str);

                if (chan1 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan1, false, gEeprom.SCAN_LIST_DEFAULT))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
                        gNextMrChannel   = chan1;
                        break;
                    }
                }

                [[fallthrough]];
            case SCAN_NEXT_CHAN_SCANLIST2:

                //sprintf(str, "-> Chan2 %d\n", chan2 + 1);
                //LogUart(str);

                if (chan2 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan2, false, gEeprom.SCAN_LIST_DEFAULT))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST2;
                        gNextMrChannel   = chan2;
                        break;
                    }
                }

                [[fallthrough]];
            /*
            case SCAN_NEXT_CHAN_SCANLIST3:
                if (chan3 >= 0)
                {
                    if (RADIO_CheckValidChannel(chan3, false, 0))
                    {
                        currentScanList = SCAN_NEXT_CHAN_SCANLIST3;
                        gNextMrChannel   = chan3;
                        break;
                    }
                }
                [[fallthrough]];
            */
            // this bit doesn't yet work if the other VFO is a frequency
            case SCAN_NEXT_CHAN_DUAL_WATCH:
                // dual watch is enabled - include the other VFO in the scan
//              if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
//              {
//                  chan = (gEeprom.RX_VFO + 1) & 1u;
//                  chan = gEeprom.ScreenChannel[chan];
//                  if (IS_MR_CHANNEL(chan))
//                  {
//                      currentScanList = SCAN_NEXT_CHAN_DUAL_WATCH;
//                      gNextMrChannel   = chan;
//                      break;
//                  }
//              }

            default:
            case SCAN_NEXT_CHAN_MR:
                currentScanList = SCAN_NEXT_CHAN_MR;
                gNextMrChannel   = prev_mr_chan;
                chan             = 0xff;
                break;
        }
    }

    if (!enabled || chan == 0xff)
    {       
        chan = RADIO_FindNextChannel(gNextMrChannel + gScanStateDir, gScanStateDir, true, gEeprom.SCAN_LIST_DEFAULT);
        if (chan == 0xFF)
        {   // no valid channel found
            chan = MR_CHANNEL_FIRST;
        }
        
        gNextMrChannel = chan;

        //sprintf(str, "----> Chan %d\n", chan + 1);
        //LogUart(str);
    }

#ifdef ENABLE_FEAT_FAST_SCAN
    if (gNextMrChannel != prev_chan) {
        if (!gFastScanOwnsChip)
            FastScanTakeOver();
        fs = FastScanStart(gNextMrChannel);
        if (fs == FS_WAIT) {
            // retuned; the glitch result is ready ~10 ms later. Go back to the main
            // loop meanwhile (keys, display) and collect it on a later pass
            fs_pending      = true;
            fs_pending_prev = prev_chan;
            gScanPauseDelayIn_10ms = 0;
            gScheduleScanListen    = true;
            return;
        }
    }

fs_decided:
    if (fs == FS_QUIET)
    {   // clearly empty: no full setup, no 90 ms wait, move on
        static uint16_t shown;
        if (fs_show_now || (uint16_t)(gFlashLightBlinkCounter - shown) >= FAST_SCAN_DISPLAY_10MS) {
            shown       = gFlashLightBlinkCounter;
            fs_show_now = false;
            gEeprom.MrChannel[    gEeprom.RX_VFO] = gNextMrChannel;
            gEeprom.ScreenChannel[gEeprom.RX_VFO] = gNextMrChannel;
            RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);
            gUpdateDisplay = true;
        }
        // step again on the next main-loop pass, no SysTick wait
        gScanPauseDelayIn_10ms = 0;
        gScheduleScanListen    = true;
        if (enabled)
            if (++currentScanList >= SCAN_NEXT_NUM)
                currentScanList = SCAN_NEXT_CHAN_SCANLIST1;
        return;
    }

    // also on the same channel (a one-channel list) if its last look was a quick one:
    // the chip is still in the quick state, deaf to the stock squelch
    if (gNextMrChannel != prev_chan || gFastScanOwnsChip)
#else
    if (gNextMrChannel != prev_chan)
#endif
    {
        gEeprom.MrChannel[    gEeprom.RX_VFO] = gNextMrChannel;
        gEeprom.ScreenChannel[gEeprom.RX_VFO] = gNextMrChannel;

        RADIO_ConfigureChannel(gEeprom.RX_VFO, VFO_CONFIGURE_RELOAD);
        RADIO_SetupRegisters(true);

        gUpdateDisplay = true;
    }

#ifdef ENABLE_FASTER_CHANNEL_SCAN
    gScanPauseDelayIn_10ms = 9;  // 90ms .. <= ~60ms it misses signals (squelch response and/or PLL lock time) ?
#else
    gScanPauseDelayIn_10ms = scan_pause_delay_in_3_10ms;
#endif

    if (enabled)
        if (++currentScanList >= SCAN_NEXT_NUM)
            currentScanList = SCAN_NEXT_CHAN_SCANLIST1;  // back round we go
}
