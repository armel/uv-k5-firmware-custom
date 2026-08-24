#!/usr/bin/env python3

import os
import sys
import time
import datetime
import argparse

os.environ["PYGAME_HIDE_SUPPORT_PROMPT"] = "hide"

import pygame
import serial
from serial.tools import list_ports

# Version
VERSION = '1.0'

# Serial configuration
DEFAULT_PORT = '/dev/ttyUSB0'  # Change if needed (/dev/cu.usbserial-11130)
BAUDRATE = 38400
TIMEOUT = 0.5

# Screen configuration
WIDTH, HEIGHT = 128, 64
FRAME_SIZE = 1024

# Protocol
HEADER = b'\xAA\x55'
TYPE_SCREENSHOT = b'\x01'
TYPE_DIFF = b'\x02'

# Framebuffer
framebuffer = bytearray([0] * FRAME_SIZE)

DEBUG = False


COLOR_SETS = {  # {key: (name, foreground, background)}
    "g": ("Grey", pygame.Color(0, 0, 0), pygame.Color(202, 202, 202)),
    "o": ("Orange", pygame.Color(0, 0, 0), pygame.Color(255, 193, 37)),
    "b": ("Blue", pygame.Color(0, 0, 0), pygame.Color(28, 134, 228)),
    "w": ("White", pygame.Color(0, 0, 0), pygame.Color(255, 255, 255)),
}

DEFAULT_COLOR = "g"  # Must be a key of "COLOR_SETS"

def send_keepalive(ser: serial.Serial):
    # Send keepalive frame
    try:
        n = ser.write(b'\x55\xAA\x00\x00')  # Keepalive frame
        if DEBUG:
            print(f"[debug] sent keepalive, {n} bytes written, in_waiting={ser.in_waiting}")
    except serial.SerialException as e:
        if DEBUG:
            print(f"[debug] keepalive write failed: {e}")

def read_frame(ser: serial.Serial) -> bytearray:
    global framebuffer
    while True:
        try:
            b = ser.read(1)
        except serial.SerialException as e:
            #print(f"[ERROR] Serial read failed: {e}")
            print("[!] Your USB serial cable is probably being used by another application such as Chirp or Chrome.")
            sys.exit(1)
        if not b:
            if DEBUG:
                print("[debug] read timeout, no byte received")
            return None
        if DEBUG:
            print(f"[debug] byte: {b.hex()}")
        if b == HEADER[0:1]:
            b2 = ser.read(1)
            if DEBUG:
                print(f"[debug] header[0] matched, byte2: {b2.hex() if b2 else '(timeout)'}")
            if b2 == HEADER[1:2]:
                t = ser.read(1)
                size_bytes = ser.read(2)
                size = int.from_bytes(size_bytes, 'big')
                if DEBUG:
                    print(f"[debug] header matched: type={t.hex() if t else '(timeout)'} size={size}")
                if t == TYPE_SCREENSHOT and size == FRAME_SIZE:
                    payload = ser.read(FRAME_SIZE)
                    framebuffer = bytearray(payload)
                    if DEBUG:
                        print(f"[debug] got full screenshot frame, {len(payload)} bytes")
                    return framebuffer
                elif t == TYPE_DIFF and size % 9 == 0:
                    payload = ser.read(size)
                    framebuffer = apply_diff(framebuffer, payload)
                    if DEBUG:
                        print(f"[debug] got diff frame, {len(payload)} bytes")
                    return framebuffer
                elif DEBUG:
                    print(f"[debug] unrecognized type/size combo, dropping")


def apply_diff(framebuffer: bytearray, diff_payload: bytes) -> bytearray:
    i = 0
    while i + 9 <= len(diff_payload):
        block_index = diff_payload[i]
        i += 1
        if block_index >= 128:
            break
        framebuffer[block_index * 8 : block_index * 8 + 8] = diff_payload[i : i + 8]
        i += 8
    return framebuffer


def draw_frame(screen: pygame.Surface, framebuffer: bytearray, bg_color: pygame.Color, fg_color: pygame.Color, pixel_size: int = 4, pixel_lcd: int = 0) -> pygame.Surface:
    def get_bit(bit_idx):
        byte_idx = bit_idx // 8
        bit_pos = bit_idx % 8
        if byte_idx < len(framebuffer):
            return (framebuffer[byte_idx] >> bit_pos) & 0x01
        return 0

    screen.fill(bg_color)
    bit_index = 0
    for y in range(64):
        for x in range(128):
            if get_bit(bit_index):
                px = x * (pixel_size - 1)
                py = y * pixel_size
                pygame.draw.rect(screen, fg_color, (px, py, pixel_size - 1 - pixel_lcd, pixel_size - pixel_lcd))
            bit_index += 1

    pygame.display.flip()
    return pygame.display.get_surface().copy()


def run_viewer(args: argparse.Namespace, ser: serial.Serial):
    pixel_size = 5
    pixel_lcd = 0
    pygame.init()
    screen = pygame.display.set_mode((WIDTH * (pixel_size - 1), HEIGHT * pixel_size))
    base_title = f"Quansheng K5Viewer v{VERSION} by F4HWN"
    pygame.display.set_caption(f"{base_title} – No data")

    fg_color, bg_color = COLOR_SETS[DEFAULT_COLOR][1:]
    last_surface = None
    frame_count = 0
    frame_lost = 0
    last_time = time.monotonic()

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                raise KeyboardInterrupt
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    raise KeyboardInterrupt
                if event.key == pygame.K_SPACE and last_surface:
                    filename = datetime.datetime.now().strftime("screenshot_%Y%m%d_%H%M%S.png")
                    pygame.image.save(last_surface, filename)
                    print(f"[✔] Screenshot saved: {filename}")
                elif event.key == pygame.K_p:
                    pixel_lcd = 1 - pixel_lcd
                    draw_frame(screen, framebuffer, bg_color, fg_color, pixel_size, pixel_lcd)
                elif event.key == pygame.K_i:
                    if bg_color == pygame.Color(0, 0, 0):
                        bg_color, fg_color = fg_color, pygame.Color(0, 0, 0)
                    else:
                        bg_color, fg_color = pygame.Color(0, 0, 0), bg_color
                    draw_frame(screen, framebuffer, bg_color, fg_color, pixel_size, pixel_lcd)
                elif event.key == pygame.K_UP:
                    if pixel_size < 12:
                        pixel_size += 1
                    #print(f"[✔] Resize: {pixel_size, (WIDTH * (pixel_size - 1)), HEIGHT * pixel_size}")
                    screen = pygame.display.set_mode((WIDTH * (pixel_size - 1), HEIGHT * pixel_size))
                    draw_frame(screen, framebuffer, bg_color, fg_color, pixel_size, pixel_lcd)
                elif event.key == pygame.K_DOWN:
                    if pixel_size > 3:
                        pixel_size -= 1
                    #print(f"[✔] Resize: {pixel_size, (WIDTH * (pixel_size - 1)), HEIGHT * pixel_size}")
                    screen = pygame.display.set_mode((WIDTH * (pixel_size - 1), HEIGHT * pixel_size))
                    draw_frame(screen, framebuffer, bg_color, fg_color, pixel_size, pixel_lcd)
                pressed_key = event.unicode
                if pressed_key in COLOR_SETS.keys():
                    fg_color, bg_color = COLOR_SETS[pressed_key][1:]
        frame = read_frame(ser)
        if frame:
            last_surface = draw_frame(screen, framebuffer, bg_color, fg_color, pixel_size, pixel_lcd)
            frame_count += 1
            now = time.monotonic()
            if now - last_time >= 1.0:
                fps = frame_count / (now - last_time)
                pygame.display.set_caption(f"{base_title} – FPS: {fps:>04.1f}")
                frame_count = 0
                last_time = now
                frame_lost = 0
        else:
            frame_lost = min(frame_lost + 1, 5)
            if frame_lost == 5:
                pygame.display.set_caption(f"{base_title} – No data")

        send_keepalive(ser)


def cmd_list_ports(args: argparse.Namespace):
    ports = list_ports.comports()
    print("Available ports:")
    for port in ports:
        if port.vid is None:  # Skipping virtual or non-USB ports
            continue
        description = " - ".join(filter(None, (port.product, port.manufacturer)))
        if description:
            print(f"- {description} : {port.device}")
        else:
            print(f"- {port.device}")


def main():
    parser = argparse.ArgumentParser(
        prog="K5Viewer",
        description="A live viewer for UV-K5 radios with F4HWN firmware",
        epilog="F4HWN repo: https://github.com/armel/uv-k5-firmware-custom"
    )
    parser.add_argument("--list-ports", action="store_true", help="list available ports and exit")
    parser.add_argument("--port", type=str, help="serial port to use (in place of 'DEFAULT_PORT')")
    parser.add_argument("--debug", action="store_true", help="print raw serial byte/frame debug info")
    parser.add_argument("--version", action="version", version=f"%(prog)s {VERSION}", help="show program's version number and exit")

    args = parser.parse_args()
    global DEBUG
    DEBUG = args.debug
    if args.list_ports:
        cmd_list_ports(args)
        exit(0)
    # Running viewer
    if not args.port and not DEFAULT_PORT:
        print("Please specify the serial port to use or set 'DEFAULT_PORT', do 'k5viewer.py --help' for help")
        exit(1)
    serial_port = args.port or DEFAULT_PORT
    try:
        ser = serial.Serial(serial_port, BAUDRATE, timeout=TIMEOUT)
    except serial.SerialException as e:
        print(f"[!] Serial error: {e}")
        sys.exit(1)
    try:
        run_viewer(args, ser)
    except KeyboardInterrupt:
        print("[✔] Exiting")
        ser.close()
        pygame.quit()
        sys.exit()


if __name__ == "__main__":
    main()