# Open questions for the real chip

Behaviour the emulator assumes but nobody has measured on the board yet. Each entry
names the assumption the emulator makes today, so a measurement either confirms it or
shows what to change. Move an entry to "Settled" with the measured result when it has
been checked.

## JPEG (`jpeg` module)

1. **Last word after the stream is exhausted.** Assumed: `rx_fifo_data` keeps returning
   the last word that was read. With `enc_size` set that word is the size trailer, so
   after DMA has drained a frame `rx_fifo_data` equals `byte_cnt_pfrm`. The word is not
   cleared by a new frame or by turning `enc_en` off. Source: recollection of how the
   firmware sees it, not a dedicated measurement.
   Test: for each combination of `enc_size` and `video_byte_rev`, drain one frame with
   the CPU, then read `rx_fifo_data` several more times and compare with
   `byte_cnt_pfrm`; repeat after turning `enc_en` off and on, and across two frames.

2. **Value before any frame was read.** Assumed: 0 (after reset and after power-up with
   the encoder never enabled). The firmware is expected to discard such words until it
   has read a picture.
   Test: after reset, read `rx_fifo_data` with `enc_en` clear, and with `enc_en` set
   before the first `start_frame`.

3. **Byte order seen by DMA.** Assumed: the same word as a CPU read, written to memory as
   four little-endian bytes. So with `video_byte_rev` clear every word in memory has its
   bytes reversed relative to the stream, and with it set the memory holds the stream in
   natural order.
   Test: drain a frame by DMA with each `video_byte_rev` value and compare the first
   word (a JPEG starts with `FF D8 FF E0`).

4. **Padding at the end of a frame.** Assumed: the picture is made to fit whole words
   together with the 5-byte trailer (a zero byte and the little-endian size), so the last
   word is exactly the little-endian size. If the real encoder instead pads the picture
   with zeros to a word boundary and then appends the trailer, the last word differs.
   Test: capture frames with `enc_size` set and look at the last 12 bytes of each frame
   and the value of `byte_cnt_pfrm`.

## UART

5. **Cold-start hand-over.** Input sent while the bootloader hands over to the app is
   kept and read later by the app, which prints stale `>` prompts (see the Known
   limitation in the README). The emulator buffers input without a depth limit; a chip
   has a finite RX FIFO. Does a chip behave the same?
   Test: with the board, send the same input burst during the hand-over and watch
   whether stale prompts appear.

## Access sizes, still open

- Repeat the narrow load and store test with the JPEG block enabled (needs its clock turned
  on). The project owner reports that the quantization table cannot be read then; the
  emulator does not model that.
- Not measured: MAC (needs the WiFi clock), the flash controller, registers with side
  effects, SPI, I2C, SAR ADC, SD, USB, security.
- Unaligned accesses with the alignment fault bit set, and unaligned accesses on timer
  bank 1 and UART2.

## Settled

Measured on the board on 2026-10-23, see `hardware-report-2026-10-23.md`.

- **GDMA unaligned 32-bit copies.** `test_gdma` passes 10 of 10 on the chip, like in the
  emulator: the emulator moves exactly the bytes asked for. Not run: source/destination
  offset pairs such as (3,0) and (1,2).
- **Byte and halfword stores to a peripheral register.** The value is repeated on every
  byte lane (`strb 0xAA` leaves `0xAAAAAAAA`, `strh 0xBBBB` leaves `0xBBBBBBBB`) and the
  register takes the whole word; there are no byte enables. A store at +1, +2 or +3
  reaches the same register as one at +0. The bus does the same.
- **Byte and halfword loads from a peripheral register.** The register answers with its
  whole word and the core picks the bytes (`ldrb` at +0..+3 gives `44 33 22 11` for
  `0x11223344`, `ldrh` at +0 and +2 give `3344 1122`). The bus does the same. The earlier
  "reads 0" result is not reproduced; it probably comes from the JPEG block being enabled
  (not measured, see below).
- **Unaligned `ldr`, `ldrh`, `str` and `strh` on a peripheral register.** The core behaves
  as on plain RAM: `ldr` at +1, +2, +3 returns the word rotated right by 8 times the
  offset; `ldrh` at +1 and +3 returns the halfword at the address with bit 0 cleared; `str`
  writes the whole word at the aligned address; no data abort. The one difference is the
  repeated value on halfword stores. **The emulator does not model this.** Unicorn splits an
  unaligned access into byte accesses before it reaches the bus, so the bus cannot tell an
  `ldr` at +1 from four byte loads: an unaligned `ldr` or `str` reads or writes the register
  word once per byte (and the last piece falls in the next word). Firmware does not do this
  on purpose, so it is left as it is.
- **The rules hold** on the JPEG quantization table (block not powered), GPIO, both timer
  banks, UART1 and UART2 status.
