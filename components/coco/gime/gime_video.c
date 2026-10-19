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

static uint8_t border_colour(void)
{
    if (!gime.legacy) {
        return gime.reg[0xA] & 0x3F;
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
static void COCO_HOT graphics_line(uint8_t *out, int width)
{
    static const uint8_t bytes_for[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
    static uint8_t pix[COCO_SCREEN_WIDTH];
    uint8_t vres = gime.reg[9];
    int bytes = bytes_for[(vres >> 2) & 7];
    int cres = vres & 3;
    int bpp = cres == 0 ? 1 : cres == 1 ? 2 : 4;
    if (bytes >= 128 && bpp == 1) {
        bpp = 2;
    }
    uint32_t start = (gime.reg[0xF] & 0x7F) * 2u;
    uint32_t base = gime.video_address;
    const uint8_t *pal = gime.palette;
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
        default:
            for (int j = 7; j >= 0; j--) {
                *p++ = pal[(d >> j) & 1];
            }
            break;
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

static void COCO_HOT text_line(uint8_t *out, int width)
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
    const uint8_t *pal = gime.palette;
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

void COCO_HOT gime_render_line(int line)
{
    static uint8_t buf[COCO_SCREEN_WIDTH] __attribute__((aligned(4)));
    int row = line - FIRST_SHOWN_LINE;
    bool shown = row >= 0 && row < COCO_SCREEN_HEIGHT;
    uint8_t edge = border_colour();
    if (!gime_video_body(line)) {
        if (shown) {
            memset(buf, edge, sizeof(buf));
            vdg_commit(row, buf);
        }
        return;
    }

    int pitch;
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
        if (shown) {
            memset(buf, edge, 64);
            memset(buf + 576, edge, 64);
            vdg_emit(pb, data, n, gime.line_in_row, gime.palette, true, COCO_ARTIFACT_OFF, buf + 64);
        }
        pitch = n;
    } else {
        bool wide = (gime.reg[9] & 0x04) != 0;
        uint8_t *body = wide ? buf : buf + 64;
        int width = wide ? 640 : 512;
        if (shown) {
            if (!wide) {
                memset(buf, edge, 64);
                memset(buf + 576, edge, 64);
            }
            if (gime.reg[8] & 0x80) {
                graphics_line(body, width);
            } else {
                text_line(body, width);
            }
        }
        if (gime.reg[8] & 0x80) {
            pitch = graphics_pitch();
        } else {
            static const uint8_t cols_for[4] = { 32, 40, 64, 80 };
            uint8_t vres = gime.reg[9];
            pitch = cols_for[((vres >> 2) & 1) | ((vres >> 3) & 2)] * ((vres & 1) ? 2 : 1);
        }
        if (gime.reg[0xF] & 0x80) {
            pitch = 256;
        }
    }
    if (shown) {
        vdg_commit(row, buf);
    }
    if (++gime.line_in_row >= lines_per_row()) {
        gime.line_in_row = 0;
        gime.video_address += (uint32_t)pitch;
    }
}
