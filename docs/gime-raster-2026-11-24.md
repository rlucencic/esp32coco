# CoCo 3 palette writes inside a line, 2026-11-24

Until this change the GIME drew each line with its registers as they were
at the line's end, so a palette write in the middle of a line recoloured
the whole line. Palette writes now show from the pixel the beam is at.
This note records the reference timing, how it was measured against XRoar,
the controls and the board results.

## Reference timing

| Source | When HBORD fires | Basis |
| --- | --- | --- |
| [XRoar](https://www.6809.org.uk/xroar/) 1.13.1 `tcc1014.c` | Where the active area ends; a line is 912 pixels (14.318 MHz) | Oscilloscope measurements of '86 and '87 GIMEs (its commits 1d5f5de2 and b24f5281) |
| [MAME](https://github.com/mamedev/mame/blob/master/src/mame/trs/gime.cpp) `gime.cpp` | End of HSYNC, 64 pixels after it falls | Emulator code tested against software |
| [Sock's GIME reference](https://www.6809.org.uk/sock/gime.html) | Falling edge of HSYNC | Documentation |

XRoar's figures are the only measured ones, so the emulator follows them.
With HBORD where a picture ends, the next 640-pixel picture starts 272
pixels (68 ticks of 3.58 MHz) after HBORD, and a 512-pixel one 400 pixels
after it. A palette write takes effect at the start of the store's write
cycle, which is cycle 4 of `STA` or `STB` extended. Other store forms are
within a cycle or two of that.

XRoar latches the border colour once per line, 22 pixels before the
picture on an '86 GIME. Its author marks this as unverified, and MAME
applies border writes mid-line instead. The emulator follows XRoar: a
border write before that point counts for the line, and a later one counts
from the next line.

## Implementation

`gime_colour_written()` in `components/coco/gime/gime_video.c` turns a
write's time into a pixel within the line's picture.

- A write before the picture's first pixel counts for the whole line.
- A write after the picture's last pixel counts for the next line.
- A write inside the picture is recorded with its pixel, up to 64 per line.

A line with recorded writes draws its palette entries once and maps each
stretch through the colours after the write that starts it. The CoCo 2
modes, whose border is not a palette entry, redraw the line for each write.
Mode and other register changes still apply to the whole line.

## Measurement

`components/coco/host/rastercart.py` makes a cartridge, or with `--bin` a
Disk BASIC binary for XRoar. The program sets a 640 x 192 two-colour screen
(`--mode 512` for 512 x 192) at 1.79 MHz and turns off the PIA interrupts:
SYNC ends on any interrupt line, even a masked one, and BASIC leaves its
60 Hz interrupt on.

Each of 88 two-line blocks then does the following.

1. SYNCs to the HBORD FIRQ.
2. Writes white to palette 0 at cycle D + 11.
3. Writes black at cycle 178, which is 64 cycles into the second line.

D increases by one per block. With `--first 4` and `--first 44` the writes
cover a full line period at one-cycle steps. `--stripes` writes eight
colours seven cycles apart on each block's first line. `--target border`
writes `$FF9A` instead.

XRoar has no command-line screenshot. For this comparison only, a local
copy of its null UI was patched to render into memory and write a PNG at
exit. The patch is GPL code and is not part of this repository. XRoar ran
with `-ui null -m coco3 -vdg-type gime1986|gime1987 -run raster.bin
-timeout 8`.

### Results

| Run | First line's edge | Second line's edge |
| --- | --- | --- |
| Before (`9cd6ca6`) | The whole line is white whatever D is | All black |
| Now | x = 8 · (D + 11) − 272 (640 pixels); before the picture the whole line, after it the next line | 240 |
| XRoar '86 and '87 | 34 pixels right of ours, both identical | 270 |
| Ours with a 3-cycle SYNC exit | 6 to 10 pixels left of XRoar | 264 |

The constant offset comes from the CPU core, not the GIME. XRoar's 6809
latches an interrupt line for a cycle and then spends two more cycles
leaving SYNC. Our core resumes at once. In a scratch build of our 6809 that
leaves SYNC three cycles late, the following held.

- Palette edges in 640- and 512-pixel modes were within 6 to 10 pixels of
  XRoar (about one CPU cycle), on the 1986 and 1987 GIMEs alike.
- The border latch fell within one cycle of XRoar's: one block of 88
  changed line.
- Border edges were within 2 pixels; XRoar's 512-pixel area sits 2 pixels
  left of centre.

The shared core now has that timing: see
[the 6809's interrupt timing](cpu-interrupt-timing-2026-11-24.md). With it,
`check-raster`'s expected positions include SYNC's 3 cycles, and ours and
XRoar's edges are 6 to 10 pixels apart.

### Checks and controls

`make -C components/coco/host check-raster` needs the ROMs. It runs the
single-write cartridges for both widths and both D ranges, and the stripes
for both widths. Each picture is compared against the formula. The stripes
are compared pixel for pixel, including writes that spill into the second
line. On the committed renderer, all 88 blocks fail.

These outputs are byte-identical to the committed build:

- WIDTH 80 and WIDTH 40 text with attributes
- HSCREEN 1, 2 and 3 drawings
- all Tiler Tex and B.E. Break-In captures and PCM recordings

The raster checks and the controls are clean under AddressSanitizer and
UBSan.

## MaTouch

The first build drew a line twice when it had writes. The raster cartridge
ran at 51 fps with core 1 at 100%, and `draw` used 51% of its time. Two
changes brought it back to full speed:

- the palette entries are drawn once and mapped through each stretch's
  colours;
- two-colour bytes are expanded four pixels a word.

| Cartridge | fps | Core 1 |
| --- | ---: | ---: |
| `raster-640-4` | 60 | 81–83% |
| `raster-512-4` | 60 | 76–77% |
| `stripes-640` (eight writes a line) | 59–61 | 87% |

`draw` fell to 15%. The board's screenshots of all three cartridges
matched host runs with the composite monitor in every pixel, compared as
RGB565. The cartridges were uploaded, inserted and deleted again through
the web API. The Tab5 build compiles but was not flashed.

## Not done

- Mid-line changes to video mode, resolution and scroll registers.
- Measured hardware behaviour for the border latch and for Sock's palette
  access glitch.
- Palette writes at 0.89 MHz were not measured. The code uses the same
  formula with 16 pixels per cycle.
- The VDG (CoCo 2) still draws whole lines.
