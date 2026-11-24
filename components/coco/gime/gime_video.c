/*
 * The GIME's picture.
 *
 *   $FF98 VMODE  7 graphics, 5 burst phase, 4 monochrome, 3 50 Hz, 2-0
 *                lines a row (1 1 2 8 9 10 11, 7: the whole field)
 *   $FF99 VRES   6-5 lines (192 200 - 225), 4-2 bytes a row (16 20 32 40
 *                64 80 128 160; text 32 40 64 80 columns), 1-0 colours (2 4
 *                16; text: bit 0 attributes)
 *   $FF9A        border colour     $FF9C  vertical scroll (lines into a row)
 *   $FF9D-9E     the picture's address / 8 (latched at the top of a field)
 *   $FF9F        7 a 256-byte virtual line, 6-0 the line's start / 2 (each
 *                line wraps within 256 bytes)
 *
 * The rows of 20, 40, 80 or 160 bytes fill all 640 pixels; the others are
 * 512 wide between 64-pixel borders. Text: 8 x 8 characters, with
 * attributes 7 blink, 6 underline, 5-3 foreground (palette 8-15), 2-0
 * background (0-7); without them palette 1 on 0.
 *
 * With $FF90 bit 7 (CoCo video) the GIME shows the CoCo 2's modes: the
 * address from the SAM's F bits (and $FF9D-9E's top bits), lines per row
 * from its V bits, the VDG's colours from palette 0-15 (which the ROM sets
 * up to look like them).
 */
#include <string.h>
#include "coco_internal.h"

void gime_frame_start(void)
{
    gime.legacy = (gime.reg[0] & 0x80) != 0;
    if (gime.legacy) {
        gime.video_address = sam_display_offset() + ((uint32_t)(gime.reg[0xE] & 0x3F) << 3) +
                             ((uint32_t)(gime.reg[0xD] & 0xE0) << 11);
        gime.line_in_row = 0;
    } else {
        gime.video_address = (uint32_t)gime.reg[0xE] << 3 | (uint32_t)gime.reg[0xD] << 11;
        gime.line_in_row = gime.reg[0xC] & 0x0F;
    }
    switch (gime.reg[9] & 0x60) {
    case 0x20: gime.top_border = 23; gime.body = 200; break;
    case 0x60: gime.top_border = 8; gime.body = 225; break;
    default: gime.top_border = VDG_TOP_BORDER; gime.body = VDG_ACTIVE; break;
    }
    if ((coco.frames & 15) == 0) {
        gime.blink = !gime.blink;
    }
}

bool gime_video_body(int line)
{
    return line >= gime.top_border && line < gime.top_border + gime.body;
}

static int lines_per_row(void)
{
    if (gime.legacy) {
        uint8_t pb = pia_output(1, 1);
        if (pb & 0x80) {
            static const uint8_t graphics[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };
            return graphics[sam_vmode()];
        }
        static const uint8_t alpha[16] = { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 4, 3, 2, 1, 12 };
        return alpha[gime.reg[0xC] & 0x0F];
    }
    static const int lpr[8] = { 1, 1, 2, 8, 9, 10, 11, 1 << 30 };
    return lpr[gime.reg[8] & 7];
}

static uint8_t border_colour(const uint8_t *colours)
{
    if (!gime.legacy) {
        return colours[16];
    }
    uint8_t pb = pia_output(1, 1);
    if (pb & 0x80) {
        return (pb & 0x08) ? 0x3F : 0x12;      /* buff (white) or green */
    }
    if ((pb & 0x40) && !(pb & 0x20)) {
        return (pb & 0x08) ? 0x26 : 0x12;      /* the T1's coloured text border */
    }
    return 0x00;
}

static inline uint8_t ram_at(uint32_t address)
{
    return coco.cfg.ram[address & (coco.ram_size - 1)];     /* 64, 128 or 512 KB */
}

/* Widen a line of pixels (one byte each) by px into out (4-byte aligned;
 * n x px a multiple of 4)
 */
static void COCO_HOT widen(uint8_t *out, const uint8_t *pix, int n, int px)
{
    uint32_t *o = (uint32_t *)out;
    switch (px) {
    case 1:
        memcpy(out, pix, (size_t)n);
        break;
    case 2:
        for (int i = 0; i < n; i += 2) {
            *o++ = pix[i] * 0x0101u | (uint32_t)(pix[i + 1] * 0x0101u) << 16;
        }
        break;
    case 4:
        for (int i = 0; i < n; i++) {
            *o++ = pix[i] * 0x01010101u;
        }
        break;
    case 8:
        for (int i = 0; i < n; i++) {
            uint32_t w = pix[i] * 0x01010101u;
            *o++ = w;
            *o++ = w;
        }
        break;
    default:
        for (int i = 0; i < n; i++) {
            out = coco_fill(out, pix[i], px);
        }
        break;
    }
}

/* Graphics: bytes a row, bits a pixel, by $FF99 bits 4-0 (as MAME has
 * them; the 128 and 160 byte rows have no 2-colour mode)
 */
/* A nibble's 4 pixels as bytes 0/1, the left one first (lowest address) */
static const uint32_t nibble_bits[16] = {
    0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
    0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

static void COCO_HOT graphics_line(uint8_t *out, int width, const uint8_t *pal)
{
    static const uint8_t bytes_for[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
    static uint8_t pix[COCO_SCREEN_WIDTH] __attribute__((aligned(4)));
    uint8_t vres = gime.reg[9];
    int bytes = bytes_for[(vres >> 2) & 7];
    int cres = vres & 3;
    int bpp = cres == 0 ? 1 : cres == 1 ? 2 : 4;
    if (bytes >= 128 && bpp == 1) {
        bpp = 2;
    }
    uint32_t start = (gime.reg[0xF] & 0x7F) * 2u;
    uint32_t base = gime.video_address;
    uint8_t *p = pix;
    for (int i = 0; i < bytes; i++) {
        uint8_t d = ram_at(base + ((start + (uint32_t)i) & 0xFF));
        switch (bpp) {
        case 4:
            p[0] = pal[d >> 4];
            p[1] = pal[d & 15];
            p += 2;
            break;
        case 2:
            p[0] = pal[d >> 6];
            p[1] = pal[(d >> 4) & 3];
            p[2] = pal[(d >> 2) & 3];
            p[3] = pal[d & 3];
            p += 4;
            break;
        default: {
            /* 4 pixels a word: each 0/1 byte times (colour 0 ^ colour 1),
             * then ^ colour 0 (colours below 64: no carries)
             */
            uint32_t c0 = pal[0] * 0x01010101u, flip = (uint32_t)(pal[0] ^ pal[1]);
            uint32_t *w = (uint32_t *)p;
            w[0] = (nibble_bits[d >> 4] * flip) ^ c0;
            w[1] = (nibble_bits[d & 15] * flip) ^ c0;
            p += 8;
            break;
        }
        }
    }
    int n = (int)(p - pix);
    widen(out, pix, n, width / n);
}

static int graphics_pitch(void)
{
    static const uint8_t bytes_for[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
    return bytes_for[(gime.reg[9] >> 2) & 7];
}

static void COCO_HOT text_line(uint8_t *out, int width, const uint8_t *pal)
{
    static uint8_t pix[COCO_SCREEN_WIDTH];
    uint8_t vres = gime.reg[9];
    static const uint8_t cols_for[4] = { 32, 40, 64, 80 };
    int cols = cols_for[((vres >> 2) & 1) | ((vres >> 3) & 2)];
    bool attributes = (vres & 1) != 0;
    int row = gime.line_in_row;
    int underline = -1;
    switch (gime.reg[8] & 7) {
    case 3: underline = 7; break;
    case 4: case 5: underline = 8; break;
    case 6: underline = 9; break;
    default: break;
    }
    uint32_t start = (gime.reg[0xF] & 0x7F) * 2u;
    uint32_t base = gime.video_address;
    uint8_t *p = pix;
    for (int i = 0; i < cols; i++) {
        uint32_t off = attributes ? (uint32_t)i * 2 : (uint32_t)i;
        uint8_t ch = ram_at(base + ((start + off) & 0xFF));
        uint8_t bg = pal[0], fg = pal[1];
        bool under = false;
        if (attributes) {
            uint8_t attr = ram_at(base + ((start + off + 1) & 0xFF));
            bg = pal[attr & 7];
            fg = pal[8 + ((attr >> 3) & 7)];
            under = (attr & 0x40) != 0;
            if ((attr & 0x80) && gime.blink) {
                ch = ' ';
                under = false;
            }
        }
        uint8_t bits = row < 8 ? gime_font[ch & 0x7F][row] : 0;
        if (under && row == underline) {
            bits = 0xFF;
        }
        for (int j = 0; j < 8; j++) {
            p[j] = (bits >> j) & 1 ? fg : bg;
        }
        p += 8;
    }
    widen(out, pix, cols * 8, width / (cols * 8));
}

/* ---- Colour writes inside a line ----
 *
 * A line (a run of the CPU) starts at HBORD, which comes where the
 * picture ends (XRoar's scope measurements of '86 and '87 GIMEs): a line
 * is 912 pixels (228 ticks), so the next 640-pixel picture starts 272
 * pixels after HBORD, a 512-pixel one 400 (shown from frame pixel 64).
 *
 * A palette write shows from the pixel the beam is at: the line is drawn
 * with the colours where its picture starts, then again from each write's
 * pixel with the colours after it. A write before the picture's first
 * pixel counts for the whole line; one after its last, for the next. The
 * write's time: its instruction's start plus WRITE_CYCLE (STA and STB
 * extended write in their fifth cycle; other stores within a cycle or two
 * of that). The border colour is taken once a line, 22 pixels before the
 * picture (XRoar's '86 GIME): later writes count from the next line.
 */
#define WRITE_CYCLE         4           /* STA/STB extended: the write in cycle 4 of 0-4 */
#define BORDER_LATCH        22          /* pixels before the picture */

void gime_colour_written(int index, uint8_t value)
{
    bool wide = !gime.legacy && (gime.reg[9] & 0x04);
    int width = wide ? 640 : 512;
    int tick = (int)(coco_now() - coco.line_tick) + (WRITE_CYCLE << coco.tick_shift);
    int x = tick * 4 - (912 - width);   /* pixels into the picture */
    if (index == 16) {
        if (x <= -BORDER_LATCH) {
            gime.line_colours[16] = value;
        }
        return;
    }
    if (x <= 0) {
        gime.line_colours[index] = value;
    } else if (x < width) {
        if (gime.changes < GIME_LINE_CHANGES) {
            gime.change[gime.changes].x = (uint16_t)(x + (COCO_SCREEN_WIDTH - width) / 2);
            gime.change[gime.changes].index = (uint8_t)index;
            gime.change[gime.changes].value = value;
            gime.changes++;
        }
    }
}

/* The line's 640 colour numbers with these colours (no state changes: a
 * line may be drawn more than once)
 */
static void COCO_HOT draw(uint8_t *buf, const uint8_t *colours, bool body)
{
    uint8_t edge = border_colour(colours);
    if (!body) {
        memset(buf, edge, COCO_SCREEN_WIDTH);
        return;
    }
    if (gime.legacy) {
        uint8_t pb = pia_output(1, 1);
        int n = vdg_bytes(pb);
        if ((pb & 0x80) && n == 16 && !(sam.state & SAM_V0)) {
            n = 32;                     /* the GIME takes the width from V0 */
        }
        uint8_t data[32];
        for (int k = 0; k < n; k++) {
            data[k] = ram_at(gime.video_address + (uint32_t)k);
        }
        memset(buf, edge, 64);
        memset(buf + 576, edge, 64);
        vdg_emit(pb, data, n, gime.line_in_row, colours, true, COCO_ARTIFACT_OFF, buf + 64);
        return;
    }
    bool wide = (gime.reg[9] & 0x04) != 0;
    if (!wide) {
        memset(buf, edge, 64);
        memset(buf + 576, edge, 64);
    }
    uint8_t *out = wide ? buf : buf + 64;
    int width = wide ? 640 : 512;
    if (gime.reg[8] & 0x80) {
        graphics_line(out, width, colours);
    } else {
        text_line(out, width, colours);
    }
}

static int pitch(void)
{
    if (gime.legacy) {
        uint8_t pb = pia_output(1, 1);
        int n = vdg_bytes(pb);
        if ((pb & 0x80) && n == 16 && !(sam.state & SAM_V0)) {
            n = 32;
        }
        return n;
    }
    if (gime.reg[0xF] & 0x80) {
        return 256;
    }
    if (gime.reg[8] & 0x80) {
        return graphics_pitch();
    }
    static const uint8_t cols_for[4] = { 32, 40, 64, 80 };
    uint8_t vres = gime.reg[9];
    return cols_for[((vres >> 2) & 1) | ((vres >> 3) & 2)] * ((vres & 1) ? 2 : 1);
}

/* A body line with palette writes inside it: from each one's pixel on, the
 * colours after it. The GIME's modes draw the pixels' palette entries once
 * (an identity palette; the border is entry 16) and look each stretch up
 * in its colours; the CoCo 2 modes (whose border isn't a palette entry)
 * draw the line again for each.
 */
static void COCO_HOT draw_with_changes(uint8_t *buf)
{
    static const uint8_t identity[17] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    static uint8_t entries[COCO_SCREEN_WIDTH] __attribute__((aligned(4)));
    uint8_t colours[17];
    memcpy(colours, gime.line_colours, sizeof(colours));
    if (gime.legacy) {
        draw(buf, colours, true);
    } else {
        draw(entries, identity, true);
        for (int x = 0; x < gime.change[0].x; x++) {
            buf[x] = colours[entries[x]];
        }
    }
    for (int k = 0; k < gime.changes;) {
        int at = gime.change[k].x;
        for (; k < gime.changes && gime.change[k].x == at; k++) {
            colours[gime.change[k].index] = gime.change[k].value;
        }
        if (gime.legacy) {
            draw(entries, colours, true);
            memcpy(buf + at, entries + at, (size_t)(COCO_SCREEN_WIDTH - at));
            continue;
        }
        int end = k < gime.changes ? gime.change[k].x : COCO_SCREEN_WIDTH;
        for (int x = at; x < end; x++) {
            buf[x] = colours[entries[x]];
        }
    }
}

void COCO_HOT gime_render_line(int line)
{
    static uint8_t buf[COCO_SCREEN_WIDTH] __attribute__((aligned(4)));
    int row = line - FIRST_SHOWN_LINE;
    bool body = gime_video_body(line);
    if (row >= 0 && row < COCO_SCREEN_HEIGHT) {
        if (gime.changes && body) {
            draw_with_changes(buf);
        } else {
            draw(buf, gime.line_colours, body);
        }
        vdg_commit(row, buf);
    }
    /* The next line starts with the colours as they are now */
    memcpy(gime.line_colours, gime.palette, 16);
    gime.line_colours[16] = gime.reg[0xA] & 0x3F;
    gime.changes = 0;
    if (body && ++gime.line_in_row >= lines_per_row()) {
        gime.line_in_row = 0;
        gime.video_address += (uint32_t)pitch();
    }
}
