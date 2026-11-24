# Tiler Tex and B.E. Break-In replays, 2026-11-24

These are Mike Snyder's original CoCo releases from his disk collection.
Use disk 3 for Tiler Tex and disk 5 for B.E. Break-In. [Author's downloads](https://cocoquest.com/index.shtml#download)

| Image | SHA-256 |
| --- | --- |
| `msnyder3.dsk` | `0275aa0e1bd1f44946718df06ca8f8fbf8b1513c67a32603aafd4222b1d74acf` |
| `msnyder5.dsk` | `c71d41cd9a8681cad64c1a61ea5f3e4f10dc095d7339c415fb78d685f929773e` |

## Host

Build `coco_host` in the project, then run from a scratch directory with
both downloads. The scripts write PPM checkpoints and drive the guest
through its original keyboard and joystick inputs.

```sh
repo=$PWD
make -C components/coco/host
cd /path/to/scratch
export COCO_ROMS="$repo/roms"
replays="$repo/components/coco/host/replays"
host="$repo/components/coco/host/build/coco_host"
COCO_FLOPPY0=msnyder3.dsk COCO_MONITOR=composite \
  COCO_SCRIPT="$replays/tiler-tex.txt" COCO_WAV=tiler-tex.wav \
  "$host" 2 4500 tiler-end.ppm
COCO_FLOPPY0=msnyder5.dsk COCO_SCRIPT="$replays/be-break-in.txt" \
  COCO_WAV=be-break-in.wav "$host" 3 2500 break-in-end.ppm
```

- **Tiler Tex on CoCo 2.** `RUN "TILERTEX"` loads the BASIC game.
  Select **N** for the normal speed and press Enter. The composite VDG
  displays the 128×192 four-colour PMODE 3 splash and the level-one maze.
  The replay has captures at frames 600, 3950 and 4280. The stereo PCM
  includes 377,450 nonzero samples out of 4,716,000 at 31,399 Hz.
- **B.E. Break-In on CoCo 3.** `LOADM "BREAKIN1":EXEC` draws the
  title in GIME graphics. The replay presses fire and moves the paddle.
  Frames 1640 and 1770 show the first brick layout and different paddle
  positions. The 2500-frame capture shows a score of 35. The stereo PCM
  includes 398,824 nonzero samples out of 2,620,000 at 31,399 Hz.

## MaTouch

The ESP-IDF 5.5.6 test build used the UART console and the speaker path.
Both games reached their first levels on the board. Tiler Tex reported
PMODE 3 at 60 fps in the maze. A console-injected touch drag changed
the emulated right joystick from `32, 32` to `63, 32` and the captured
maze changed. B.E. Break-In showed 320×192 with 16 colours (HSCREEN 2).
After fire and a touch drag, the first-level capture showed the paddle
on the right. Board rates near these B.E. Break-In captures were
59–61 fps. Both disk images matched their uploads after play.

The speaker driver started at 31,399 Hz. For the CoCo 2's BASIC
`SOUND 89,60`, the board's audio telemetry reported a 257 Hz DAC signal.
A listening check and hands-on touch and mouse checks remain open.
