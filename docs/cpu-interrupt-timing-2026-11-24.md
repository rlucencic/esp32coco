# 6809 interrupt and SYNC timing, 2026-11-24

The [palette measurement](gime-raster-2026-11-24.md) put raster writes 30
to 34 pixels, about four CPU cycles, earlier than XRoar. A scratch copy of
the 6809 core that left SYNC three cycles later closed most of the gap. The
shared core in `../esp32emu/components/m6809` now follows the 6809 data
sheet's cycle-by-cycle sequences as XRoar implements them. XRoar's core was
restructured "to more accurately reflect [the] data sheet". Its code was
read for behaviour only.

## What the 6809 does

- **Latching.** The interrupt inputs are latched at the start of every bus
  cycle, and the latch is examined at the end of an instruction. A line
  that rises during an instruction's last cycle is seen only after the
  next instruction.
- **SYNC.** The CPU checks the latch once per cycle. When a latched line
  appears, it spends one more dead cycle, so the next instruction starts 3
  cycles after the latch. SYNC still takes at least its own 4 cycles.
- **CWAI.** Stacking takes 18 cycles. If a line was latched by then, the
  4-cycle vector fetch follows at once: 22 cycles from the opcode to the
  handler. Otherwise the handler starts 6 cycles after the latch.
- **Entry costs.** IRQ and NMI take 19 cycles and FIRQ takes 10. These
  already matched.

## The core before and after

| | Before | Now |
| --- | --- | --- |
| A line rising in an instruction's last cycle, or exactly where it ends | Taken at that end | Taken after the next instruction |
| SYNC waking on a line | The next instruction at once | 3 cycles after the latch, at least SYNC's 4 |
| CWAI with the interrupt pending | 20 + 7 = 27 cycles | 22 |
| CWAI waiting | The handler 7 cycles after the line | 6 after the latch |
| A line that falls | Immediate | Immediate (no phantom interrupt from a late acknowledge) |

The core only sees lines at instruction boundaries, so it needs to know
when a line rose. `m6809_set_interrupts()` and `m6809_nmi()` record a
rise.

- **Between runs.** A rise between `m6809_run()`s comes at the event that
  ended the last run, which is the `until` that run began with. The
  instruction that crossed that point sees the line if it went past it by
  a cycle or more.
- **From bus callbacks.** A rise during an instruction, from its own I/O,
  counts as the last cycle.
- **Written directly.** Inputs written straight into `irq` and `firq`
  count as latched long ago, which keeps the table-driven test cases
  unchanged.

The common path costs one flag test per instruction. The latch is checked
only where an interrupt would be taken. The CoCo sets its lines through
`m6809_set_interrupts()`.

## Checks

- **Core tests.** `make -C ../esp32emu/components/m6809/test test`: 151
  cases and 1302 opcode checks pass. "CWAI then IRQ" now expects 22 cycles.
  Seven new timing tests run through `m6809_run()`, and each fails on the
  old core:
  - SYNC waking 3 cycles after the line
  - SYNC with the line already up taking 4 cycles
  - a rise exactly at an instruction's end waiting one instruction
  - a rise an instruction went past being taken at once
  - a rise from the program's own `STA` being taken after the next
    instruction
  - CWAI's handler starting 6 cycles after the line
- **Raster check.** `make -C components/coco/host check-raster` passes once
  `rastercart.py` puts SYNC's 3 cycles into its expected positions; every
  edge moved 24 pixels. Against XRoar, palette edges in 640- and 512-pixel
  modes are now 6 to 10 pixels apart, about one cycle, where they were 30
  to 34. The border latch agrees within one cycle.
- **Same as before the change, byte for byte:**
  - the Disk BASIC format, save, reopen and load script (its HALT and NMI
    byte loop);
  - every Tiler Tex and B.E. Break-In picture, from DSK and from DMK;
  - WIDTH 80 text and the HSCREEN 1 and 3 drawings;
  - [NitrOS-9 EOU 1.0.1](https://www.lcurtisboyle.com/nitros9/nitros9.html),
    6809 and 6309 native mode, from `68EMU.dsk` or `63EMU.DSK` with the
    128 MB VHD. Its `mdir`, `dir /dd`, `free /dd`, `makdir /dd/IRQTEST`
    screens and the written VHD are identical.
- **Shifted slightly:**
  - Both games' PCM differs after 11.5 s (Tiler Tex) and 17.4 s (B.E.
    Break-In). Loudness per half second is within 2%, and of about 3,500
    sampled level changes, 95% fall on the same sample and none is more
    than 3 samples (0.1 ms) off.
  - The WIDTH 40 and HSCREEN 2 captures differ by 76 and 4 pixels. Typing
    and drawing progress move by a frame; the final pictures are identical.
- **Speed.** Host runs of B.E. Break-In and a NitrOS-9 boot took the same
  time before and after.

## MaTouch

ESP-IDF 5.5.6, both builds compile, MaTouch flashed.

- **Raster cartridges.** All three ran at 59 to 60 fps. Their pictures
  matched the host's pixel for pixel. Core 1 was 84%, 79% and 88% busy;
  before the change it was 81–83%, 76–77% and 87%. The profile shows
  `m6809_step` at 12.7% where it was 11.8%; most of the rest is drawing,
  since different lines now have their writes inside the picture.
- **Game replay.** This morning's replay reached the Tiler Tex maze and
  the B.E. Break-In level with touch steering at 59 to 61 fps.

The Tab5 build compiles but was not flashed.
