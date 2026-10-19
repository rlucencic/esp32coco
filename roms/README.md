# Color Computer ROM images

The CoCo's ROMs are Tandy's and Microsoft's: they are copyrighted and not
part of this repository. Put your own images here before `idf.py flash`
(the build writes them into the `coco_roms` flash partition), or copy them
into the `coco/roms` folder on the SD card. Either way, the names below.

| File | What | Size | CRC32 | SHA-1 |
|---|---|---|---|---|
| `bas13.rom` | Color BASIC 1.3 (CoCo 2) | 8 KB | D8F4D15E | 28b92bebe35fa4f026a084416d6ea3b1552b63d3 |
| `bas12.rom` | Color BASIC 1.2 (instead of 1.3) | 8 KB | 54368805 | 0f14dc46c647510eb0b7bd3f53e33da07907d04f |
| `extbas11.rom` | Extended Color BASIC 1.1 (CoCo 2) | 8 KB | A82A6254 | ad927fb4f30746d820cb8b860ebb585e7f095dea |
| `disk11.rom` | Disk Extended Color BASIC 1.1 (the disk controller's) | 8 KB | 0B9C5415 | 10bdc5aa2d7d7f205f67b47b19003a4bd89defd1 |
| `coco3.rom` | The CoCo 3's ROM (Color BASIC 2.0, with Extended and Super Extended BASIC) | 32 KB | B4C88D6C | e0d82953fb6fd03768604933df1ce8bc51fc427d |

These are the images MAME and XRoar use (the same checksums); collections
of them are at, for example,
[colorcomputerarchive.com](https://colorcomputerarchive.com/repo/ROMs/XRoar/CoCo/BASIC_OS/)
(names there may differ: rename them to the ones above).

- The CoCo 2 needs Color BASIC; Extended BASIC is optional (without it,
  plain Color BASIC).
- The CoCo 3 needs `coco3.rom`.
- Disk BASIC (`disk11.rom`, or `disk10.rom` for 1.0) is the disk
  controller's ROM, for both machines: without it, there is no disk
  controller (floppies) in the cartridge slot. The hard disk (the VHD
  interface) doesn't need it; booting OS-9 from a floppy does.

## In flash

`idf.py flash` writes whichever of the files exist into the 64 KB
`coco_roms` partition at 0x810000, in 8 KB slots:

| Slot | Offset | File |
|---|---|---|
| 0 | 0x810000 | `bas13.rom` (else `bas12.rom`) |
| 1 | 0x812000 | `extbas11.rom` |
| 2 | 0x814000 | `disk11.rom` (else `disk10.rom`) |
| 4-7 | 0x818000 | `coco3.rom` (32 KB) |

`idf.py app-flash` leaves them as they are. The boards are shared with the
other projects, whose ROM partitions are at the same place: after one of
them, flash this one again with `idf.py flash`.

## On the SD card

`coco/roms/<name>` (upper or lower case), used for any ROM the flash
partition doesn't have. The web page's status lists the ROMs in use; the
setup screen says which one is missing if the machine can't start.
