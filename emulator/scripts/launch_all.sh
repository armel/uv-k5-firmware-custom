#!/usr/bin/env bash
# One-command orchestration for the UV-K5 emulator: starts Renode, the LCD
# viewer, and the keyboard bridge together (the manual flow in README.md is
# the same three commands run by hand in three terminals). Also supports
# running the Robot Framework smoke test instead of the interactive session.
#
# Usage:
#   emulator/scripts/launch_all.sh path/to/firmware.elf [options]
#
# Options:
#   --eeprom PATH     EEPROM persistence file (default: emulator/.generated/eeprom.bin)
#   --monitor-port N  Renode monitor port (default: 8888)
#   --keyboard-port N Keyboard bridge socket port (default: 9812)
#   --no-keyboard     Start Renode + viewer only (view-only, no keypress injection)
#   --test            Run emulator/tests/smoke_boot.robot against this .elf and exit
#                      (no viewer/keyboard, no long-running processes)
#
# Ctrl+C stops everything this script started (Renode, viewer, keyboard bridge).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMU_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$EMU_DIR/.." && pwd)"
GENERATED_DIR="$EMU_DIR/.generated"
mkdir -p "$GENERATED_DIR"

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

if [ $# -lt 1 ]; then
    echo "Available firmware .elf files in $REPO_DIR:" >&2
    find "$REPO_DIR" -maxdepth 1 -name "f4hwn.*" ! -name "*.bin" ! -name "*.packed*" -exec basename {} \; >&2 || true
    echo >&2
    usage
fi

FIRMWARE_ELF="$1"; shift
FIRMWARE_ELF="$(cd "$(dirname "$FIRMWARE_ELF")" && pwd)/$(basename "$FIRMWARE_ELF")"
if [ ! -f "$FIRMWARE_ELF" ]; then
    echo "error: firmware .elf not found: $FIRMWARE_ELF" >&2
    exit 1
fi

EEPROM_FILE="$GENERATED_DIR/eeprom.bin"
MONITOR_PORT=8888
KEYBOARD_PORT=9812
WITH_KEYBOARD=1
TEST_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --eeprom) EEPROM_FILE="$2"; shift 2 ;;
        --monitor-port) MONITOR_PORT="$2"; shift 2 ;;
        --keyboard-port) KEYBOARD_PORT="$2"; shift 2 ;;
        --no-keyboard) WITH_KEYBOARD=0; shift ;;
        --test) TEST_ONLY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; usage ;;
    esac
done

command -v renode >/dev/null 2>&1 || {
    echo "error: 'renode' not found on PATH -- see emulator/README.md Setup (brew install renode/tap/renode)" >&2
    exit 1
}
python3 -c "import pygame" 2>/dev/null || {
    echo "error: pygame not importable by python3 -- see emulator/README.md Setup (pip3 install pygame)" >&2
    exit 1
}

if [ "$TEST_ONLY" = "1" ]; then
    command -v renode-test >/dev/null 2>&1 || {
        echo "error: 'renode-test' not found on PATH (should ship alongside renode)" >&2
        exit 1
    }
    FB_ADDR="$(nm "$FIRMWARE_ELF" | awk '$3=="gFrameBuffer"{print "0x"$1}')"
    if [ -z "$FB_ADDR" ]; then
        echo "error: could not resolve gFrameBuffer in $FIRMWARE_ELF via nm" >&2
        exit 1
    fi
    echo "Running smoke_boot.robot against $FIRMWARE_ELF (gFrameBuffer @ $FB_ADDR)..."
    exec renode-test "$EMU_DIR/tests/smoke_boot.robot" \
        --variable FIRMWARE_ELF:"$FIRMWARE_ELF" \
        --variable FRAMEBUFFER_ADDR:"$FB_ADDR"
fi

port_in_use() {
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sys.exit(0 if s.connect_ex(('127.0.0.1', $1)) == 0 else 1)
"
}

if port_in_use "$MONITOR_PORT"; then
    echo "error: something is already listening on 127.0.0.1:$MONITOR_PORT" \
         "(a previous emulator run?). Stop it first or pass --monitor-port." >&2
    exit 1
fi

PIDS=()
cleanup() {
    echo
    echo "Shutting down..."
    # Renode's own wrapper script (libexec/renode) launches the actual
    # `dotnet .../Renode.dll` process as a plain child rather than exec'ing
    # into it, so run.sh's `exec renode ...` only gets us as far as that
    # wrapper -- killing $pid alone leaves Renode.dll running, orphaned.
    # Kill direct children first, then the tracked pid itself, with a SIGKILL
    # sweep after a short grace period for anything that ignored SIGTERM.
    for pid in "${PIDS[@]:-}"; do
        pkill -P "$pid" 2>/dev/null || true
        kill "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "${PIDS[@]:-}"; do
        pkill -9 -P "$pid" 2>/dev/null || true
        kill -9 "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

RENODE_LOG="$GENERATED_DIR/renode.log"
echo "Starting Renode (log: $RENODE_LOG)..."
UVK5_KEYBOARD_PORT="$KEYBOARD_PORT" UVK5_MONITOR_PORT="$MONITOR_PORT" \
    "$SCRIPT_DIR/run.sh" "$FIRMWARE_ELF" "$EEPROM_FILE" > "$RENODE_LOG" 2>&1 &
PIDS+=($!)

echo -n "Waiting for Renode monitor on port $MONITOR_PORT..."
for _ in $(seq 1 60); do
    if port_in_use "$MONITOR_PORT"; then
        echo " up."
        break
    fi
    echo -n "."
    sleep 0.5
done
if ! port_in_use "$MONITOR_PORT"; then
    echo
    echo "error: Renode never opened the monitor port -- check $RENODE_LOG" >&2
    exit 1
fi

echo "Starting display viewer..."
python3 "$EMU_DIR/host_tools/display_viewer.py" --elf "$FIRMWARE_ELF" --monitor-port "$MONITOR_PORT" &
PIDS+=($!)

if [ "$WITH_KEYBOARD" = "1" ]; then
    echo "Starting keyboard bridge on port $KEYBOARD_PORT..."
    echo "(click the keyboard bridge window, then type -- see its on-screen help)"
    python3 "$EMU_DIR/host_tools/keyboard_bridge.py" --port "$KEYBOARD_PORT" &
    PIDS+=($!)
fi

echo
echo "Everything is running. Press Ctrl+C here to stop the whole session."
wait -n "${PIDS[@]}"
