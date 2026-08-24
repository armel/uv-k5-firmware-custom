# SARADC (battery voltage/current ADC). board.c's BOARD_ADC_GetBatteryInfo()
# does `while (!ADC_CheckEndOfConversion(ADC_CH9)) {}` -- an *unbounded* loop
# that hangs boot forever unless channel 9's STAT.EOC bit reads 1 immediately.
# We take the simple route and make every channel always report "conversion
# complete" with a fixed mid-scale reading, since nothing in Phase 1's scope
# needs a specific battery percentage to be correct -- just present and sane.
#
# Register layout (bsp/dp32g030/saradc.h): CFG=0x00, START=0x04, IE=0x08,
# IF=0x0C, then ADC_Channel_t {STAT, DATA} pairs starting at 0x10, 8 bytes
# apart -- channel N is at 0x10 + 8*N (confirmed against driver/adc.c's
# `(ADC_Channel_t*)&SARADC_CH0)[channel]` indexing). Only channels 4 (battery
# voltage) and 9 (battery current) are ever read by this firmware, but we
# make EVERY channel behave the same way rather than special-casing just
# those two, in case a future firmware change reads another channel.

CHANNEL_BASE = 0x10
CHANNEL_STRIDE = 0x08

# 12-bit ADC reading, mid-scale. Tune this (and per-channel values below) if
# the emulated UI shows an implausible battery percentage/voltage -- the
# actual displayed value also depends on calibration constants loaded from
# EEPROM (see eeprom's blank-EEPROM fallback behavior in gpioa_bitbang.py).
DEFAULT_ADC_VALUE = 2048

if request.IsInit:
    regs = {}
elif request.IsWrite:
    regs[request.Offset] = request.Value
elif request.IsRead:
    if request.Offset >= CHANNEL_BASE:
        rel = request.Offset - CHANNEL_BASE
        channel = rel // CHANNEL_STRIDE
        reg_in_channel = rel % CHANNEL_STRIDE
        if reg_in_channel == 0:
            request.Value = 1  # STAT.EOC always set
        elif reg_in_channel == 4:
            request.Value = DEFAULT_ADC_VALUE  # DATA
        else:
            request.Value = regs.get(request.Offset, 0)
    else:
        request.Value = regs.get(request.Offset, 0)
