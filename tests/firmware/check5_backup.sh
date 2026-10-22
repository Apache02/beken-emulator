#!/usr/bin/env bash
# Check 5: bkloader reads the bootloader segment back; it is 64 KB and starts
# with the loaded bootloader image.
set -eu
. "$(dirname "$0")/lib.sh"
need_bkloader

start_emulator
capture_start
wait_for 'RAM loader shell is ready\.' 30
capture_stop

out=$WORK_DIR/bkloader.out
dump=$WORK_DIR/bootloader_backup.bin
run_bkloader 120 "$out" backup --port "$PORT" --speed 921600 --segment bootloader --output "$dump" \
  || fail "backup: $BKLOADER_ERROR"
[ -f "$dump" ] || fail "bkloader wrote no $dump"
size=$(stat -c %s "$dump")
[ "$size" -eq 65536 ] || fail "backup is $size bytes, expected 65536"
image_size=$(stat -c %s "$BOOTLOADER_BIN")
cmp -n "$image_size" "$dump" "$BOOTLOADER_BIN" \
  || fail "the backup does not start with the loaded bootloader image ($image_size bytes)"
grep -q 'CRC mismatch' "$out" && fail "bkloader reported a CRC mismatch"

pass "65536 bytes, first $image_size bytes equal the bootloader image"
