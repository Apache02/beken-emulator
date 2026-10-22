#!/usr/bin/env bash
# Check 2: stop autoboot with Enter and use the bootloader shell.
set -eu
. "$(dirname "$0")/lib.sh"

start_emulator
capture_start

wait_for 'Press \[enter\] to stop autoboot:' 10
uart_key '\r'
wait_for '^> ?$' 5

# Each command: send it, then wait until its output is followed by a prompt.
run_cmd() {
  mark
  uart_send "$1"
  wait_for '^> ?$' "${2:-5}"
}

run_cmd 'help'
since_mark | grep -q 'flash_dump' || fail "'help' does not list flash_dump"

run_cmd 'flash id'
# The id line: "flash id: 0x001c7015   2 MB"
id_line=$(since_mark | grep -E '^flash id: 0x[0-9a-f]{8}' | head -n 1)
[ -n "$id_line" ] || fail "'flash id' printed no 'flash id: 0x........' line"
echo "$id_line" | grep -Eq '^flash id: 0x001c7015([^0-9a-fx]|$)' \
  || fail "'flash id' shows '$id_line', expected 0x001c7015"
echo "$id_line" | grep -Eq '(^|[^0-9])2 MB' || fail "'flash id' shows '$id_line', expected 2 MB"

run_cmd 'flash unprotect'
since_mark | grep -qi 'error\|unknown\|fail' && fail "'flash unprotect' reported an error"
since_mark | grep -q '^done' || fail "'flash unprotect' did not report done"

run_cmd 'flash_dump 0x0 48'
since_mark | grep -A1 '^addr 0x00000000' | grep -q '^17 01 00 ea 14 f0 9f e5' \
  || fail "'flash_dump 0x0 48' does not start with 17 01 00 ea 14 f0 9f e5"

run_cmd 'partitions'
since_mark | grep -Eq '^\| app +\|' || fail "'partitions' does not list the app partition"

pass "help, flash id (0x001c7015, 2 MB), flash unprotect, flash_dump, partitions"
