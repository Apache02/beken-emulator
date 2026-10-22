#!/usr/bin/env bash
# Check 3: bkloader reboots the chip into the bootloader shell, loads an IRAM
# image that erases and programs one flash sector, runs it and captures its
# report. When the first attempt fails, it is retried once on the same
# emulator (no restart); a note is printed when the retry was needed.
set -eu
. "$(dirname "$0")/lib.sh"
need_bkloader
need_file "$IRAM_BIN"

start_emulator
capture_start
# Workaround: bkloader is started only after the app reports its shell. When
# it was started during the hand-over from the bootloader to the app, the app
# printed stale '>' prompt lines on starting to read the input sent in that
# window, and the tool took one of them for the bootloader prompt (see
# README.md, "Starting bkloader against a booted device").
wait_for 'RAM loader shell is ready\.' 30
capture_stop

attempt() {
  local out=$WORK_DIR/bkloader-$1.out
  run_bkloader 60 "$out" iram --speed 921600 --port "$PORT" --capture 6 --until "==END==" "$IRAM_BIN" \
    || { echo "attempt $1: $BKLOADER_ERROR"; return 1; }
  grep -Eq '(^|[^0-9])6 / 6 passed' "$out" || { echo "attempt $1: no '6 / 6 passed' in the bkloader output"; return 1; }
  grep -q '==END==' "$out" || { echo "attempt $1: no '==END==' in the bkloader output"; return 1; }
}

if attempt 1; then
  pass "6 / 6 passed, ==END=="
elif attempt 2; then
  echo "NOTE $CHECK_NAME: the first attempt failed; the retry on the same emulator passed"
  pass "6 / 6 passed, ==END== (after one retry on the same emulator)"
else
  fail "bkloader iram failed twice on the same emulator"
fi
