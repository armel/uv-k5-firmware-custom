# GPIOA: 2-register bank (DATA @ 0x0, DIR @ 0x4, bsp/dp32g030/gpio.h) hosting
# TWO things the real hardware wires to this port, on overlapping pins
# (confirmed in driver/gpio.h -- "Shared with I2C!" / "Shared with voice
# chip!"):
#
#   - The EEPROM, bit-banged I2C on pins 10 (SCL) / 11 (SDA) -- see
#     driver/i2c.c and driver/eeprom.c. Slave address 0xA0 (write) / 0xA1
#     (read), 16-bit big-endian memory address, sequential byte access.
#   - The 4x4-ish keyboard matrix: row outputs on pins 10-13, column inputs
#     on pins 3-6 -- see driver/keyboard.c's `keyboard[]` table, mirrored
#     below as KEY_MAP.
#
# Keyboard row-selects toggle bits 10/11 (the same physical pins as I2C
# SCL/SDA), which does make the I2C watcher see spurious START/STOP-shaped
# transitions on every keyboard scan -- traced by hand against the exact
# scan sequence in driver/keyboard.c and confirmed harmless: every spurious
# START is followed by a spurious STOP a step or two later, before 8 bits
# can ever accumulate into a real byte, and KEYBOARD_Poll()'s own trailing
# I2C_Stop() call cleans up the bus state after every single scan anyway.
# Real I2C transactions and keyboard scans never interleave (single-threaded
# firmware, no real interrupts -- see the architecture doc), so there's no
# risk of a genuine EEPROM transaction being corrupted by a scan landing
# mid-transaction.
#
# DIR bit convention (bsp/dp32g030/gpio.h): 1 = output, 0 = input.

import os
import socket
import threading

PIN_I2C_SCL = 10
PIN_I2C_SDA = 11
PIN_KB_COL0 = 3
PIN_KB_COL1 = 4
PIN_KB_COL2 = 5
PIN_KB_COL3 = 6

OFFSET_DATA = 0x0
OFFSET_DIR = 0x4

EEPROM_SIZE = 0x2000  # 8KB, matches EEPROM_WriteBuffer's own address bound check
EEPROM_FILE = os.environ.get("UVK5_EEPROM_FILE", "")
KEY_BRIDGE_PORT = int(os.environ.get("UVK5_KEYBOARD_PORT", "9812"))

# Mirrors driver/keyboard.c's `keyboard[]` table exactly: row index -> which
# key is wired to each of the 4 column pins. Row 0 is the SIDE1/SIDE2 special
# case (no row line actually driven low -- see the long comment above).
KEY_MAP = {
    (0, PIN_KB_COL0): "SIDE1",
    (0, PIN_KB_COL1): "SIDE2",
    (1, PIN_KB_COL0): "MENU",
    (1, PIN_KB_COL1): "1",
    (1, PIN_KB_COL2): "4",
    (1, PIN_KB_COL3): "7",
    (2, PIN_KB_COL0): "UP",
    (2, PIN_KB_COL1): "2",
    (2, PIN_KB_COL2): "5",
    (2, PIN_KB_COL3): "8",
    (3, PIN_KB_COL0): "DOWN",
    (3, PIN_KB_COL1): "3",
    (3, PIN_KB_COL2): "6",
    (3, PIN_KB_COL3): "9",
    (4, PIN_KB_COL0): "EXIT",
    (4, PIN_KB_COL1): "STAR",
    (4, PIN_KB_COL2): "0",
    (4, PIN_KB_COL3): "F",
}


def get_bit(value, bit):
    return (value >> bit) & 1


def set_bit(value, bit, bitval):
    if bitval:
        return value | (1 << bit)
    return value & ~(1 << bit)


def load_eeprom():
    data = bytearray([0xFF]) * EEPROM_SIZE
    if EEPROM_FILE and os.path.isfile(EEPROM_FILE):
        with open(EEPROM_FILE, "rb") as f:
            contents = f.read(EEPROM_SIZE)
            data[: len(contents)] = contents
    return data


def save_eeprom(data):
    if not EEPROM_FILE:
        return
    directory = os.path.dirname(EEPROM_FILE)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    with open(EEPROM_FILE, "wb") as f:
        f.write(bytes(data))


def start_keyboard_bridge(held_keys, lock):
    """Background thread: accepts one client (host_tools/keyboard_bridge.py)
    and applies its 'KEYDOWN <name>' / 'KEYUP <name>' lines to held_keys.
    Best-effort -- if sockets/threading don't work in this context, the
    keyboard peripheral degrades to "no keys ever pressed" rather than
    crashing peripheral init."""

    def serve():
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", KEY_BRIDGE_PORT))
        srv.listen(1)
        while True:
            conn, _ = srv.accept()
            buf = b""
            try:
                while True:
                    chunk = conn.recv(256)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        parts = line.decode(errors="ignore").strip().split()
                        if len(parts) == 2:
                            action, name = parts
                            with lock:
                                if action == "KEYDOWN":
                                    held_keys.add(name)
                                elif action == "KEYUP":
                                    held_keys.discard(name)
            finally:
                conn.close()

    t = threading.Thread(target=serve)
    t.daemon = True
    t.start()


if request.IsInit:
    data = (1 << PIN_I2C_SCL) | (1 << PIN_I2C_SDA)
    direction = (1 << PIN_I2C_SCL) | (1 << PIN_I2C_SDA)  # SDA/SCL start as outputs

    eeprom = load_eeprom()

    # I2C/EEPROM watcher state.
    i2c_phase = "IDLE"  # IDLE, ADDR, MEMADDR_HI, MEMADDR_LO, WRITE_BYTE, READ_BYTE
    shift_in = 0
    bit_count = 0
    mem_addr = 0
    addr_hi = 0
    awaiting_ack = False
    read_bits_served = 0
    current_read_byte = 0

    # Keyboard watcher state.
    active_row = 0  # row 0 (SIDE1/SIDE2) is the correct default/no-row-selected state
    held_keys = set()
    held_keys_lock = threading.Lock()
    try:
        start_keyboard_bridge(held_keys, held_keys_lock)
    except Exception as e:
        self.WarningLog("keyboard bridge socket failed to start: %s" % str(e))

elif request.IsWrite:
    old_data = data
    if request.Offset == OFFSET_DIR:
        old_dir = direction
        direction = request.Value
        # Ack phase ends the moment firmware hands SDA back to the master
        # (flips it from input back to output) -- see driver/i2c.c's
        # I2C_Write()/I2C_Read() tail end.
        if get_bit(old_dir, PIN_I2C_SDA) == 0 and get_bit(direction, PIN_I2C_SDA) == 1:
            awaiting_ack = False
    elif request.Offset == OFFSET_DATA:
        data = request.Value
        scl_now = get_bit(data, PIN_I2C_SCL)
        scl_was = get_bit(old_data, PIN_I2C_SCL)
        sda_now = get_bit(data, PIN_I2C_SDA)
        sda_was = get_bit(old_data, PIN_I2C_SDA)

        # --- I2C/EEPROM watcher ---
        if scl_was == 1 and scl_now == 1 and sda_was == 0 and sda_now == 1:
            i2c_phase = "IDLE"
            bit_count = 0
            awaiting_ack = False
        elif scl_was == 1 and scl_now == 1 and sda_was == 1 and sda_now == 0:
            i2c_phase = "ADDR"
            bit_count = 0
            shift_in = 0
            awaiting_ack = False
        elif scl_was == 0 and scl_now == 1:
            if i2c_phase in ("ADDR", "MEMADDR_HI", "MEMADDR_LO", "WRITE_BYTE"):
                shift_in = ((shift_in << 1) | sda_now) & 0xFF
                bit_count += 1
                if bit_count == 8:
                    bit_count = 0
                    awaiting_ack = True
                    if i2c_phase == "ADDR":
                        if shift_in == 0xA1:
                            i2c_phase = "READ_BYTE"
                            read_bits_served = 0
                            current_read_byte = eeprom[mem_addr % EEPROM_SIZE]
                        else:  # 0xA0 (write) -- this firmware never uses any other address
                            i2c_phase = "MEMADDR_HI"
                    elif i2c_phase == "MEMADDR_HI":
                        addr_hi = shift_in
                        i2c_phase = "MEMADDR_LO"
                    elif i2c_phase == "MEMADDR_LO":
                        mem_addr = ((addr_hi << 8) | shift_in) % EEPROM_SIZE
                        i2c_phase = "WRITE_BYTE"
                    elif i2c_phase == "WRITE_BYTE":
                        eeprom[mem_addr] = shift_in
                        save_eeprom(eeprom)
                        mem_addr = (mem_addr + 1) % EEPROM_SIZE
            elif i2c_phase == "READ_BYTE":
                if read_bits_served == 8:
                    mem_addr = (mem_addr + 1) % EEPROM_SIZE
                    current_read_byte = eeprom[mem_addr]
                    read_bits_served = 0
                read_bits_served += 1

        # --- Keyboard watcher ---
        # Row 0 (SIDE1/SIDE2) is the special case where NO row line is
        # pulled low at all: driver/keyboard.c's row-0 mask is 0xffff,
        # which in C only clears bits 16-31 of a 32-bit register and
        # leaves bits 10-13 untouched -- SIDE1/SIDE2 are wired straight to
        # columns 3/4 with no row-select dependency. Getting this backwards
        # (treating "no bits low" as "no row active" and leaving columns
        # floating at whatever `data` happened to hold) reads as a phantom
        # SIDE1 press from the very first GPIOA read, which is exactly
        # what made main.c's boot-splash wait loop
        # (`while (boot_counter_10ms > 0) if (KEYBOARD_Poll() != KEY_INVALID) ...`)
        # exit immediately on every boot.
        rows_low = [b for b in (10, 11, 12, 13) if get_bit(data, b) == 0]
        if len(rows_low) == 1:
            active_row = rows_low[0] - 9  # bit10->row1, 11->row2, 12->row3, 13->row4
        else:
            active_row = 0

elif request.IsRead:
    if request.Offset == OFFSET_DIR:
        request.Value = direction
    elif request.Offset == OFFSET_DATA:
        value = data
        if get_bit(direction, PIN_I2C_SDA) == 0:
            if awaiting_ack:
                value = set_bit(value, PIN_I2C_SDA, 0)
            elif i2c_phase == "READ_BYTE" and 1 <= read_bits_served <= 8:
                bit_val = (current_read_byte >> (8 - read_bits_served)) & 1
                value = set_bit(value, PIN_I2C_SDA, bit_val)
        with held_keys_lock:
            pressed = set(held_keys)
        for col_pin in (PIN_KB_COL0, PIN_KB_COL1, PIN_KB_COL2, PIN_KB_COL3):
            key_name = KEY_MAP.get((active_row, col_pin))
            if key_name is not None and key_name in pressed:
                value = set_bit(value, col_pin, 0)
            elif get_bit(direction, col_pin) == 0:
                value = set_bit(value, col_pin, 1)
        request.Value = value
    else:
        request.Value = 0
