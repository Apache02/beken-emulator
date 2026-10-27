#!/usr/bin/env bash
# Check 6: bkloader loads an IRAM image that tests the GDMA (memory to memory
# copies, edge cases, address loops, repeat mode, stop and restart, interrupts,
# arbitration and a benchmark that reads the MAC's microsecond counters), runs
# it and captures its report.
set -eu
. "$(dirname "$0")/lib.sh"
need_bkloader
need_file "$GDMA_BIN"

start_emulator
capture_start
wait_for 'RAM loader shell is ready\.' 30
capture_stop

out=$WORK_DIR/bkloader.out
run_bkloader 240 "$out" iram --speed 921600 --port "$PORT" --capture 120 --until "==END==" "$GDMA_BIN" \
  || fail "bkloader iram: $BKLOADER_ERROR"
grep -Eq '(^|[^0-9])37 / 37 passed' "$out" || fail "no '37 / 37 passed' in the bkloader output"
grep -q 'benchmark' "$out" || fail "the benchmark did not run"

pass "37 / 37 passed, benchmark ran"
