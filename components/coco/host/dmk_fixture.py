"""Generate an original double-density track with nonsequential IDs.

Sectors (ID track 7, side 0): 3 deleted data, 1 of 256 bytes, 2 with a bad
data CRC, 4 whose data runs over sector 5's ID, 5 with no data mark, 6 with
its data mark too late (60 bytes after the ID), 8 with size code 4 (128
bytes: the 1793 uses the low two bits).
"""
import argparse
import binascii
import struct
from pathlib import Path

TRACK_LEN = 0x1900
SYNC = b'\xa1' * 3


def crc(data):
    return struct.pack('>H', binascii.crc_hqx(SYNC + data, 0xffff))


def record(payload):
    return SYNC + payload + crc(payload)


def make_dmk():
    header = bytearray(16)
    struct.pack_into('<BBHB', header, 0, 0, 1, TRACK_LEN, 0x10)
    track = bytearray(b'\0' * 128 + b'\x4e' * (TRACK_LEN - 128))
    sectors = [
        # ID offset, sector, size code, data mark, fill, data mark distance
        (160, 3, 0, 0xf8, 0x33, 20),
        (520, 1, 1, 0xfb, 0x11, 20),
        (900, 2, 0, 0xfb, 0x22, 20),
        (1200, 4, 0, 0xfb, 0x44, 20),
        (1260, 5, 0, None, None, None),
        (1600, 6, 0, 0xfb, 0x66, 60),
        (2000, 8, 4, 0xfb, 0x88, 20),
    ]
    for index, (offset, sector, size_code, mark, fill, distance) in enumerate(sectors):
        struct.pack_into('<H', track, index * 2, offset | 0x8000)
        id_record = record(bytes([0xfe, 7, 0, sector, size_code]))
        track[offset - 3:offset - 3 + len(id_record)] = id_record
        if mark is None:
            continue
        data_record = bytearray(record(bytes([mark]) + bytes([fill]) * (128 << (size_code & 3))))
        if sector == 2:
            data_record[-1] ^= 1
        if sector == 8:
            data_record[0] = 0x14       # the first A1 as Read Track gives it (CRC over three)
        start = offset + distance
        track[start:start + len(data_record)] = data_record
    # Sector 4's data now holds sector 5's ID: its CRC over what is there
    mark = 1200 + 23
    track[mark + 129:mark + 131] = crc(bytes(track[mark:mark + 129]))
    # The rest of the table as tools leave it: Diecom's $0080 filler, a
    # pointer that misses its FE, one past the track (all to be ignored)
    struct.pack_into('<HHH', track, 2 * len(sectors), 0x0080, 0x8000 | 1199, 0x8000 | 0x3FFF)
    for i in range(2 * len(sectors) + 6, 128, 2):
        struct.pack_into('<H', track, i, 0x0080)
    return header + track


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('valid', type=Path)
    parser.add_argument('invalid', type=Path)
    output = parser.parse_args()
    image = make_dmk()
    output.valid.write_bytes(image)
    output.invalid.write_bytes(image[:-1])          # its size and header disagree
