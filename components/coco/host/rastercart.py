"""A CoCo 3 cartridge that measures where a mid-line palette write lands.

    python3 rastercart.py out.ccc [--first D0] [--mode 640|512]
        [--target palette|border] [--stripes] [--bin]     make the cartridge
    python3 rastercart.py check frame.ppm [--first D0]    read a picture of it
    python3 rastercart.py expect frame.ppm [--first D0] [--mode M] [--stripes]
                                                          compare with gime_video.c

The program copies itself to $1000 (a cartridge; --bin: a Disk BASIC
binary loaded there, for XRoar's -run), turns the PIAs' interrupts off
(SYNC ends on any interrupt line, masked or not), sets a 640 x 192
two-colour screen (VRES $14; --mode 512: 512 x 192, VRES $10) at logical
$4000 whose pixels are all palette 0 (black) but for a ruler on the first
four rows (a palette 1 byte at each end, a pixel every eighth of the
width), a red border, and the CPU to 1.79 MHz. Each frame, after VBORD and
78 HBORDs, come 88 blocks of two lines. Each block SYNCs to its first
line's HBORD (FIRQ masked: the next instruction starts at once), writes
white to palette 0 (--target border: $FF9A) with STA whose write cycle is
D + 11 cycles after that start, D = D0 + block, then black again at cycle
178 (64 cycles into the block's second line) and SYNCs to the next block.
--stripes writes eight colours, 7 cycles apart, from cycle D + 11.

`check` finds the blocks (from the first line with white) in a picture of
the 640-pixel area and prints, for each D, both lines' white. `expect`
checks a picture of our host against what gime_video.c draws (the stripes
pixel for pixel, in the host's RGB colours).
"""
import argparse
import sys
from pathlib import Path

BLOCKS = 88
SKIP = 78
RESTORE_CYCLE = 178
COL_A, COL_B, MARK, BORDER = 0x00, 0x3F, 0x12, 0x24
STRIPES = (0x3F, 0x24, 0x12, 0x09, 0x36, 0x2D, 0x1B, 0x07)    # --stripes: 7 cycles apart


class Asm:
    def __init__(self, org):
        self.org = org
        self.code = bytearray()

    @property
    def pc(self):
        return self.org + len(self.code)

    def emit(self, *data):
        for d in data:
            self.code += d if isinstance(d, (bytes, bytearray)) else bytes([d])

    def ext(self, op, address):
        self.emit(op, address >> 8, address & 255)

    def branch(self, op, target):
        offset = target - (self.pc + 2)
        assert -128 <= offset < 128
        self.emit(op, offset & 255)

    def delay(self, cycles):
        """Exactly `cycles` cycles (0 or at least 2); clobbers B."""
        assert cycles == 0 or cycles >= 2
        if cycles >= 12:
            pad = 2 + (cycles - 4) % 5          # 2..6 cycles, then LDB + DECB/BNE loops
            loops = (cycles - 2 - pad) // 5
            self.emit(0xC6, loops)              # LDB #n          2
            top = self.pc
            self.emit(0x5A)                     # DECB            2
            self.branch(0x26, top)              # BNE             3
            cycles = pad
        if cycles % 2:
            self.emit(0x21, 0x00)               # BRN             3
            cycles -= 3
        self.emit(*([0x12] * (cycles // 2)))    # NOP             2


def payload(first, mode=640, target=0xFFB0, stripes=False):
    row = 80 if mode == 640 else 64
    border = BORDER if target == 0xFFB0 else COL_A
    a = Asm(0x1000)
    a.emit(0x1A, 0x50)                          # ORCC #$50: no IRQ, FIRQ
    # The PIAs' interrupts off and their flags read away: SYNC ends on any
    # interrupt line, masked or not (BASIC leaves the 60 Hz one on)
    for control in (0xFF01, 0xFF03, 0xFF21, 0xFF23):
        a.ext(0xB6, control)                    # LDA control
        a.emit(0x84, 0xFE)                      # ANDA #$FE
        a.ext(0xB7, control)                    # STA control
        a.ext(0xB6, control - 1)                # LDA data: clears the flags
    a.ext(0xB7, 0xFFD9)                         # STA $FFD9: 1.79 MHz
    for reg, value in ((0xFF90, 0x58), (0xFF91, 0x00), (0xFF92, 0x00), (0xFF98, 0x80),
                       (0xFF99, 0x14 if mode == 640 else 0x10), (0xFF9A, border), (0xFF9C, 0x00), (0xFF9D, 0xE8), (0xFF9E, 0x00), (0xFF9F, 0x00),
                       (0xFFB0, COL_A), (0xFFB1, MARK)):
        a.emit(0x86, value)                     # LDA #value
        a.ext(0xB7, reg)                        # STA reg
    # The picture at logical $4000 (physical $74000): clear it, then the ruler
    a.emit(0x8E, 0x40, 0x00)                    # LDX #$4000
    a.emit(0xCC, 0x00, 0x00)                    # LDD #0
    top = a.pc
    a.emit(0xED, 0x81)                          # STD ,X++
    end = 0x4000 + row * 192
    a.emit(0x8C, end >> 8, end & 255)           # CMPX #end
    a.branch(0x26, top)                         # BNE
    for r in range(4):
        base = 0x4000 + r * row
        a.emit(0x86, 0xFF)
        a.ext(0xB7, base)
        a.ext(0xB7, base + row - 1)
        a.emit(0x86, 0x80)
        for col in range(row // 8, row, row // 8):
            a.ext(0xB7, base + col)
    frame = a.pc
    a.emit(0x86, 0x08)
    a.ext(0xB7, 0xFF93)                         # FIRQ from VBORD
    a.ext(0xB6, 0xFF93)                         # acknowledge
    a.emit(0x13)                                # SYNC
    a.emit(0x86, 0x10)
    a.ext(0xB7, 0xFF93)                         # FIRQ from HBORD
    a.ext(0xB6, 0xFF93)
    a.emit(0xC6, SKIP)                          # LDB #SKIP
    top = a.pc
    a.emit(0x13)                                # SYNC
    a.ext(0xB6, 0xFF93)                         # LDA $FF93
    a.emit(0x5A)                                # DECB
    a.branch(0x26, top)                         # BNE
    for block in range(BLOCKS):
        d = first + block
        a.emit(0x13)                            # SYNC: t = 0
        a.ext(0xB6, 0xFF93)                     # LDA $FF93      0-5
        if stripes:
            a.delay(d)                          # 5 to 5 + D; each STA starts 2 later
            for colour in STRIPES:              # written in cycles D + 11 + 7k
                a.emit(0x86, colour)            # LDA #colour    2
                a.ext(0xB7, target)             # STA            5
            a.delay(RESTORE_CYCLE - 18 - d - 7 * (len(STRIPES) - 1))
        else:
            a.emit(0x86, COL_B)                 # LDA #white     5-7
            a.delay(d)                          # 7 to 7 + D
            a.ext(0xB7, target)                 # STA $FFB0: written in cycle D + 11
            a.delay(RESTORE_CYCLE - 18 - d)
        a.emit(0x86, COL_A)                     # LDA #black
        a.ext(0xB7, target)                     # written in cycle 178
        a.ext(0xB6, 0xFF93)                     # the second line's HBORD
    a.emit(0x7E, frame >> 8, frame & 255)       # JMP frame
    return a.code


def cartridge(first, mode=640, target=0xFFB0, stripes=False):
    body = payload(first, mode, target, stripes)
    head = Asm(0xC000)
    head.emit(0x1A, 0x50)                       # ORCC #$50
    src = 0xC000 + 21
    head.emit(0x8E, src >> 8, src & 255)        # LDX #src
    head.emit(0x10, 0x8E, 0x10, 0x00)           # LDY #$1000
    top = head.pc
    head.emit(0xA6, 0x80)                       # LDA ,X+
    head.emit(0xA7, 0xA0)                       # STA ,Y+
    end = src + len(body)
    head.emit(0x8C, end >> 8, end & 255)        # CMPX #end
    head.branch(0x26, top)                      # BNE
    head.emit(0x7E, 0x10, 0x00)                 # JMP $1000
    assert len(head.code) == 21
    rom = head.code + body
    assert len(rom) <= 0x3000, len(rom)        # the code below the picture at $4000
    return bytes(rom) + bytes(0x4000 - len(rom))


def read_ppm(path):
    data = Path(path).read_bytes()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[pos:end])
        pos = end
    width, height = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:pos + 1 + width * height * 3]
    return width, height, [pixels[y * width * 3:(y + 1) * width * 3] for y in range(height)]


def white_span(row, width):
    """The first and last white pixel of a row, or None."""
    xs = [x for x in range(width) if row[x * 3:x * 3 + 3] == b'\xff\xff\xff']
    return (xs[0], xs[-1]) if xs else None


def check(path, first, left=0, width=None):
    """Each block's first line's white (from the write on) and its second
    line's (up to the restore): x in the 640-pixel area from `left`."""
    full, height, rows = read_ppm(path)
    width = width or full
    rows = [r[left * 3:(left + width) * 3] for r in rows]
    spans = [white_span(r, width) for r in rows]
    top = next((y for y, s in enumerate(spans) if s), None)
    if top is None:
        sys.exit('no white in the picture')
    results = []
    for block in range(BLOCKS):
        y = top + 2 * block
        if y + 1 >= height:
            break
        results.append((first + block, y, spans[y], spans[y + 1]))
    for d, y, a, b in results:
        fmt = lambda s: f'{s[0]:3}-{s[1]:3}' if s else '   -   '
        print(f'D {d:3}  write cycle {d + 11:3}  line {y:3}  first line {fmt(a)}  second line {fmt(b)}')
    return results


def expected(d, mode):
    """What gime_video.c draws for block D's palette writes (1.79 MHz:
    8 pixels a cycle; HBORD where the picture ends, 912 pixels a line)."""
    left = (640 - mode) // 2

    def into(cycle):
        return 8 * cycle - (912 - mode)
    x = into(d + 11)
    if x <= 0:
        first = (left, left + mode - 1)
    elif x < mode:
        first = (left + x, left + mode - 1)
    else:
        first = None
    x2 = into(RESTORE_CYCLE - 114)
    second = (left, left + x2 - 1) if x < mode else (left + max(x - 912, 0), left + x2 - 1)
    return first, second


def expect(path, first, mode):
    """Check a picture of our host against expected(); the count of blocks
    that differ."""
    bad = 0
    for d, y, a, b in check(path, first):
        want = expected(d, mode)
        if (a, b) != want:
            print(f'D {d}: got {a} {b}, expected {want[0]} {want[1]}')
            bad += 1
    return bad


def rgb(c):
    """The host's RGB monitor colour of a CoCo 3 colour (coco.c), as RGB565
    expanded back to 8 bits the way host_main.c writes PPMs."""
    def level(hi, lo):
        return (hi << 1 | lo) * 0x55
    r, g, b = level(c >> 5 & 1, c >> 2 & 1), level(c >> 4 & 1, c >> 1 & 1), level(c >> 3 & 1, c & 1)
    return r, g, b


def expect_stripes(path, first, mode):
    """Every pixel of each block's two lines against the stripes' timing."""
    width, height, rows = read_ppm(path)
    left = (640 - mode) // 2

    def colour_at(row, x):
        return tuple(row[x * 3:x * 3 + 3])
    whites = [y for y, r in enumerate(rows) if b'\xff\xff\xff' in bytes(r)]
    lookup = {}
    for c in (COL_A,) + STRIPES:
        probe = rgb(c)
        lookup[c] = probe
    # the PPM's own colours for them, from the RGB565 round trip
    def norm(t):
        r, g, b = t
        return (r >> 3, g >> 2, b >> 3)
    top = whites[0] if whites else None
    # the first block line: the first row (below the ruler) with any stripe colour
    names = {norm(lookup[c]): c for c in lookup}
    ruler_end = next(y for y in range(height) if norm(colour_at(rows[y], left)) != norm(rgb(MARK))
                     and y > 0 and norm(colour_at(rows[y - 1], left)) == norm(rgb(MARK)))
    top = next(y for y in range(ruler_end, height)
               if any(names.get(norm(colour_at(rows[y], x))) not in (None, COL_A) for x in range(left, left + mode)))
    bad = 0
    for block in range(BLOCKS):
        d = first + block
        # each write's pixel from the first line's picture start (the second
        # line's starts 912 later); a pixel shows the last write at or before it
        writes = [(8 * (d + 11 + 7 * k) - (912 - mode), c) for k, c in enumerate(STRIPES)]
        writes.append((8 * RESTORE_CYCLE - (912 - mode), COL_A))

        def shown(at):
            c = COL_A
            for pos, colour in writes:
                if pos <= at:
                    c = colour
            return c
        want1 = [shown(x) for x in range(mode)]
        want2 = [shown(912 + x) for x in range(mode)]
        for line, want in ((top + 2 * block, want1), (top + 2 * block + 1, want2)):
            got = [names.get(norm(colour_at(rows[line], left + x))) for x in range(mode)]
            if got != want:
                where = next(x for x in range(mode) if got[x] != want[x])
                print(f'D {d} line {line}: first difference at {left + where}: {got[where]} for {want[where]}')
                bad += 1
                break
    return bad


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'expect':
        parser = argparse.ArgumentParser()
        parser.add_argument('cmd')
        parser.add_argument('picture')
        parser.add_argument('--first', type=int, default=4)
        parser.add_argument('--mode', type=int, choices=(640, 512), default=640)
        parser.add_argument('--stripes', action='store_true')
        args = parser.parse_args()
        import contextlib, io
        with contextlib.redirect_stdout(io.StringIO()) as out:
            if args.stripes:
                bad = expect_stripes(args.picture, args.first, args.mode)
            else:
                bad = expect(args.picture, args.first, args.mode)
        if bad:
            sys.exit(out.getvalue() + f'{bad} blocks differ')
        print(f'{args.picture}: {BLOCKS} blocks as expected')
        return
    if len(sys.argv) > 1 and sys.argv[1] == 'check':
        parser = argparse.ArgumentParser()
        parser.add_argument('cmd')
        parser.add_argument('picture')
        parser.add_argument('--first', type=int, default=4)
        parser.add_argument('--left', type=int, default=0, help='the 640-pixel area\'s first column')
        parser.add_argument('--width', type=int, help='its width (default the picture\'s)')
        args = parser.parse_args()
        check(args.picture, args.first, args.left, args.width)
        return
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out', type=Path)
    parser.add_argument('--first', type=int, default=4)
    parser.add_argument('--mode', type=int, choices=(640, 512), default=640)
    parser.add_argument('--target', choices=('palette', 'border'), default='palette')
    parser.add_argument('--bin', action='store_true', help='a Disk BASIC binary at $1000 instead')
    parser.add_argument('--stripes', action='store_true', help='eight colours a line, 7 cycles apart')
    args = parser.parse_args()
    target = 0xFFB0 if args.target == 'palette' else 0xFF9A
    if args.bin:
        code = payload(args.first, args.mode, target, args.stripes)
        args.out.write_bytes(b'\x00' + len(code).to_bytes(2, 'big') + b'\x10\x00' + code + b'\xff\x00\x00\x10\x00')
    else:
        args.out.write_bytes(cartridge(args.first, args.mode, target, args.stripes))


if __name__ == '__main__':
    main()
