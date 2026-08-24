# GPIOC: 2-register bank (DATA @ 0x0, DIR @ 0x4, bsp/dp32g030/gpio.h) hosting
# two things the real hardware wires to this port:
#
#   - The BK4819 RF chip, bit-banged on pins 0/1/2 (SCN=chip-select,
#     SCL=clock, SDA=data) -- see driver/bk4819.c's BK4819_WriteRegister /
#     BK4819_ReadRegister / BK4819_WriteU8 / BK4819_WriteU16 / BK4819_ReadU16.
#     This is a shallow Phase 1 stub: a register-value dict with read-after-
#     write round-tripping, no RF/DTMF/FSK behavior. BK4819_REG_0C (the
#     interrupt-status register CheckRadioInterrupts() polls) is never
#     written by firmware, so it defaults to 0 via the dict's .get(..., 0) --
#     which is exactly "nothing pending," so that polling loop always exits
#     immediately with no special-casing needed.
#   - PTT, a plain input on pin 5 (active low -- 1 = not pressed).
#
# DIR bit convention (bsp/dp32g030/gpio.h): 1 = output, 0 = input.

PIN_SCN = 0
PIN_SCL = 1
PIN_SDA = 2
PIN_PTT = 5

OFFSET_DATA = 0x0
OFFSET_DIR = 0x4


def get_bit(value, bit):
    return (value >> bit) & 1


def set_bit(value, bit, bitval):
    if bitval:
        return value | (1 << bit)
    return value & ~(1 << bit)


if request.IsInit:
    data = (1 << PIN_SCN) | (1 << PIN_SDA) | (1 << PIN_PTT)  # idle-high defaults
    direction = (1 << PIN_SCN) | (1 << PIN_SCL) | (1 << PIN_SDA)  # SDA starts as output

    registers = {}  # BK4819 7-bit register address -> 16-bit value

    # "phase" tracks where we are in a single BK4819 transaction:
    #   IDLE, ADDR (shifting in the 8-bit reg-address+R/W byte),
    #   WRITE_DATA (shifting in 16 bits to store), READ_WAIT_DIR (address
    #   byte done, waiting for firmware to flip SDA to input),
    #   READ_DATA (shifting 16 bits back out).
    phase = "IDLE"
    bit_count = 0
    shift_in = 0
    current_addr = 0
    read_value = 0
    read_bit_index = 0

elif request.IsWrite:
    old_data = data
    old_dir = direction
    if request.Offset == OFFSET_DIR:
        direction = request.Value
        # Direction flipping SDA to input right after a fully-shifted
        # address byte with the read bit set is the firmware's signal that
        # it's about to clock 16 read bits back out.
        if phase == "READ_WAIT_DIR" and get_bit(direction, PIN_SDA) == 0:
            phase = "READ_DATA"
            read_bit_index = 0
            read_value = registers.get(current_addr, 0)
    elif request.Offset == OFFSET_DATA:
        data = request.Value
        scn_now = get_bit(data, PIN_SCN)
        scn_was = get_bit(old_data, PIN_SCN)
        scl_now = get_bit(data, PIN_SCL)
        scl_was = get_bit(old_data, PIN_SCL)
        sda_now = get_bit(data, PIN_SDA)

        if scn_was == 0 and scn_now == 1:
            # Deselect: end of transaction, always safe to reset.
            phase = "IDLE"
            bit_count = 0
        elif scn_now == 0 and scn_was == 1:
            # Fresh chip-select assert: start of a new transaction.
            phase = "ADDR"
            bit_count = 0
            shift_in = 0
        elif scl_was == 0 and scl_now == 1:
            # Rising SCL edge: data was set up by the master before this
            # edge (WriteU8/WriteU16), or is being consumed by the master
            # right after it (ReadU16) -- either way, this is the one
            # instant we advance the bit count for both directions.
            if phase == "ADDR":
                shift_in = ((shift_in << 1) | sda_now) & 0xFF
                bit_count += 1
                if bit_count == 8:
                    is_read = (shift_in & 0x80) != 0
                    current_addr = shift_in & 0x7F
                    bit_count = 0
                    if is_read:
                        phase = "READ_WAIT_DIR"
                    else:
                        phase = "WRITE_DATA"
                        shift_in = 0
            elif phase == "WRITE_DATA":
                shift_in = ((shift_in << 1) | sda_now) & 0xFFFF
                bit_count += 1
                if bit_count == 16:
                    registers[current_addr] = shift_in
                    phase = "IDLE"
                    bit_count = 0
            elif phase == "READ_DATA":
                if read_bit_index < 16:
                    read_bit_index += 1

elif request.IsRead:
    if request.Offset == OFFSET_DIR:
        request.Value = direction
    elif request.Offset == OFFSET_DATA:
        value = data
        # Output-configured bits (per DIR) echo back what was last written;
        # PTT and (while mid-read) SDA are the only inputs we compute.
        if get_bit(direction, PIN_PTT) == 0:
            value = set_bit(value, PIN_PTT, 1)  # never pressed in Phase 1
        if phase == "READ_DATA" and get_bit(direction, PIN_SDA) == 0:
            if read_bit_index < 16:
                bit_val = (read_value >> (15 - read_bit_index)) & 1
            else:
                bit_val = 0
            value = set_bit(value, PIN_SDA, bit_val)
        request.Value = value
    else:
        request.Value = 0
