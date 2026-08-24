#!/usr/bin/env python3
"""Visual on-screen keypad for the UV-K5 Renode emulator.

Renders a clickable keypad mirroring driver/keyboard.c's row/column layout
exactly (see KEY_GRID below -- row 0's SIDE1/SIDE2 placement matches the
real hardware's "no row line actually driven" special case documented in
emulator/peripherals/gpioa_bitbang.py), and forwards clicks -- plus real-
keyboard shortcuts -- as 'KEYDOWN <name>' / 'KEYUP <name>' lines to the
emulator's GPIOA peripheral, which is listening on a local socket.

Labels are drawn with a tiny hand-rolled 5x7 bitmap font instead of
pygame.font: some pygame builds on this project's dev machines don't ship a
working font module (missing SDL_ttf), which used to make this window
either blank or crash outright. A fixed pixel font sidesteps that platform
quirk entirely, so the keypad looks the same everywhere.

Usage: keyboard_bridge.py [--port 9812]
"""

import argparse
import os
import socket
import time

os.environ["PYGAME_HIDE_SUPPORT_PROMPT"] = "hide"
import pygame

# Mirrors driver/keyboard.c's keyboard[] table exactly: row 0 has only two
# real keys (SIDE1/SIDE2), the other two grid cells are unused on real
# hardware.
KEY_GRID = [
    ["SIDE1", "SIDE2", None, None],
    ["MENU", "1", "4", "7"],
    ["UP", "2", "5", "8"],
    ["DOWN", "3", "6", "9"],
    ["EXIT", "STAR", "0", "F"],
]

# Real-keyboard shortcuts, so quick tests don't require reaching for the mouse.
KEY_MAP = {
    pygame.K_UP: "UP",
    pygame.K_DOWN: "DOWN",
    pygame.K_RETURN: "MENU",
    pygame.K_KP_ENTER: "MENU",
    pygame.K_ESCAPE: "EXIT",
    pygame.K_BACKSPACE: "EXIT",
    pygame.K_ASTERISK: "STAR",
    pygame.K_KP_MULTIPLY: "STAR",
    pygame.K_f: "F",
    pygame.K_LEFTBRACKET: "SIDE1",
    pygame.K_RIGHTBRACKET: "SIDE2",
    pygame.K_0: "0", pygame.K_KP0: "0",
    pygame.K_1: "1", pygame.K_KP1: "1",
    pygame.K_2: "2", pygame.K_KP2: "2",
    pygame.K_3: "3", pygame.K_KP3: "3",
    pygame.K_4: "4", pygame.K_KP4: "4",
    pygame.K_5: "5", pygame.K_KP5: "5",
    pygame.K_6: "6", pygame.K_KP6: "6",
    pygame.K_7: "7", pygame.K_KP7: "7",
    pygame.K_8: "8", pygame.K_KP8: "8",
    pygame.K_9: "9", pygame.K_KP9: "9",
}

# Minimal 5x7 bitmap font -- just the glyphs this UI's labels need.
FONT_5X7 = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10101", "10011", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "/": ["00001", "00001", "00010", "00100", "01000", "10000", "10000"],
    "[": ["01110", "01000", "01000", "01000", "01000", "01000", "01110"],
    "]": ["01110", "00010", "00010", "00010", "00010", "00010", "01110"],
}


def draw_text(surface, text, x, y, scale, color):
    cursor_x = x
    for ch in text.upper():
        glyph = FONT_5X7.get(ch, FONT_5X7[" "])
        for row_idx, row in enumerate(glyph):
            for col_idx, bit in enumerate(row):
                if bit == "1":
                    surface.fill(color, (cursor_x + col_idx * scale, y + row_idx * scale, scale, scale))
        cursor_x += 6 * scale


def text_width(text, scale):
    return len(text) * 6 * scale


BG = (30, 30, 30)
BTN = (70, 70, 75)
BTN_HELD = (60, 160, 90)
BTN_BORDER = (110, 110, 115)
TEXT = (230, 230, 230)
LEGEND_TEXT = (150, 150, 150)

MARGIN = 16
BTN_W, BTN_H, GAP = 76, 48, 8
COLS, ROWS = 4, 5
GRID_W = COLS * BTN_W + (COLS - 1) * GAP
GRID_H = ROWS * BTN_H + (ROWS - 1) * GAP
HEADER_H = 24
LEGEND_LINES = [
    "KEYBOARD SHORTCUTS:",
    "ARROWS UP/DOWN  ENTER MENU  ESC EXIT",
    "* STAR  F F  [ / ] SIDE1/SIDE2  0-9",
]
LEGEND_H = len(LEGEND_LINES) * 16 + 8

WIN_W = GRID_W + 2 * MARGIN
WIN_H = HEADER_H + GRID_H + LEGEND_H + 3 * MARGIN


def build_buttons():
    buttons = []
    top = MARGIN + HEADER_H
    for r, row in enumerate(KEY_GRID):
        for c, key_name in enumerate(row):
            if key_name is None:
                continue
            rect = pygame.Rect(MARGIN + c * (BTN_W + GAP), top + r * (BTN_H + GAP), BTN_W, BTN_H)
            buttons.append((key_name, rect))
    return buttons


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9812)
    args = parser.parse_args()

    sock = None
    for attempt in range(20):
        try:
            sock = socket.create_connection((args.host, args.port), timeout=2)
            break
        except (ConnectionRefusedError, OSError):
            time.sleep(0.5)
    if sock is None:
        raise SystemExit("could not connect to emulator keyboard socket at %s:%d "
                          "(is the emulator running?)" % (args.host, args.port))

    def send(action, name):
        try:
            sock.sendall(("%s %s\n" % (action, name)).encode())
        except OSError:
            pass

    pygame.init()
    screen = pygame.display.set_mode((WIN_W, WIN_H))
    pygame.display.set_caption("UV-K5 Emulator Keypad")

    buttons = build_buttons()
    active_keys = set()
    mouse_down_key = None

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

            elif event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                for key_name, rect in buttons:
                    if rect.collidepoint(event.pos):
                        mouse_down_key = key_name
                        active_keys.add(key_name)
                        send("KEYDOWN", key_name)
                        break

            elif event.type == pygame.MOUSEBUTTONUP and event.button == 1:
                if mouse_down_key is not None:
                    active_keys.discard(mouse_down_key)
                    send("KEYUP", mouse_down_key)
                    mouse_down_key = None

            elif event.type == pygame.KEYDOWN:
                name = KEY_MAP.get(event.key)
                if name:
                    active_keys.add(name)
                    send("KEYDOWN", name)

            elif event.type == pygame.KEYUP:
                name = KEY_MAP.get(event.key)
                if name:
                    active_keys.discard(name)
                    send("KEYUP", name)

        screen.fill(BG)
        draw_text(screen, "UV-K5 EMULATOR KEYPAD", MARGIN, MARGIN // 2, 2, TEXT)

        for key_name, rect in buttons:
            held = key_name in active_keys
            pygame.draw.rect(screen, BTN_HELD if held else BTN, rect, border_radius=6)
            pygame.draw.rect(screen, BTN_BORDER, rect, width=1, border_radius=6)
            scale = 2 if len(key_name) <= 2 else 1
            tw = text_width(key_name, scale)
            th = 7 * scale
            draw_text(screen, key_name, rect.centerx - tw // 2, rect.centery - th // 2, scale, TEXT)

        legend_y = MARGIN + HEADER_H + GRID_H + MARGIN
        for i, line in enumerate(LEGEND_LINES):
            draw_text(screen, line, MARGIN, legend_y + i * 16, 1, LEGEND_TEXT)

        pygame.display.flip()
        pygame.time.wait(16)

    if mouse_down_key is not None:
        send("KEYUP", mouse_down_key)
    sock.close()
    pygame.quit()


if __name__ == "__main__":
    main()
