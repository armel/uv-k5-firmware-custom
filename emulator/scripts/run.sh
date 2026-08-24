#!/usr/bin/env bash
# Boots a compiled UV-K5 firmware .elf (or .bin, see below) in Renode.
#
# Usage: emulator/scripts/run.sh path/to/firmware.elf [eeprom_file]
#
# If given a .bin instead of a .elf, this still works IF a matching .elf
# with debug symbols sits next to it (the normal Makefile output does: e.g.
# f4hwn.custom and f4hwn.custom.bin are both produced from the same link
# step) -- display_viewer.py needs the .elf specifically to resolve
# gFrameBuffer/gStatusLine's addresses via `nm`.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 path/to/firmware.elf [eeprom_file]" >&2
    exit 1
fi

FIRMWARE_ELF="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
EEPROM_FILE="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMU_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GENERATED_DIR="$EMU_DIR/.generated"
mkdir -p "$GENERATED_DIR"

GENERATED_REPL="$GENERATED_DIR/uvk5.repl"
sed "s#@@EMU_DIR@@#$EMU_DIR#g" "$EMU_DIR/platform/uvk5.repl.in" > "$GENERATED_REPL"

if [ -z "$EEPROM_FILE" ]; then
    EEPROM_FILE="$GENERATED_DIR/eeprom.bin"
fi
export UVK5_EEPROM_FILE="$EEPROM_FILE"
export UVK5_KEYBOARD_PORT="${UVK5_KEYBOARD_PORT:-9812}"

MONITOR_PORT="${UVK5_MONITOR_PORT:-8888}"

echo "Firmware:    $FIRMWARE_ELF"
echo "EEPROM file: $EEPROM_FILE"
echo "Monitor:     telnet 127.0.0.1 $MONITOR_PORT"
echo "Keyboard:    127.0.0.1 $UVK5_KEYBOARD_PORT"
echo
echo "In another terminal, once this is running:"
echo "  $EMU_DIR/host_tools/display_viewer.py --elf \"$FIRMWARE_ELF\" --monitor-port $MONITOR_PORT"
echo "  $EMU_DIR/host_tools/keyboard_bridge.py --port \"$UVK5_KEYBOARD_PORT\""
echo

exec renode --disable-xwt -P "$MONITOR_PORT" \
    -e "mach create \"uvk5\"" \
    -e "machine LoadPlatformDescription @$GENERATED_REPL" \
    -e "sysbus LoadELF @$FIRMWARE_ELF" \
    -e "start"
