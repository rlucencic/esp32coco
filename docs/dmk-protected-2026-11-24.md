# Protected DMK images, 2026-11-24

The [Color Computer Archive](https://colorcomputerarchive.com/repo/Disks/)
keeps copy-protected originals as DMK images inside the normal per-title
zips, in a `Copy protected DMK version/` folder. Most are named `.dsk`.
Twenty-three zips (22 games and Kidwriter) hold 29 DMKs, 21 of them boot
disks.

Most kits were made with Carl England's Defeater. It reads each protected
track with Read Track and writes it back with Write Track, so the
protection is what a WD1793 can write. None of these disks needs weak bits
or rotation timing.

## What the reader needed

1. **Recognise a DMK by its contents.** A DMK is now recognised by its
   header and size, whatever its extension. A `.dsk` that isn't one is read
   as JVC. The DMK still has to agree with its size, and at least one
   valid ID must exist; a `.dmk` that fails is refused.
2. **Tolerate the ID tables.** Tools pad the table instead of ending it
   with zero; Diecom's images use `$0080`. In a full table the last pointer
   can miss its `FE` by a few bytes (Those Darn Marbles 2, Marty's
   Nightmare, White Fire of Eternity). When an image is inserted, its table
   is cut down in memory to the double-density pointers that point at an
   `FE` with the ID inside the track. A controller in double density
   wouldn't see single-density IDs either. DMKs are read-only, so the file
   isn't changed.
3. **Accept longer tracks.** Wild West has 0x1980-byte tracks. The limit is
   now 0x2940, the format's largest.
4. **Accept two sync bytes before a data mark.** In 58 of the 14,779 data
   marks across all 29 images (in Kidwriter, Shamus and Zanth), the first
   of the three `A1` sync bytes reads as `$14`, `$C2` or `$02`. That is the
   clock-bit quirk of Read Track, kept by the kits. 40 of those 58 have a
   correct data CRC when three `A1`s are assumed. A data mark now needs two
   `A1`s in front of it. Every mark in all 29 images has at least that.
   XRoar accepts any `FB` or `F8` within 43 bytes. Before this change,
   Kidwriter got record-not-found on its check sector `$13`.

`components/coco/host/dmkinfo.py` lists an image's tracks as the reader
sees them. It shows IDs out of order, IDs on another track, size codes,
CRC errors, missing or deleted data marks, and pointers it ignores.

## Results

On the host, each boot disk was started with the command below. The
pictures were taken at 20, 40 and 70 seconds and compared with XRoar
1.13.1 running the same image. XRoar chooses the format by extension, so it
was given a `.dmk` copy.

| Title | Machine | Command | Ours |
| --- | --- | --- | --- |
| Barbarian Quest | CoCo 3 | `LOADM"BARB":EXEC` | title, monitor question |
| Bash 3.00.00 | CoCo 2 | `RUN"*"` | playing (the same in XRoar) |
| Caladuril 2 | CoCo 3 | `LOADM"CAL2":EXEC` | title |
| Gates of Delirium | CoCo 2 | `LOADM"GATES"` | title |
| Iron Forest | CoCo 3 | `LOADM"IRON"` | title, monitor question |
| Kidwriter | CoCo 2 | `RUN"SPIN"` | title, demo page |
| Kyum-Gai: To Be Ninja | CoCo 3 | `LOADM"NINJA":EXEC` | after `Y` and disk 2, playing |
| Major Istar | CoCo 2 | `LOADM"MISTAR":EXEC` | artifact-colour screen |
| Those Darn Marbles | CoCo 3 | `LOADM"TDM":EXEC` | title |
| Marty's Nightmare | CoCo 3 | `RUN"*"` | title, monitor question |
| Medieval Madness | CoCo 3 | `LOADM"MEDIEVAL"` | monitor menu |
| Mine Rescue | CoCo 3 | `RUN"*"` | title |
| Nuke the Love Boat | CoCo 3 | `LOADM"NUKE":EXEC` | monitor question |
| Photon | CoCo 3 | `LOADM"PHOTON":EXEC` | title, monitor question |
| The Seventh Link | CoCo 3 | `LOADM"LINK":EXEC` | title |
| Shamus | CoCo 2 | `RUN"SHAMUS"` | title, then its menu |
| Snake Pit | CoCo 2 | `RUN"*"` | title, then play |
| White Fire of Eternity | CoCo 2 | `LOADM"BOOT":EXEC` | title, artifact-colour screen |
| Wild West | CoCo 3 | `LOADM"WILDWEST":EXEC` | monitor question |
| Z-89 | CoCo 3 | `RUN"*"` | title, monitor question |
| The Magic of Zanth | CoCo 3 | `LOADM"ZANTH":EXEC` | monitor question |

All 21 reach their title, first prompt or play. The other eight DMKs are
data disks or second disks. Every image read back unchanged.

**Against XRoar:**
- **The same:** 13 titles reach the same point in XRoar. XRoar's disk
  turns at real speed, so it reaches Nuke the Love Boat, Zanth and Major
  Istar's prompts later.
- **Wild West:** XRoar answers `?IO ERROR`; its 0x1980-byte tracks are the
  likely cause.
- **The Diecom titles:** Gates of Delirium, Iron Forest and Medieval
  Madness start themselves through BASIC's output hook at `$0167`. XRoar's
  `-type` feeds BASIC's input without passing through that hook, so in
  XRoar they stay at `OK`.
- **White Fire of Eternity:** ours shows its title at 8–12 seconds and
  XRoar at 20. The following screen is the artifact-colour test: blue in
  ours, orange in XRoar, which uses the other colour phase.

**Commands that matter:**
- **Shamus.** The directory has a sector with a bad ID CRC, so `DIR`
  answers `?IO ERROR`. That is the protection. The loader is `SHAMUS.BAS`,
  so the command is `RUN"SHAMUS"`.
- **The Diecom loaders.** Their entry address is `$0000`, and the game
  starts through the hook. `:EXEC` jumps to `$0000` and runs BASIC's
  variables as code. In ours that reached a `STU $1010`, which cleared two
  bytes of the loader; the protected sector still read without error, and
  the game then reset to BASIC. The new `COCO_WATCH=<addr>` host option
  showed which instruction changed the byte and the 24 steps before it.

## MaTouch

ESP-IDF 5.5.6. The four images were uploaded under their `.dsk` names and
inserted through the web API.

| Title | Reached | fps | Core 1 |
| --- | --- | ---: | ---: |
| Iron Forest | title, monitor question | 60 | 100% |
| Wild West | monitor question | 60 | 68% |
| Kidwriter | demo page | 60 | 50% |
| Snake Pit | title | 60 | 53% |

The images read back unchanged and were deleted again. The Tab5 build
compiles but was not flashed.

## Controls

- `make -C components/coco/host check-dmk` covers a padded table, a
  misaligned pointer, the fixture named `.dsk`, a data mark after `14 A1
  A1` and a truncated file.
- These are byte-identical to the committed build:
  - both game replays from DSK and DMK, pictures and PCM;
  - the Disk BASIC format, save and load script;
  - NitrOS-9 EOU (6809 and 6309), its screens and the VHD.
- JVC and OS-9 floppies are still read as JVC.

## Not done

- Writing to DMK images.
- Gameplay past the first prompt, except for Kyum-Gai, Bash and Snake
  Pit.
- Barbarian Quest 2, Those Darn Marbles 2, Warrior King 2, Caladuril 2
  disk 2 and Zanth's scene disk were not reached through their games.
- Iron Forest keeps the MaTouch's core 1 at 100% at 60 fps. It was not
  profiled.
