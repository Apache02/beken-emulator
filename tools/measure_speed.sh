#!/usr/bin/env bash
# Reports emulation speed; no pass/fail threshold.
#
# The bootloader's autoboot countdown is an idle-free busy loop. Premises,
# from the SDK sources (paths relative to the SDK root):
#
# - src/applications/bootloader/main.cpp, enter_shell(): each of the two
#   ticks is ten busy_wait_ms(100) calls with a UART poll in between.
# - src/platform/misc/busy_wait/busy_wait.c, busy_wait_at_least_cycles():
#   a two-instruction loop ("subs %0, #4" / "bcs 1b") that counts 4 cycles
#   per iteration, run for ms * 1000 * 2 * (sctrl_get_cpu_freq_hz() / 2e6)
#   cycles.
# - src/platform/drivers/hardware_sctrl/cpu_freq.c, sctrl_get_cpu_freq_hz():
#   reads the system controller's control register (0x00800008, see
#   src/platform/soc/bk7221u/include/soc/sctrl.h); with mclk_source 0 and
#   divider 0 it returns XTAL_CLOCK_HZ = 26 MHz (soc/clock.h).
#
# The emulator has no system controller module, so that register reads 0 and
# one tick is 26e6 / 4 * 2 = 13,000,000 instructions (the UART polls are
# negligible). At insns_per_us = 100 that is 130 ms of device time. The
# script asserts the clock premise: each run must log the bus warning
# "unclaimed peripheral read at 0x00800008" (unclaimed reads return 0). The
# instruction count per loop iteration cannot be checked from outside; a note
# says so.
#
# The script starts the emulator RUNS times (default 5), times the second tick
# on UART2 (each tick starts with a '\r' that is printed the moment the
# previous one ends; the start of the first may already sit in the pty buffer
# when the port is opened, so it is not used) and prints wall time per tick,
# instructions per second and wall time per emulated (device) second.
#
# No separate busy-loop image exists in firmware/; the countdown is the
# idle-free busy loop measured here.
set -eu
CHECK_NAME=measure_speed
. "$(dirname "$0")/../tests/firmware/lib.sh"

RUNS=${RUNS:-5}
INSNS_PER_TICK=13000000
INSNS_PER_US=$(sed -n 's/^insns_per_us *= *\([0-9]*\).*/\1/p' "$REPO_ROOT/profiles/BK7252.toml")

now_us() { echo "${EPOCHREALTIME//[^0-9]/}"; }

# Reads UART2 in '\r'-terminated pieces and records when each countdown
# piece ends. Sets T_START (emulator started to second tick started) and
# T_TICK2 (second tick), in microseconds.
measure_once() {
  local t0 t piece t_press2= t_end= deadline
  t0=$(now_us)
  start_emulator
  deadline=$((t0 + 20000000))
  # Read through a pipe: bash's read changes terminal modes on a tty.
  exec 3< <(exec cat "$PORT")
  CAT_PID=$!
  while [ "$(now_us)" -lt "$deadline" ]; do
    IFS= read -r -d $'\r' -t 5 piece <&3 || fail "no countdown on UART2 (read status $?)"
    t=$(now_us)
    case "$piece" in
      "Press [enter] to stop autoboot:"*2) t_press2=$t ;;
      "Press [enter] to stop autoboot:"*1) t_end=$t; break ;;
    esac
  done
  exec 3<&-
  capture_stop
  [ -n "$t_press2" ] && [ -n "$t_end" ] || fail "countdown did not end within 20 s"
  T_START=$((t_press2 - t0))
  T_TICK2=$((t_end - t_press2))
  grep -q 'unclaimed peripheral read at 0x00800008' "$EMU_OUT" \
    || fail "premise broken: the system controller's control register (0x00800008) is no longer an unclaimed read returning 0, so the CPU clock may not be 26 MHz; update INSNS_PER_TICK"
  kill "$EMU_PID" 2>/dev/null || true
  wait "$EMU_PID" 2>/dev/null || true
  EMU_PID=
}

ms() { awk -v us="$1" 'BEGIN { printf "%.1f", us / 1000 }'; }

echo "emulator: $EMULATOR"
echo "bootloader countdown: $INSNS_PER_TICK instructions per tick, insns_per_us = $INSNS_PER_US"
echo "note: checked per run: control register 0x00800008 reads 0 (26 MHz clock);"
echo "note: not checked: 2 instructions per 4-cycle busy-wait iteration (from the SDK's busy_wait.c)"
total_us=0
ticks=0
for run in $(seq 1 "$RUNS"); do
  measure_once
  echo "run $run: process start to second tick $(ms "$T_START") ms, second tick $(ms "$T_TICK2") ms"
  total_us=$((total_us + T_TICK2))
  ticks=$((ticks + 1))
done

awk -v us="$total_us" -v n="$ticks" -v insns="$INSNS_PER_TICK" -v ipu="$INSNS_PER_US" 'BEGIN {
  tick = us / n / 1e6
  ips = insns / tick
  printf "bootloader busy loop: %.1f ms wall per tick, %.1f M instructions/s, %.3f s wall per emulated second\n",
         tick * 1000, ips / 1e6, ipu * 1e6 / ips
}'
