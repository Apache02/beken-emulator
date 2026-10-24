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
build/bkemu BK7252 --load bootloader:firmware/bootloader_v0.1.0-2-g5ae564b-dirty_crc.bin --load app:firmware/app_crc.bin
```

`firmware/` holds ready-to-run images, so this works right after the build. The
camera application with its web page, a sensor on the I2C bus and a forwarded
port is started like this:

```
build/bkemu BK7252 \
  --load bootloader:firmware/bootloader_v0.1.0-2-g5ae564b-dirty_crc.bin \
  --load app:firmware/A9_B_V1_3-camera-v0.1.0-uart2_crc.bin \
  -p 8080:80 -p 2323:23 \
  --i2c1 hi704 \
  --uart2 stdio \
  --log-level unknown=warning
```

Once the line `associated with 'Beken-72838a'` has appeared, open
`http://127.0.0.1:8080/`; the application's shell is on UART2 (the terminal) and,
over the network, on port 2323. Ctrl-C stops the emulator.

`BK7252` names the profile `profiles/BK7252.toml`, found next to the
executable (`<exe>/../profiles`) or in the directory named by
`BKEMU_PROFILES`. `--load TARGET:FILE` places an image: `bootloader` (flash
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
`I2C1`); `--log-level NAME=LEVEL` sets the level (`debug`, `info`, `warning` or
`error`) of one logger, for example `--log-level bus=debug`, or of all of them with
`--log-level all=debug`; later options win. The emulator runs until it receives SIGINT or SIGTERM.

### WiFi

The WiFi MAC (module `MAC`) is an open access point to the firmware. With
`-p`/`--publish` or `--ssid` the emulator adds a virtual client of that access
point: it joins the firmware's network, takes an address from the firmware's
DHCP server and forwards host ports to the firmware.

```
build/bkemu BK7252 --load ... -p 8080:80 -p 127.0.0.1:5353:53/udp --ssid mycam
```

`-p [HOST_IP:]HOST_PORT:GUEST_PORT[/tcp|/udp]` (repeatable) listens on the host
address (default `127.0.0.1`, protocol `tcp`) and forwards every connection or
datagram to the firmware's GUEST_PORT. The emulator prints one line per publish
(`tcp 127.0.0.1:8080 -> guest port 80`) before the UART lines. A TCP client that
connects before the firmware has an address waits up to 30 s of emulated time,
then is dropped with a warning. `--ssid NAME` makes the client join only the
access point called NAME (default: the first open network it sees). Secured
networks are not joined. `--log-level wifi=debug` and `--log-level net=debug` show the
register accesses and the network's own debug messages.

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

The firmware checks need the images in `firmware/` and the path of the SDK's
`bkloader` in `BKLOADER`; without the latter the `firmware` test is reported as
skipped. `tools/measure_speed.sh` reports
emulation speed. Details in `tests/firmware/README.md`.
