# bk7252-emulator

An emulator of the Beken BK7252 SoC (ARM968E-S) written in C++20 on top of
Unicorn. It runs unmodified firmware images: the bootloader and an
application from flash, plus images loaded to RAM, with the SPI flash, the
interrupt controller, the watchdog, two timers and two UARTs emulated. The
UARTs appear on the host as pseudo-terminals, so host tools such as the SDK's
`bkloader` talk to the emulator as they would to a board.

## Usage

### Build

Needs CMake 3.20 or later and a C++20 compiler; Unicorn, toml++ and doctest
are fetched at configure time.

```
cmake -S . -B build
cmake --build build -j
```

This produces `build/bkemu` and `build/unit_tests`.

### Run

```
build/bkemu BK7252 --load bootloader:firmware/<bootloader>_crc.bin --load app:firmware/app_crc.bin
```

`BK7252` names the profile `profiles/BK7252.toml`, found next to the
executable (`<exe>/../profiles`) or in the directory named by
`BK7252_PROFILES`. `--load TARGET:FILE` places an image: `bootloader` (flash
offset 0) and `app` (flash offset 0x11000) take CRC-protected `_crc.bin`
images; `RAM`, `IRAM` and plain addresses are also accepted. On start the
emulator prints where its UARTs are connected:

```
UART1: /dev/pts/N
UART2: /dev/pts/M
```

UART2 carries the bootloader's console; open it with any terminal program or
pass its path to `bkloader --port`. `--uart NAME=stdio` connects the UART
module `NAME` of the profile to the emulator's own standard input and output
instead (`--uart1 stdio` and `--uart2 stdio` are shorthands for `UART1` and
`UART2`; the terminal is put in raw mode, and Ctrl-C stops the emulator instead of
reaching the firmware); `--uart NAME=/dev/ttyUSB0` connects it to a real serial port at
115200 baud instead; `--i2c NAME=TYPE[@ADDRESS]` attaches a device (for example `hi704`)
to the I2C controller module `NAME` (`--i2c1 TYPE` is the shorthand for
`I2C1`); `-d LIST` enables debug logging. The emulator runs until it receives SIGINT or SIGTERM.

### Known limitation: start the host tool after the app banner

Start `bkloader` (or anything that sends input at once) only after the app's
banner, e.g. `RAM loader shell is ready.`, has appeared on UART2. Input sent
while the bootloader hands over to the app is kept and read later by the app,
which then prints stale `>` prompt lines; `bkloader` can take one of them for
the bootloader prompt and fail its baud-rate switch. Whether a chip behaves
the same is not established (the emulator buffers UART input without a depth
limit, a chip has a finite RX FIFO). See `tests/firmware/README.md`.

### Tests

```
ctest --test-dir build                 # unit tests and firmware checks
BKLOADER=/path/to/bkloader tests/firmware/run_all.sh
```

The firmware checks need the images in `firmware/` (not part of the
repository) and the path of the SDK's `bkloader` in `BKLOADER`; without them
the `firmware` test is reported as skipped. `tools/measure_speed.sh` reports
emulation speed. Details in `tests/firmware/README.md`.
