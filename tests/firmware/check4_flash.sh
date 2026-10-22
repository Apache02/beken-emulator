#!/usr/bin/env bash
# Check 4: bkloader writes the app image (app_crc.bin) to the app segment.
set -eu
. "$(dirname "$0")/lib.sh"
need_bkloader

start_emulator
capture_start
wait_for 'RAM loader shell is ready\.' 30
capture_stop

out=$WORK_DIR/bkloader.out
run_bkloader 120 "$out" flash --port "$PORT" --speed 921600 "$APP_BIN" \
  || fail "flash: $BKLOADER_ERROR"
grep -q 'Erased 11 sector(s)' "$out" || fail "no 'Erased 11 sector(s)' in the bkloader output"
grep -q 'Flash CRC32 OK' "$out" || fail "no 'Flash CRC32 OK' in the bkloader output"
grep -q "Wrote 42296 bytes to 'app'" "$out" || fail "no \"Wrote 42296 bytes to 'app'\" in the bkloader output"

pass "Erased 11 sector(s), Flash CRC32 OK, Wrote 42296 bytes to 'app'"
