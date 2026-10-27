# bk7252-emulator

An emulator of the Beken BK7252 SoC (ARM968E-S) written in C++20 on top of
Unicorn. It runs unmodified firmware images: the bootloader and an
application from flash, plus images loaded to RAM, with the SPI flash, the
interrupt controller, the watchdog, two timers and two UARTs emulated. The
UARTs appear on the host as pseudo-terminals, so host tools such as the custom
SDK's `bkloader` talk to the emulator as they would to a board.

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
`I2C1`); `--log-level NAME=LEVEL` sets the level (`debug`, `info`, `warning`,
`error` or `none`, which silences the logger) of one logger, for example
`--log-level bus=debug`, or of all of them with `--log-level all=debug`; later
options win. The emulator runs until it receives SIGINT or SIGTERM.

### Flash file

Without options the flash lives in memory and is gone when the emulator stops.
`--flash-mount FILE` keeps it in `FILE`, like a virtual machine disk:

```
build/bkemu BK7252 --flash-mount flash.bin --load bootloader:... --load app:...
build/bkemu BK7252 --flash-mount flash.bin --load bootloader:...
```

A missing file is created with the size of the flash (erased, all `0xFF`); an
existing one must have that size and is what the flash holds at start. The
loads are applied after the file is mounted, and every load, page program and
erase is written to the file at once, so it survives even a killed emulator. A
load that finds its image in the file already says so in the log. The file is
the physical image with the CRC bytes, like the `_crc.bin` files, and is locked
while an emulator uses it. `--flash-mount NAME=FILE` picks the flash module
`NAME` when a profile has more than one.

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

For firmware that joins a network as a station, `--wifi-ap SSID` runs a
virtual access point instead (BSSID `02:00:00:00:00:02`) with a router behind
it on `10.0.2.0/24`: its DHCP server gives the firmware `10.0.2.15` (mask
`255.255.255.0`, lease one day), and the gateway `10.0.2.2` answers ARP and
ping and is the firmware's DNS server. `-p` works the same way in this role:
host connections reach the firmware from `10.0.2.2`. The firmware has no
internet access unless `--wifi-internet` is given (see below): without it
traffic beyond the LAN is dropped, and DNS queries get no answer.

```
build/bkemu BK7252 --load ... --wifi-ap TESTNET --wifi-password 12345678 --wifi-internet -p 18080:80
```

The network is open unless `--wifi-password PASS` (8 to 63 characters) is
given, which makes it WPA2-PSK (CCMP); a station with a wrong password fails
the 4-way handshake, and the log says so. `--wifi-channel N` (1 to 13, default
1) picks the channel; the access point is heard only while the firmware's radio
is tuned to it. `--wifi-ap` and `--ssid` exclude each other.

With `--wifi-internet` (it needs `--wifi-ap`) the router gives the firmware
internet access. Its TCP connections and UDP
datagrams to addresses outside the LAN are made again from ordinary sockets of
the emulator process (NAT), so the host and the network see plain outgoing
connections of the emulator; nothing needs root. DNS queries to `10.0.2.2` are
relayed to the first IPv4 `nameserver` of the host's `/etc/resolv.conf`. The
gateway is not a door to the host: apart from published ports, TCP to
`10.0.2.2` is reset and UDP to it other than DHCP and DNS is dropped, and the
host's loopback (`127.0.0.0/8`) cannot be reached. The host's other addresses,
such as its LAN address, are reached like any internet host, and so are
link-local addresses (`169.254.0.0/16`, e.g. a cloud metadata service at
`169.254.169.254`): only loopback and the gateway are closed. The log names the
first TCP or UDP connection to each remote address once (`firmware connects to
203.0.113.10:80 (tcp)`), and the DNS relay's upstream once (`DNS relayed to
127.0.0.53:53`). A DNS server that refuses the queries or never answers them
is reported with one warning.

Ping to an internet address works too: the echo request is sent on an
unprivileged ICMP socket of the emulator, which the kernel allows only for the
groups in `net.ipv4.ping_group_range`. Many systems default to `1 0` (nobody);
then the first ping logs a warning and gets no reply. To allow your group:
`sudo sysctl -w net.ipv4.ping_group_range="0 2147483647"`.

The emulator runs the firmware about ten times slower than real time, so a
join takes tens of seconds, and a remote server sees a slow client.

Firmware that runs its own access point next to the station on another channel
switches its radio between the two, and many frames can then be lost (25 to
100% ping loss per run was measured with such firmware); putting
`--wifi-channel` on the channel of the firmware's own access point avoids the
switching. `--log-level net.ap=debug`, `--log-level net.router=debug` and
`--log-level net.nat=debug` show the access point's, the router's and the NAT's
messages.

### Capturing the WiFi traffic

`--pcap FILE` (with `--wifi-ap`, `--ssid` or `-p`) records the 802.11 frames
the firmware sends and receives: link type 802.11 plus radiotap, which names
the channel the firmware's radio is on once it is known. Beacons, probes,
authentication, association, the WPA2 handshake and the data frames are all
there. The emulated MAC does not encrypt, so data frames are plain and
Wireshark shows the IP traffic without a key. Timestamps are device time,
counted from zero. Every frame is written at once, so a file can be followed
while the emulator runs. Wireshark reads a named pipe live:

```
mkfifo /tmp/bk.cap
wireshark -k -i /tmp/bk.cap &
build/bkemu BK7252 --load ... --wifi-ap TESTNET --pcap /tmp/bk.cap
```

Opening a pipe waits for its reader, and the emulator says so. If the reader
goes away, the capture stops with one warning and the emulator carries on.

### Deep sleep

When the firmware puts the chip into deep sleep, the chip powers down until a
wake-up, and the wake-up (GPIO, RTC) is not emulated: the emulator prints
`The firmware put the chip into deep sleep; ... stopping.` and exits with
status 0. Firmware often does this when a battery check reads low; the SAR ADC
has no analog input, so its profile parameter `sample` stands for the battery.
Its value, 3200, was found by trial: with it the vendor camera firmware (built
from the vendor SDK) does not go into deep sleep at start.

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

The firmware checks need the images in `firmware/` and the path of the custom
SDK's `bkloader` in `BKLOADER`; without the latter the `firmware` test is
reported as skipped. `tools/measure_speed.sh` reports
emulation speed. Details in `tests/firmware/README.md`.
