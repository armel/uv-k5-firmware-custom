#!/usr/bin/env python3
"""Live LCD viewer for the UV-K5 Renode emulator.

Reads gFrameBuffer/gStatusLine directly out of emulated RAM via Renode's
monitor (telnet) interface -- no SPI0 protocol decoding needed, see
emulator/README.md for why. Reuses the exact bit-unpacking/rendering
approach from k5viewer/k5viewer.py (the real serial-port viewer this
project already had), so the same 1024-byte, [statusline, framebuffer]
layout applies.

Usage:
    display_viewer.py --elf path/to/firmware.elf [--monitor-port 8888]
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

os.environ["PYGAME_HIDE_SUPPORT_PROMPT"] = "hide"
import pygame

WIDTH, HEIGHT = 128, 64
FRAME_SIZE = 1024
FRAMEBUFFER_SIZE = 896  # 7 * 128, gFrameBuffer
STATUSLINE_SIZE = 128  # gStatusLine

FG_COLOR = pygame.Color(0, 0, 0)
BG_COLOR = pygame.Color(202, 202, 202)


def resolve_symbol(elf_path, name):
    """Resolve a global's RAM address via the host's `nm` -- works on
    macOS's LLVM-based nm even for foreign-arch (ARM) ELFs, since symbol
    table parsing doesn't require understanding the target instruction
    set. If your platform's `nm` can't read the ELF, install
    arm-none-eabi-binutils and set NM=arm-none-eabi-nm."""
    nm = os.environ.get("NM", "nm")
    out = subprocess.check_output([nm, elf_path]).decode()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == name:
            return int(parts[0], 16)
    raise RuntimeError("symbol %s not found in %s (tried `%s`)" % (name, elf_path, nm))


class RenodeMonitor:
    """Minimal telnet client for Renode's monitor port."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        time.sleep(0.3)
        self.sock.recv(65536)  # discard telnet negotiation + banner

    def command(self, text):
        self.sock.sendall((text + "\n").encode())
        time.sleep(0.05)
        deadline = time.time() + 2.0
        buf = b""
        while time.time() < deadline:
            self.sock.settimeout(0.2)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            if buf.rstrip().endswith(b")"):
                break
        return buf.decode(errors="replace")

    def read_bytes(self, addr, count):
        resp = self.command(
            'python "print(\',\'.join(str(b) for b in '
            "self.Machine.SystemBus.ReadBytes(0x%x, %d)))\"" % (addr, count)
        )
        # The command text itself gets echoed back by the telnet server
        # before the real output -- and it contains a hex address literal
        # like "0x20001376", whose leading "0" alone satisfies a
        # zero-or-more-commas pattern. That silently matched *instead of*
        # the real data on every single call (re.search takes the first,
        # leftmost match), so read_bytes always "succeeded" with a bogus
        # 1-byte result and the caller's length check failed forever --
        # 100% frame-read failure, no error, blank display, every time.
        # Real reads always have at least one comma (>= 2 bytes); requiring
        # one skips straight past the echoed command's lone address digits.
        match = re.search(r"([0-9]+(?:,[0-9]+)+)", resp)
        if not match:
            return None
        return bytes(int(x) for x in match.group(1).split(","))


def draw_frame(screen, statusline, framebuffer, pixel_size=5):
    """gStatusLine/gFrameBuffer are the ST7565's native page/column-major LCD
    memory: each byte is one 128-wide column within an 8-pixel-tall "page"
    (bit 0 = top row of that page), not a row-major bitmap. k5viewer.py's
    draw_frame() (which this was originally copied from) instead expects a
    row-major bitstream -- but that's because it's fed the *wire* format
    produced by screenshot.c's getScreenShot(), which explicitly transposes
    page/column-major memory into row-major before sending over UART. Since
    we read gStatusLine/gFrameBuffer directly out of RAM, we skip that
    transpose and must decode the native page-major layout ourselves, or
    every frame renders as unreadable noise despite valid underlying data."""
    screen.fill(BG_COLOR)
    pages = [statusline] + [framebuffer[l * WIDTH:(l + 1) * WIDTH] for l in range(7)]
    for page_idx, page in enumerate(pages):
        for x in range(WIDTH):
            if x >= len(page):
                continue
            byte = page[x]
            for b in range(8):
                if (byte >> b) & 1:
                    y = page_idx * 8 + b
                    px = x * (pixel_size - 1)
                    py = y * pixel_size
                    pygame.draw.rect(screen, FG_COLOR, (px, py, pixel_size - 1, pixel_size))
    pygame.display.flip()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True, help="path to the firmware .elf")
    parser.add_argument("--monitor-host", default="127.0.0.1")
    parser.add_argument("--monitor-port", type=int, default=8888)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--dump", type=float, default=0,
                         help="every N seconds, print a status line and save the current "
                              "frame as a PNG to --dump-path (0 = disabled)")
    parser.add_argument("--dump-path", default="/tmp/uvk5_emulator_frame.bmp",
                         help="use a .bmp extension -- this pygame build can't save PNG "
                              "without SDL_image")
    args = parser.parse_args()

    framebuffer_addr = resolve_symbol(args.elf, "gFrameBuffer")
    statusline_addr = resolve_symbol(args.elf, "gStatusLine")
    contiguous = statusline_addr == framebuffer_addr + FRAMEBUFFER_SIZE
    print("gFrameBuffer @ 0x%x, gStatusLine @ 0x%x (contiguous: %s)" % (
        framebuffer_addr, statusline_addr, contiguous))

    print("Connecting to Renode monitor at %s:%d ..." % (args.monitor_host, args.monitor_port))
    monitor = RenodeMonitor(args.monitor_host, args.monitor_port)

    pygame.init()
    pixel_size = 5
    screen = pygame.display.set_mode((WIDTH * (pixel_size - 1), HEIGHT * pixel_size))
    pygame.display.set_caption("UV-K5 Emulator Display")

    period = 1.0 / args.fps
    running = True
    frame_count = 0
    fail_count = 0
    consecutive_fails = 0
    last_report = time.time()
    while running:
        start = time.time()
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        status = fb = None
        if contiguous:
            raw = monitor.read_bytes(framebuffer_addr, FRAME_SIZE)
            if raw and len(raw) == FRAME_SIZE:
                fb = raw[:FRAMEBUFFER_SIZE]
                status = raw[FRAMEBUFFER_SIZE:]
        else:
            status = monitor.read_bytes(statusline_addr, STATUSLINE_SIZE)
            fb = monitor.read_bytes(framebuffer_addr, FRAMEBUFFER_SIZE)

        if status is not None and fb is not None:
            draw_frame(screen, status, fb, pixel_size)
            frame_count += 1
            consecutive_fails = 0
        else:
            fail_count += 1
            consecutive_fails += 1
            if consecutive_fails in (1, 10, 50) or consecutive_fails % 200 == 0:
                print("[!] read_bytes failed to produce a frame (%d consecutive, %d total) "
                      "-- monitor connection or parsing issue, not a firmware problem"
                      % (consecutive_fails, fail_count))

        if args.dump and time.time() - last_report > args.dump:
            last_report = time.time()
            print("[i] %d frames drawn, %d failed reads so far" % (frame_count, fail_count))
            if status is not None and fb is not None:
                pygame.image.save(screen, args.dump_path)
                print("[i] saved current frame to %s" % args.dump_path)

        elapsed = time.time() - start
        if elapsed < period:
            time.sleep(period - elapsed)

    pygame.quit()


if __name__ == "__main__":
    try:
        main()
    except (ConnectionResetError, BrokenPipeError, OSError):
        # Expected on shutdown when Renode's monitor socket goes away out
        # from under an in-flight read (e.g. a launcher killing Renode and
        # this process at roughly the same time) -- not a real error.
        pass
    except KeyboardInterrupt:
        pass
