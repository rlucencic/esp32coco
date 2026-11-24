"""Make a double-density DMK from a JVC (.DSK) image, for testing DMK reads.

The tracks are laid out as a 1793 formats them (gaps, sync bytes, index
mark, ID and data fields with their CRCs), the sectors interleaved, so the
IDs come out of order on the track. Each 6400-byte DMK track holds up to
18 sectors of 256 bytes.

    python3 jvc2dmk.py disk.dsk disk.dmk [--interleave 4] [--sides 1|2]
"""
import argparse
import binascii
import struct
from pathlib import Path

TRACK_LEN = 0x1900          # with the 128-byte table of ID pointers
SYNC = b'\xa1' * 3
GAP3 = 24


def crc(data):
    return struct.pack('>H', binascii.crc_hqx(SYNC + data, 0xffff))


def jvc_geometry(image, sides):
    header = len(image) % 256
    h = image[:header]
    spt = h[0] if header >= 1 and h[0] else 18
    if sides is None:
        sides = h[1] if header >= 2 and h[1] in (1, 2) else 1
    size_code = h[2] if header >= 3 and h[2] <= 3 else 1
    first = h[3] if header >= 4 else 1
    return header, spt, sides, 128 << size_code, size_code, first


def order(spt, interleave):
    """Logical sector index for each physical slot."""
    slots = [None] * spt
    p = 0
    for s in range(spt):
        while slots[p] is not None:
            p = (p + 1) % spt
        slots[p] = s
        p = (p + interleave) % spt
    return slots


def make_track(sectors, track, side, size_code, first, interleave):
    raw = bytearray(b'\x4e' * 32 + b'\0' * 12 + b'\xc2' * 3 + b'\xfc' + b'\x4e' * 32)
    ids = []
    for s in order(len(sectors), interleave):
        raw += b'\0' * 12 + SYNC
        ids.append(128 + len(raw))
        field = bytes([0xfe, track, side, first + s, size_code])
        raw += field + crc(field)
        raw += b'\x4e' * 22 + b'\0' * 12 + SYNC
        field = b'\xfb' + sectors[s]
        raw += field + crc(field) + b'\x4e' * GAP3
    if 128 + len(raw) > TRACK_LEN:
        raise SystemExit(f'track {track} side {side}: {len(raw)} bytes do not fit a DMK track')
    raw += b'\x4e' * (TRACK_LEN - 128 - len(raw))
    table = bytearray(128)
    for n, offset in enumerate(ids):
        struct.pack_into('<H', table, n * 2, offset | 0x8000)
    return table + raw


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('jvc', type=Path)
    parser.add_argument('dmk', type=Path)
    parser.add_argument('--interleave', type=int, default=4)
    parser.add_argument('--sides', type=int, choices=(1, 2))
    args = parser.parse_args()
    image = args.jvc.read_bytes()
    header, spt, sides, ssize, size_code, first = jvc_geometry(image, args.sides)
    body = image[header:]
    tracks = len(body) // (spt * sides * ssize)
    if not 0 < tracks <= 86:
        raise SystemExit(f'{tracks} tracks: not a floppy image')
    out = bytearray(16)
    struct.pack_into('<BBHB', out, 0, 0, tracks, TRACK_LEN, 0x10 if sides == 1 else 0)
    for t in range(tracks):
        for side in range(sides):
            base = (t * sides + side) * spt * ssize
            sectors = [body[base + s * ssize:base + (s + 1) * ssize] for s in range(spt)]
            out += make_track(sectors, t, side, size_code, first, args.interleave)
    args.dmk.write_bytes(out)
    print(f'{args.dmk}: {tracks} tracks, {sides} side(s), {spt} sectors of {ssize} bytes')


if __name__ == '__main__':
    main()
