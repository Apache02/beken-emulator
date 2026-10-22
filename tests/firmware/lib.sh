# Shared helpers for the firmware acceptance checks. Sourced by the check
# scripts; needs bash (set -eu is set by the caller).
#
#   start_emulator           start build/bkemu with the bootloader and the app,
#                            set PORT to the host end of UART2 and register a
#                            trap that stops the emulator on every exit path
#   capture_start            copy everything UART2 sends into $UART_LOG
#   capture_stop             stop that copy (the host tool needs the port alone)
#   wait_for <regex> <secs>  wait until $UART_LOG, from the mark on, has a line
#                            matching the extended regex; fail after <secs>
#   mark                     move the mark to the end of $UART_LOG
#   uart_send <text>         send <text> and a line feed to UART2
#   uart_key <printf-fmt>    send raw bytes (printf format, e.g. '\r')
#   need_bkloader            exit 77 (skipped) when $BKLOADER is unusable
#   fail <message>           print FAIL and exit 1

FW_DIR_LIB=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$FW_DIR_LIB/../.." && pwd)
EMULATOR=${BK7252_EXE:-$REPO_ROOT/build/bkemu}
FIRMWARE=$REPO_ROOT/firmware
BOOTLOADER_BIN=$FIRMWARE/bootloader_v0.1.0-2-g5ae564b-dirty_crc.bin
APP_BIN=$FIRMWARE/app_crc.bin
IRAM_BIN=$FIRMWARE/test_flash_sector_iram.bin
CHECK_NAME=${CHECK_NAME:-$(basename "$0" .sh)}

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/bk7252-check.XXXXXX")
UART_LOG=$WORK_DIR/uart2.log
EMU_OUT=$WORK_DIR/emulator.out
EMU_PID=
CAT_PID=
BK_PID=
MARK=0
PORT=

cleanup() {
  status=$?
  if [ -n "$BK_PID" ]; then kill "$BK_PID" 2>/dev/null || true; wait "$BK_PID" 2>/dev/null || true; fi
  if [ -n "$CAT_PID" ]; then kill "$CAT_PID" 2>/dev/null || true; wait "$CAT_PID" 2>/dev/null || true; fi
  if [ -n "$EMU_PID" ]; then
    kill "$EMU_PID" 2>/dev/null || true
    # bounded: SIGTERM, then SIGKILL after 2 s
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "$EMU_PID" 2>/dev/null || break
      sleep 0.2
    done
    kill -9 "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
  fi
  if [ -n "${VERBOSE:-}" ] || { [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; }; then
    echo "---- $CHECK_NAME: last UART2 output ----" >&2
    [ -f "$UART_LOG" ] && tail -c 2000 "$UART_LOG" | tr -d '\000' >&2 || true
    echo "---- $CHECK_NAME: emulator output ----" >&2
    [ -f "$EMU_OUT" ] && tail -n 20 "$EMU_OUT" >&2 || true
  fi
  rm -rf "$WORK_DIR"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail() {
  echo "FAIL $CHECK_NAME: $*" >&2
  exit 1
}

pass() {
  echo "PASS $CHECK_NAME${1:+: $1}"
}

need_bkloader() {
  if [ -z "${BKLOADER:-}" ] || [ ! -x "$BKLOADER" ]; then
    echo "SKIP $CHECK_NAME: BKLOADER is not set to an executable bkloader"
    exit 77
  fi
}

need_file() {
  [ -f "$1" ] || fail "missing $1"
}

start_emulator() {
  [ -x "$EMULATOR" ] || fail "emulator not built: $EMULATOR"
  need_file "$BOOTLOADER_BIN"
  need_file "$APP_BIN"
  : > "$UART_LOG"
  "$EMULATOR" BK7252 --load "bootloader:$BOOTLOADER_BIN" --load "app:$APP_BIN" \
    > "$EMU_OUT" 2>&1 &
  EMU_PID=$!
  local i
  for i in $(seq 1 50); do
    PORT=$(sed -n 's/^UART2: \(\/dev\/pts\/[0-9]*\)$/\1/p' "$EMU_OUT")
    [ -n "$PORT" ] && break
    kill -0 "$EMU_PID" 2>/dev/null || fail "emulator exited: $(cat "$EMU_OUT")"
    sleep 0.1
  done
  [ -n "$PORT" ] || fail "no 'UART2: /dev/pts/N' line from the emulator within 5 s"
}

capture_start() {
  cat "$PORT" >> "$UART_LOG" &
  CAT_PID=$!
}

capture_stop() {
  if [ -n "$CAT_PID" ]; then
    kill "$CAT_PID" 2>/dev/null || true
    wait "$CAT_PID" 2>/dev/null || true
    CAT_PID=
  fi
}

mark() {
  MARK=$(stat -c %s "$UART_LOG")
}

# Prints the log from the mark on, with carriage returns and NULs removed.
since_mark() {
  tail -c +"$((MARK + 1))" "$UART_LOG" | tr -d '\r\000'
}

wait_for() {
  local regex=$1 secs=$2 deadline
  deadline=$(( $(date +%s%N) / 1000000 + secs * 1000 ))
  while :; do
    if since_mark | grep -Eq -- "$regex"; then return 0; fi
    kill -0 "$EMU_PID" 2>/dev/null || fail "emulator exited while waiting for '$regex'"
    [ "$(( $(date +%s%N) / 1000000 ))" -lt "$deadline" ] || fail "no '$regex' on UART2 within ${secs} s"
    sleep 0.05
  done
}

uart_key() {
  # shellcheck disable=SC2059
  printf "$1" > "$PORT"
}

uart_send() {
  printf '%s\n' "$1" > "$PORT"
}

# run_bkloader <secs> <out> <args...>: runs the host tool with a time limit.
# <out> receives its output cleaned (ANSI colour codes removed, carriage
# returns turned into line breaks), which is also printed without the
# progress lines; the raw output is kept in <out>.raw. On failure sets
# BKLOADER_ERROR to a description ("bkloader timed out after N s" or
# "bkloader exited with status N") and returns non-zero.
BKLOADER_ERROR=
run_bkloader() {
  local secs=$1 out=$2
  shift 2
  # In the background so that a signal to this script runs the trap at once,
  # and the trap can stop the tool too.
  timeout -k 5 "$secs" "$BKLOADER" "$@" > "$out.raw" 2>&1 &
  BK_PID=$!
  local rc=0
  wait "$BK_PID" || rc=$?
  BK_PID=
  sed 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\r/\n/g' "$out.raw" > "$out"
  grep -v '^ *\(Uploading\|Receiving\|\[[0-9]*/[0-9]*\] Uploading\)' "$out" || true
  case $rc in
    0) BKLOADER_ERROR= ;;
    124|137) BKLOADER_ERROR="bkloader timed out after $secs s" ;;
    *) BKLOADER_ERROR="bkloader exited with status $rc" ;;
  esac
  return $rc
}
