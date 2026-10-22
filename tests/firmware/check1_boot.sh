#!/usr/bin/env bash
# Check 1: the bootloader starts, counts down autoboot and boots the app
# (ram_loader), which reports its shell.
set -eu
. "$(dirname "$0")/lib.sh"

start_emulator
capture_start

wait_for '^Bootloader v[^ ]+ by Apache02' 10
wait_for 'Press \[enter\] to stop autoboot:' 10
wait_for 'go os_addr\(0x10000\)' 15
wait_for 'RAM loader shell is ready\.' 10

# The four lines must have come in this order.
line_of() { since_mark | tr '\r' '\n' | grep -nE -m1 -- "$1" | cut -d: -f1; }
l1=$(line_of '^Bootloader v[^ ]+ by Apache02')
l2=$(line_of 'Press \[enter\] to stop autoboot:')
l3=$(line_of 'go os_addr\(0x10000\)')
l4=$(line_of 'RAM loader shell is ready\.')
[ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ] && [ "$l3" -lt "$l4" ] \
  || fail "boot lines out of order (banner $l1, autoboot $l2, go $l3, ready $l4)"

pass "bootloader banner, autoboot prompt, go os_addr(0x10000), RAM loader shell is ready."
