# Firmware acceptance checks

Five checks that run real firmware in `build/bkemu` and drive it over UART2,
each on a freshly started emulator.

```
BKLOADER=/path/to/bkloader tests/firmware/run_all.sh
```

or through CTest (`ctest -L firmware` in the build directory, which also sets
`BKEMU_EXE` to the built executable). The scripts work from any directory.

Exit statuses: `run_all.sh` exits with 77 (CTest reports the test as skipped)
when `BKLOADER` is unset or not executable, or when one of the images is
missing; otherwise 0 when all six pass and 1 when any fails. The individual
scripts exit 0 on pass and 1 on failure, including a missing image. Checks 1
and 2 do not use `BKLOADER`; checks 3 to 6 exit 77 when it is unset or not
executable.

Images, in `firmware/` (not part of the repository):
`bootloader_v0.1.0-2-g5ae564b-dirty_crc.bin`, `app_crc.bin` (the `ram_loader`
application), `test_flash_sector_iram.bin` (an IRAM test that erases and
programs one flash sector), `test_gdma_iram.bin` (an IRAM test of the GDMA
driver: copies, edge cases and a benchmark that times itself with the MAC's
microsecond counters).

| Script | Does | Passes when |
| ------ | ---- | ----------- |
| `check1_boot.sh` | start, read UART2 | in this order: the banner `Bootloader v... by Apache02`, `Press [enter] to stop autoboot:`, `go os_addr(0x10000)`, `RAM loader shell is ready.` |
| `check2_shell.sh` | Enter during autoboot; `help`, `flash id`, `flash unprotect`, `flash_dump 0x0 48`, `partitions` | the line `flash id: 0x001c7015 ... 2 MB`; the dump starts `17 01 00 ea 14 f0 9f e5`; `help` lists `flash_dump`; `unprotect` reports `done`; `partitions` lists `app` |
| `check3_iram.sh` | `bkloader iram --speed 921600 --port $PORT --capture 6 --until "==END==" test_flash_sector_iram.bin` | `6 / 6 passed` and `==END==` |
| `check4_flash.sh` | `bkloader flash --port $PORT --speed 921600 app_crc.bin` | `Erased 11 sector(s)`, `Flash CRC32 OK`, `Wrote 42296 bytes to 'app'` |
| `check5_backup.sh` | `bkloader backup --port $PORT --speed 921600 --segment bootloader --output $OUT` | `$OUT` is 65536 bytes and starts with the loaded bootloader image |
| `check6_gdma.sh` | `bkloader iram --speed 921600 --port $PORT --capture 120 --until "passed" test_gdma_iram.bin` | `10 / 10 passed` and the benchmark ran |

Checks 3 to 6 search bkloader's output with colour codes removed and carriage
returns turned into line breaks; a run that hits the time limit is reported as
`bkloader timed out after N s`. When the first attempt of check 3 fails, it is
retried once on the same emulator and a `NOTE` line says so.

`lib.sh` holds the helpers: `start_emulator` (sets `PORT` to the host end of
UART2 and registers a trap that stops the emulator, the reader and the host
tool on every exit path), `wait_for <regex> <seconds>`, `uart_send <text>`,
`run_bkloader`. On failure a script prints `FAIL <check>: <reason>` and the
tail of the UART2 and emulator output. Set `VERBOSE=1` to see that output on
success too.

## Starting bkloader against a booted device

Checks 3 to 6 wait for `RAM loader shell is ready.` before they start
`bkloader`; this is a workaround. In experiments that started the tool during
the hand-over from the bootloader to the app (about 0.3 to 0.5 s after the
emulator starts, when the machine is loaded), the tool's Ctrl+C and `reboot`
arrived before the app read its input. When the app started reading that
input it printed stale `>` prompt lines (line redraws and empty-line
prompts), the tool took one of them for the bootloader prompt and failed with
`Echo mismatch`, `Unexpected speed response: '> reboot'` and
`Failed to switch baud rate`. In a byte trace of both directions no byte was
lost or reordered.

It is not established whether a chip behaves the same: the emulator buffers
UART input without a depth limit while the chip has a finite RX FIFO, so a
chip may drop part of such early input instead.

## Speed

`tools/measure_speed.sh` (report only, no threshold) times the bootloader's
autoboot countdown, an idle-free busy loop. The countdown length follows the
CPU clock the bootloader programs: 13,000,000 instructions per tick at 26 MHz
while the emulator had no system controller, 48,000,000 now (the bootloader
selects 120 MHz and the profile's `read_adjust` makes the loops see 96 MHz).
The script names the SDK sources this count comes from and notes the premises
it cannot check; update `INSNS_PER_TICK` when the profile's `read_adjust` or the
bootloader changes. No separate busy-loop image is available.

Measure with a Release build. A Debug build (`-O0`, Unicorn included) runs
about three times slower.

Measured 2026-10-23, Release build, 5 runs:

```
bootloader countdown: 48000000 instructions per tick, insns_per_us = 100
note: not checked: the bootloader programs 120 MHz (read as 96 MHz through read_adjust);
note: not checked: 2 instructions per 4-cycle busy-wait iteration (from the SDK's busy_wait.c)
run 1: process start to second tick 714.3 ms, second tick 679.8 ms
run 2: process start to second tick 702.0 ms, second tick 675.9 ms
run 3: process start to second tick 702.5 ms, second tick 661.3 ms
run 4: process start to second tick 712.2 ms, second tick 674.6 ms
run 5: process start to second tick 714.2 ms, second tick 664.9 ms
bootloader busy loop: 671.3 ms wall per tick, 71.5 M instructions/s, 1.399 s wall per emulated second
```

Earlier series on the same machine, before the system controller existed, gave 66.7, 74.7 and 75.8 M instructions/s. With `insns_per_us = 100` a fully busy CPU needs
100 M instructions per emulated second, so busy code runs at about 0.75 of
real time; code that waits for interrupts skips ahead to the next device event.
