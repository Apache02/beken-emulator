# Hardware measurements, 2026-10-23

Answers to questions 6, 7, 8 and 9 of `hardware-questions.md`, measured on the board.
Questions 1-5 were not run.

Setup: BK7252 (flash id `0x001c7015`), code run from IRAM through the bootloader shell,
output captured on UART2. No clock or power setting was changed by the measurements: every
block was in the state the bootloader leaves it in. In particular the JPEG block was **not**
powered up.

## Summary

| # | Question | Emulator assumption | Measured | Verdict |
|---|----------|---------------------|----------|---------|
| 6 | GDMA, unaligned 32-bit copies | exact bytes moved | all cases pass | confirmed |
| 7 | Byte/halfword stores | repeated on every lane; stores at non-multiple-of-4 ignored | repeated on every lane; stores at +1, +2, +3 are **not** ignored | half wrong |
| 8 | Byte/halfword loads at a word-aligned address | read 0 | return the matching bytes of the word | wrong for this block state |
| 9 | Do the rules hold for all peripherals | unknown | same on GPIO, both timer banks, UART1, UART2 status | holds for the blocks tried |

## Question 7: narrow stores

Register under test, word at offset 0, preloaded with `0x11223344`, reloaded before each
step:

| Access | Result |
|--------|--------|
| `strb 0xAA` at +0 | `0xAAAAAAAA` |
| `strh 0xBBBB` at +0 | `0xBBBBBBBB` |
| `strb 0xCC` at +1 | `0xCCCCCCCC` |
| `strb 0xCC` at +2 | `0xCCCCCCCC` |
| `strb 0xCC` at +3 | `0xCCCCCCCC` |
| `strh 0xDDDD` at +2 | `0xDDDDDDDD` |

- The byte is repeated on every byte lane, as hypothesised.
- A store at an address that is not a multiple of 4 reaches the same register. The
  hypothesis that such stores are ignored is wrong. The word address is decoded from the
  upper bits and the low two bits only select the byte lane in the core.
- There are no byte enables: the other bytes of the old word are not kept.

The same pattern was seen on every register tried (see question 9). For registers with
fewer than 32 implemented bits (UART1 `flow_config`, mask `0x0007FFFF`) the repeated value
is masked: `strb 0xAA` gives `0x0002AAAA`, where `0x0002` are bits left from the preload.

## Question 8: narrow loads

With `0x11223344` in the register:

| Access | Result |
|--------|--------|
| `ldrb` at +0, +1, +2, +3 | `44 33 22 11` |
| `ldrh` at +0, +2 | `3344 1122` |
| `ldr` | `0x11223344` |

Narrow loads do not return 0. The register answers with the whole word and the core picks
the bytes. `ldrb` was checked at all four offsets, `ldrh` at +0 and +2; unaligned `ldrh`
and `ldr` are in the next section.

A byte dump (`dump`) and a word dump (`dump32`) from the bootloader shell agree on all stable
values, for example the quantization table word `27e53680` at `0x0080A080` reads as
`80 36 e5 27`. They differ only for registers that change between two reads (UART2 FIFO
status at `0x00802208`, TRNG data at `0x00802484`).

The earlier measurement "read any other way, it reads 0" is not reproduced in this state.
Reported by the project owner, not measured here: while the JPEG block is enabled the
quantization table can no longer be read. The earlier result probably comes from that state.
It has not been measured yet.

## Unaligned word and halfword accesses

Same registers, alignment faults disabled, `0x11223344` preloaded. A word in plain RAM was
run through the same accesses as a reference.

| Access | Plain RAM | Peripheral registers |
|--------|-----------|----------------------|
| `ldr` at +1 | `0x44112233` | same |
| `ldr` at +2 | `0x33441122` | same |
| `ldr` at +3 | `0x22334411` | same |
| `ldrh` at +1 | `0x00003344` | same |
| `ldrh` at +3 | `0x00001122` | same |
| `str 0xA1B2C3D4` at +1, +2, +3 | word stored, `0xA1B2C3D4` | same |
| `strh 0xBEEF` at +1 | low half only: `0x1122BEEF` | `0xBEEFBEEF` |
| `strh 0xBEEF` at +3 | high half only: `0xBEEF3344` | `0xBEEFBEEF` |

- An unaligned `ldr` returns the word rotated right by 8 times the offset, on peripherals
  exactly as on RAM. The rotation is done by the core, not by the bus.
- An unaligned `ldrh` returns the halfword at the address with bit 0 cleared, on both.
- An unaligned `str` writes the whole word at the aligned address, on both.
- An unaligned `strh` writes the halfword at the address with bit 0 cleared. On RAM only that
  half changes. On a peripheral both halves take the value, as for the aligned `strh` in
  question 7, so the test cannot show which half the core addressed.
- No data abort was raised with the alignment fault bit cleared.

The only difference between a peripheral and RAM is the repeated value on narrow stores.

## Question 9: other peripherals

Same sequence as question 7 and 8, with the same outcome:

| Block | Register | Address | Mask |
|-------|----------|---------|------|
| JPEG | `quantization_table[0]` | `0x0080A080` | `0xFFFFFFFF` |
| GPIO | `intlv1` | `0x0080288C` | `0xFFFFFFFF` |
| Timer bank 0 | `counter[2]` | `0x00802A08` | `0xFFFFFFFF` |
| Timer bank 1 | `counter[2]` | `0x00802A48` | `0xFFFFFFFF` |
| UART1 | `flow_config` | `0x00802118` | `0x0007FFFF` |
| UART2 | `fifo_status` (read only) | `0x00802208` | read only |

For UART2 `fifo_status` (`0x00090080`) `ldrb` and `ldrh` at +0 returned `0x80` and
`0x0080`, the low byte and halfword of the word. Each writable register was restored to its
saved value after the test.

Not measured:

- MAC (`0xC0000000`): needs the WiFi clock, which was left off.
- Flash controller: a stray write can damage the flash.
- Registers with side effects (data FIFOs, write-1-to-clear status): all tested registers
  behave like plain memory.
- Unaligned word and halfword accesses with the alignment fault bit **set**: not tried.
- Unaligned accesses on timer bank 1 and UART2 (the unaligned run covered the JPEG table,
  GPIO `intlv1`, timer bank 0 and UART1).
- Everything else in the peripheral window (SPI, I2C, SAR ADC, SD, USB, security).
- JPEG with the block enabled.

## Question 6: GDMA unaligned copies

`test_gdma` run on the board, 10 of 10 passed. Memory-to-memory copies with both widths 32
bit:

| Case | Bytes | Result |
|------|-------|--------|
| source and destination aligned to 4 | 1024 | ok |
| both at offset 2 | 510 | ok |
| both at offset 1 | 257 | ok |
| source at offset 0, destination at offset 3 | 200 | ok |
| patterns `0x00`, `0xFF`, `0xAA`, `0x55` | - | ok |
| large transfer | 8192 | ok |

Edge cases (zero length, oversize, invalid channel, NULL pointers) also pass.

Limits of the test: bytes just outside the destination are checked only in the aligned case.
The pairs (source offset, destination offset) tried are (0,0), (2,2), (1,1) and (0,3); for
example (3,0) and (1,2) were not run.

The emulator moving exactly the requested bytes agrees with the chip on these cases.

## Open follow-ups

- Repeat questions 7 and 8 with the JPEG block enabled (needs its clock turned on).
- Questions 1-5 are still open.
