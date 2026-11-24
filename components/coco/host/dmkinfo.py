"""Find DMK images by content (archives name most of them .dsk) and
summarise their tracks the way fdc.c reads them.

    python3 dmkinfo.py FILE|DIR...

For each DMK: tracks, sides, track length; then each track that isn't a
plain 18 x 256 one, as its IDs in table order: the sector number, /tN for
an ID whose track isn't the physical one, /sN its side, /nN a size code
other than 1, /IDCRC, /nodata (no data mark within 43 bytes after two
A1s), /del (deleted data), /CRC (a bad data CRC); SD for a single-density
pointer and ?@offset for one that misses its FE (fdc.c ignores both).
The last line sums up what the disk needs from the controller.
"""
import binascii
import os
import struct
import sys

SYNC = b'\xa1' * 3


def crc_ok(data):
    return binascii.crc_hqx(SYNC + data, 0xffff) == 0


def parse(path):
    data = open(path, 'rb').read()
    if len(data) < 16 + 128:
        return None
    tracks, tlen, flags = data[1], struct.unpack_from('<H', data, 2)[0], data[4]
    sides = 1 if flags & 0x10 else 2
    if not tracks or tlen < 128 or data[12:16] != b'\0\0\0\0' or len(data) != 16 + tracks * sides * tlen:
        return None
    return data, tracks, sides, tlen, flags


def summary(path):
    p = parse(path)
    if not p:
        return None
    data, tracks, sides, tlen, flags = p
    lines = [f'{path}: {tracks} tracks, {sides} side(s), track length {tlen:#x}, flags {flags:#04x}, '
             f'write protect {data[0]:#04x}']
    notes = set()
    for t in range(tracks):
        for side in range(sides):
            base = 16 + (t * sides + side) * tlen
            track = data[base:base + tlen]
            ids = []
            for i in range(0, 128, 2):
                e = struct.unpack_from('<H', track, i)[0]
                if not e:
                    break
                ids.append(e)
            if not ids:
                lines.append(f'  track {t} side {side}: no IDs')
                notes.add('empty tracks')
                continue
            desc = []
            for e in ids:
                off, dd = e & 0x3FFF, bool(e & 0x8000)
                if not dd:
                    notes.add('single-density IDs')
                    desc.append('SD')
                    continue
                if off + 7 > tlen or track[off] != 0xFE:
                    desc.append(f'?@{off}')
                    notes.add('pointer not at FE')
                    continue
                trk, sd, sec, size = track[off + 1:off + 5]
                idcrc = crc_ok(track[off:off + 7])
                mark = None
                for m in range(off + 10, min(off + 7 + 43, tlen)):
                    if track[m] in (0xFB, 0xF8) and track[m - 2:m] == SYNC[:2]:
                        mark = m
                        break
                n = 128 << (size & 3)
                datacrc = crc_ok(track[mark:mark + 1 + n + 2]) if mark and mark + 3 + n <= tlen else None
                s = f'{sec:02X}'
                if trk != t:
                    s += f'/t{trk}'
                    notes.add('ID track != physical')
                if sd != side:
                    s += f'/s{sd}'
                if size != 1:
                    s += f'/n{size}'
                    notes.add(f'size code {size}')
                if not idcrc:
                    s += '/IDCRC'
                    notes.add('ID CRC errors')
                if mark is None:
                    s += '/nodata'
                    notes.add('IDs without data')
                elif track[mark] == 0xF8:
                    s += '/del'
                    notes.add('deleted data')
                if datacrc is False:
                    s += '/CRC'
                    notes.add('data CRC errors')
                desc.append(s)
            standard = len(desc) == 18 and all(len(x) == 2 for x in desc) and \
                sorted(int(x, 16) for x in desc) == list(range(1, 19))
            if not standard:
                lines.append(f'  track {t} side {side}: {len(desc)} IDs: ' + ' '.join(desc))
    lines.append('  needs: ' + (', '.join(sorted(notes)) if notes else 'nothing unusual (18 x 256 everywhere)'))
    return '\n'.join(lines)


if __name__ == '__main__':
    for root in sys.argv[1:]:
        if os.path.isfile(root):
            s = summary(root)
            if s:
                print(s)
            continue
        for dirpath, _, files in sorted(os.walk(root)):
            for f in sorted(files):
                s = summary(os.path.join(dirpath, f))
                if s:
                    print(s)
