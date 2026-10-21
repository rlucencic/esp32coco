# esp32coco: Tandy Color Computer 2 and 3 on the MaTouch ESP32-S3 7" and the M5Stack Tab5

A CoCo 2 (64 KB, MC6847 VDG, MC6883 SAM) or CoCo 3 (128/512 KB, GIME) with
a Motorola 6809 or Hitachi 6309, on two boards: the Makerfabs MaTouch
ESP32-S3 7" (1024x600, target esp32s3, the default) and the M5Stack Tab5
(ESP32-P4, 1280x720, target esp32p4). Cartridges, cassette (.CAS), two
floppies (.DSK, WD1793 + Disk BASIC), an OS-9 hard disk (VHD interface),
Bluetooth keyboard and mouse, USB keyboard and mouse on the Tab5, an LVGL
setup screen, a web page. ESP-IDF v5.5.4, C.

The board side (tasks, LCD, touch, SD, WiFi, BLE, USB HID, setup screen,
web page, console) and the 6809/6309 core (`components/m6809`) are the
shared library in `../esp32emu`, also used by the other projects (esp32ste,
esp32mac, esp32mac2, esp32mac3, esp32cpc, esp32zx, esp32cbm, esp32xl,
esp32msx, esp32gs). See its CLAUDE.md; changes there need all of them built,
for both boards.

## Build, flash, debug

Always with the IDF's Python environment:

```sh
bash -c '. $IDF_PATH/export.sh >/dev/null 2>&1; \
  export IDF_PYTHON_ENV_PATH=/Users/robertlucencic/.espressif/tools/python/v5.5.4/venv; \
  export PATH=$IDF_PYTHON_ENV_PATH/bin:$PATH; \
  idf.py build && idf.py -p /dev/cu.usbmodem1101 app-flash'
# Tab5:
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig build
  idf.py -B build-tab5 -DIDF_TARGET=esp32p4 -DSDKCONFIG=$PWD/build-tab5/sdkconfig -p /dev/cu.usbmodem3101 flash
```

- MaTouch `/dev/cu.usbmodem1101`, Tab5 `/dev/cu.usbmodem3101`;
  `http://coco.local` (one board at a time under that name).
- `flash` also writes `roms/*.rom` (not in git; `roms/README.md`) into the
  64 KB `coco_roms` partition at 0x810000 (8 KB slots: Color BASIC,
  Extended BASIC, Disk BASIC, the CoCo 3's 32 KB at slot 4); `app-flash`
  only the app. The boards are shared with the other projects (their ROM
  partitions are at the same place): after one of them, a full `flash`. NVS
  (WiFi, BLE pairings) is shared; this project's settings are namespace
  `coco` (`model`, `cpu`, `ram`, `monitor`, `artifacts`, `joystick`,
  `tape_speed`), its drives `cocodisks`.
- Tab5 settings: this project's `sdkconfig.defaults`, then
  `../esp32emu/boards/m5stack_tab5/sdkconfig.defaults` (`CMakeLists.txt`),
  into `build-tab5/sdkconfig`.
- `../esp32emu/tools/emu_console.py PORT log N | lcd F.png | send "cmd" | info | profile ELF`
  with the IDF venv's Python (`-I`).
- Flashing restarts the board and ends the user's session: say so first.

Console commands: the shared ones (`emu/console.h`: `t x y [ms]` touches,
`r` reset, `p` profile, ...) and the CoCo's (`main/main.c`): `k <n>` tap a
key (row * 8 + column: 1 A, 48 ENTER, 35 SPACE, 55 SHIFT), `K <n> 0|1`,
`T <text>` type (`\n` ENTER), `j <x> <y> <b>` joystick, `W` reset button,
`w` rewind the tape, `S <1-8>` fast tape loading, `R <2|3> [6809|6309]
[128|512]` restart as that machine. Touches: Start `t 850 532` (MaTouch),
`t 1065 637` (Tab5); the corner long-press `t 1010 12 2600` / `t 1270 10
2500`; Drives `t 594 532` / `t 742 637`; the drives page's New buttons
(Tab5) at x 943, rows y 181 268 354 440 526.

## Layout

| Path | What |
|---|---|
| `main/main.c` | The CoCo as an `emu_machine_t`: memory, ROMs, setup rows, key map, joystick (mouse, touch, keypad), the five drives, status, console |
| `main/settings.c`, `rom_store.c` | NVS settings; ROMs from the partition or `coco/roms` on the SD card |
| `components/coco/` | The CoCo, platform independent (`include/coco.h`), a folder per part |
| `components/coco/host/` | Desktop build for testing |
| `../esp32emu/components/m6809/` | The 6809/6309 core, disassembler, debugger adapter, its tests |
| `roms/` | The ROM images (not distributed), `README.md` with names and checksums |

## The core (`components/coco`)

- `coco.c`: the frame (262 lines) and its lines: syncs and interrupts at a
  line's start (HS each line; the VDG's FS falls at line 216 and rises at
  248, where the SAM's counter reloads; the GIME's FS is low 243-248, VBORD
  at the bottom border, HBORD each line), the CPU to the line's end, then
  the VDG or GIME draws it and the sound takes two samples. The PIAs'
  wiring (`coco_pia_input`, `coco_pia_changed`), the colours (VDG as MAME
  measures them, CoCo 3 RGB from the 6 bits, composite computed in YUV),
  fast tape loading (extra silent frames while a tape plays).
- **Time**: CPU cycles and ticks of the 3.58 MHz colour carrier (228 a
  line; 4 a cycle at 0.89 MHz, 2 at 1.79). `coco_now()`,
  `coco_cycle_at()` (shifts, no 64-bit division). Events (the disk
  controller's bytes, the GIME timer) stop the CPU's run early
  (`coco_event_changed()` ends a run when one moves earlier).
- `cpu/cpu.c`: the bus: page $FF is I/O for both machines; the floating
  bus (next instruction byte for `,R`, else $FFFF's) for anything unmapped.
- `memory/memory.c`: CoCo 2 from the SAM (TY, P1, M), CoCo 3 from the GIME's
  MMU (task, MMUEN, ROM mode MC1-0, MC3 constant $FE page; the ROM page
  goes with the slot, as MAME has it) into the CPU's 256-byte pages.
- `sam/sam.c`: the SAM's bits and the video counter (DA0 toggles, xdiv/ydiv
  by V, the HS clearing with carries) as MAME's 6883sam.
- `vdg/vdg.c`: the 6847's modes (`vdg_emit`, also the GIME's CoCo 2 modes
  with `t1`: lower case with GM0, inverse with GM1, no SG6), artifact
  colours for PMODE 4 (pairs of pixels), `vdg_commit()` (a hash per line in
  internal RAM; only changed lines are copied to the PSRAM frame).
  `vdg/font6847.c`: our own 5x7 font in the 6847's style, lower case too.
- `gime/gime.c`: registers, MMU, palette, timer (value + 2, 63.5 us or 279
  ns units; `$FF94` restarts it), interrupts (rising edges, enables in
  `$FF92/93`, reading them acknowledges). `gime/gime_video.c`: the frame's
  latches (address, scroll, geometry 192/200/225), text (font8x8, public
  domain, `gime/font8x8.c`) and graphics, wide (640) or 512 with borders;
  each line expanded to a pixel a byte and widened with word stores.
- `pia/pia.c`, `keyboard/keyboard.c` (matrix, typing queue), `joystick/`,
  `sound/sound.c` (DAC + single bit through the mux, averaged per half line,
  DC blocker, ring in PSRAM), `cartridge/`, `cassette/`, `fdc/` (WD1793 +
  DSKREG, JVC images), `harddisk/vhd.c` (VHD at $FF80).
- The frame is 640x240 colour numbers (0-63 CoCo 3, 64-79 VDG, 80-83
  artifact) in PSRAM; `coco_render_line_rgb565()` for the presenter.

## Lessons (each one cost a debugging session)

- **HALT takes hold one instruction late.** Disk BASIC's read loop (`LDB
  $FF4B; STB ,X+; STA DSKREG; BRA`) stores the byte it read before the CPU
  halts; at a sector's end the NMI comes after that store. Halting right
  after the read lost every sector's last byte (OS-9's REL had a 0 for an
  `LDA #$7E`: it ran into zeros). The halt now starts 6 cycles after the
  start of the instruction that took the byte (`cpu_took_byte()`): the
  cycle counter counts at an instruction's end, so "now" during an access
  is its start.
- **The WD1793 doesn't wait for the CPU.** DSKINI issues READ ADDRESS only
  to see the drive answer and never reads the 6 bytes: bytes come at their
  time, unread ones set lost data.
- **A .CAS has no silence**, but the ROM waits half a second with the motor
  on before it listens (after CLOAD's name block too): the tape stands still
  while the input isn't read for 3 ms. And every block ends with a $55
  trailer whose last cycle has no rising edge after it: the recorder
  completes it when the motor stops, playback adds two $55 after the end.
- **Typing waits for BASIC**: no keyboard buffer; a key is held until the
  ROM has scanned its column twice, and after ENTER until it scans ten
  frames in a row (SAVE's BREAK checks would take a key and lose it).
- **Internal RAM on the MaTouch**: the sound ring (16 KB) and the disk
  controller's track buffer were in internal RAM; with 36 KB left the
  supervisor task (16 KB stack: the corner long-press, touches) couldn't
  start. Big buffers go to PSRAM; ~68 KB free at the setup screen.
- **memcmp/memset on the PSRAM frame cost half the core** on the MaTouch
  (40 fps in HSCREEN 2): lines are hashed, fills are word stores, and
  `address % ram_size` (a division per byte) became a mask.

## Testing

- **Host build**: `make -C components/coco/host`, then `build/coco_host
  <2|3> [frames] [out.ppm]`. Environment: `COCO_ROMS`, `COCO_RAM`,
  `COCO_CPU=6309`, `COCO_NODISK`, `COCO_CART`, `COCO_FLOPPY0/1`, `COCO_HD`,
  `COCO_CAS`, `COCO_MONITOR=composite`, `COCO_ARTIFACTS`, `COCO_SCRIPT`
  (`<frame> type|key|joy|shot|text|insert|eject|rewind|reset|warm|peek|dump|dis|status`;
  a line waits for the text typed before it), `COCO_TEXT`, `COCO_WAV`,
  `COCO_TRACE=<pc>:<n>`, `COCO_PCLOG=<pc>:<n>`, `COCO_STATUS=<n>`,
  `COCO_FDCLOG`, `COCO_DEBUG=<port>`. Runs ~5000 frames a second.
- The 6809/6309 core: `make -C ../esp32emu/components/m6809/test test`
  (151 cases, 1302 opcode checks).
- Checked on the host (2026-10-19): both models boot BASIC (Color BASIC
  1.3 + Extended 1.1 + Disk 1.1; the CoCo 3's with Disk BASIC 2.1); text,
  SG4, PMODE 1/3/4 (artifacts), WIDTH 40/80 with attributes, HSCREEN 2;
  SAVE/LOAD/DIR/DSKINI; CSAVE/CLOAD; JOYSTK; SOUND (257 Hz for SOUND 89, as
  the ROM's loop gives); a test cartridge autostarts on both; NitrOS-9 EOU
  1.0.1 6809 and 6309 (native mode) boot from 68EMU/63EMU.DSK and run from
  the VHD (dir, free, makdir writes); the 6309 in native mode runs BASIC
  ~11% faster.
- Checked on the boards (2026-10-19): CoCo 3 BASIC at 60 fps on both
  (MaTouch core 1 48% text, 65% HSCREEN 2; Tab5 35%), CoCo 2 with a 6309 and
  PMODE 4 on the MaTouch, a new floppy made from the drives page and
  SAVE/LOAD on it (written back to the SD card), a new tape and
  CSAVE/CLOAD on the Tab5, NitrOS-9 6309 from the SD card's VHD on the Tab5
  (mdir), the setup screen and drives page on both, USB host started on the
  Tab5. Not checked by eye/ear/hand: sound, Bluetooth and USB keyboards,
  mouse/touch as the joystick, the Tab5 Keyboard.
- Test media live in the scratchpad (NitrOS-9 EOU from lcurtisboyle.com);
  never in the repository.
- **Free disk space** (web page, `space/space.c`): floppies OS-9, then
  RS-DOS; the VHD OS-9; counted at insert, eject and after a flush. Host:
  FREE(0)/FREE(1) and NitrOS-9's `free`; Tab5 (2026-10-21): an OS-9
  floppy 132096 of 184320, a 128 MB NitrOS-9 hard disk 38687744 of
  134212608.

## Open items

- Games, commercial cartridges (bank-switched ones over 32 KB), Multi-Pak
  (cartridge and disk controller together), DriveWire, RS-232.
- Floppy formats other than JVC (DMK, VDK); mid-line video changes.
- A web page screenshot for the README (served from a local mock, as the
  other projects did: never the live board's page).

## Working with the user

- Expects changes built, flashed and checked on the board, then a short
  report of what was verified and what wasn't.
- Keep their private data (WiFi name, IP, SD card contents) out of anything
  meant for publishing.
