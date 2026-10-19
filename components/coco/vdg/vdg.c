/*
 * The MC6847 VDG (CoCo 2), and its modes as the CoCo 3's GIME shows them.
 *
 * Its mode comes from PIA1 PB3-7 (CSS, GM0 + INT/EXT, GM1, GM2, A/G) and,
 * in the text modes, from each byte (bit 7 A/S, bit 6 INV):
 *
 *   text         32 x 16, 5 x 7 in 8 x 12 cells; normal video (INV) dark
 *                green on green (orange with CSS)
 *   SG4          bit 7 set: 2 x 2 blocks in 8 colours (bits 6-4)
 *   SG6          ... with INT/EXT: 2 x 3 blocks in 4 (bits 7-6, + CSS)
 *   CG1 RG1 CG2 RG2 CG3 RG3 CG6 RG6   the graphics modes, 64 to 256 wide,
 *                4 colours (two sets by CSS) or 2 (black and green or buff)
 *
 * The SAM's counter fetches the bytes (16 or 32 a line; the SAM's V bits
 * repeat lines). PMODE 4 (RG6) on a TV: pairs of pixels as artifact colours.
 * The CoCo 3 (an MC6847T1, as MAME has it): GM0 gives lower case for
 * $00-$1F, GM1 inverts the text, no SG6, and the colours are its palette's.
 */
#include <string.h>
#include "coco_internal.h"

static uint8_t s_vdg_map[16];           /* VDG colour -> colour number (CoCo 2) */

void vdg_init(void)
{
    vdg_forget_lines();
    for (int i = 0; i < 16; i++) {
        s_vdg_map[i] = (uint8_t)(64 + i);
    }
}

enum { AG = 0x80, GM2 = 0x40, GM1 = 0x20, GM0 = 0x10, CSS = 0x08 };

int vdg_bytes(uint8_t pb)
{
    if (pb & AG) {
        switch ((pb >> 4) & 7) {
        case 0: case 1: case 3: case 5:
            return 16;
        default:
            return 32;
        }
    }
    return 32;
}


/* 512 colour numbers of a line from its n bytes; row_in_char 0-11 */
void COCO_HOT vdg_emit(uint8_t pb, const uint8_t *data, int n, int row_in_char, const uint8_t *map, bool t1,
                       coco_artifact_t artifacts, uint8_t *out)
{
    bool css = (pb & CSS) != 0;
    int byte_px = 512 / n;
    if (pb & AG) {
        int gm = (pb >> 4) & 7;
        if (gm == 0 || gm == 2 || gm == 4 || gm == 6) {
            /* four colours, 2 bits a pixel */
            int px = byte_px / 4;
            const uint8_t *set = map + (css ? 4 : 0);
            for (int i = 0; i < n; i++) {
                uint8_t d = data[i];
                for (int j = 6; j >= 0; j -= 2) {
                    out = coco_fill(out, set[(d >> j) & 3], px);
                }
            }
            return;
        }
        uint8_t c0 = map[css ? 10 : 8], c1 = map[css ? 11 : 9];
        int px = byte_px / 8;
        if (gm == 7 && artifacts != COCO_ARTIFACT_OFF && map == s_vdg_map) {
            /* pairs of 256-wide pixels: 00 black, 11 white, 10 and 01 colours */
            uint8_t a = css ? 80 : 82, b = css ? 81 : 83;
            if (artifacts == COCO_ARTIFACT_RED_BLUE) {
                uint8_t t = a;
                a = b;
                b = t;
            }
            for (int i = 0; i < n; i++) {
                uint8_t d = data[i];
                for (int j = 6; j >= 0; j -= 2) {
                    int pair = (d >> j) & 3;
                    uint8_t c = pair == 0 ? c0 : pair == 3 ? c1 : pair == 2 ? a : b;
                    out = coco_fill(out, c, 2 * px);
                }
            }
            return;
        }
        for (int i = 0; i < n; i++) {
            uint8_t d = data[i];
            for (int j = 7; j >= 0; j--) {
                out = coco_fill(out, (d >> j) & 1 ? c1 : c0, px);
            }
        }
        return;
    }

    /* text and semigraphics: 32 cells of 16 pixels */
    uint8_t dark = map[css ? 14 : 12], bright = map[css ? 15 : 13], black = map[8];
    bool intext = (pb & GM0) != 0;
    for (int i = 0; i < 32; i++) {
        uint8_t c = data[i];
        uint8_t left, right;
        if (c & 0x80) {
            if (!intext || t1) {
                /* SG4: rows 0-5 bits 3 and 2, rows 6-11 bits 1 and 0 */
                int shift = row_in_char < 6 ? 2 : 0;
                uint8_t on = map[(c >> 4) & 7];
                left = (c >> (shift + 1)) & 1 ? on : black;
                right = (c >> shift) & 1 ? on : black;
            } else {
                /* SG6: rows 0-3 bits 5 and 4, 4-7 bits 3 and 2, 8-11 bits 1 and 0 */
                int shift = row_in_char < 4 ? 4 : row_in_char < 8 ? 2 : 0;
                uint8_t on = map[((c >> 6) & 3) + (css ? 4 : 0)];
                left = (c >> (shift + 1)) & 1 ? on : black;
                right = (c >> shift) & 1 ? on : black;
            }
            out = coco_fill(out, left, 8);
            out = coco_fill(out, right, 8);
            continue;
        }
        uint8_t bits;                   /* 8 pixels, bit 7 the left; set = bright */
        if (intext && !t1) {
            bits = (uint8_t)~c;         /* no external ROM: "stripes" */
        } else {
            bool inv = (c & 0x40) != 0;
            const uint8_t *glyph;
            bool lower = t1 && intext && !inv && c < 0x20;
            glyph = lower ? vdg_font_lower[c] : vdg_font[c & 0x3F];
            int r = row_in_char - 3;
            uint8_t g = (r >= 0 && r < 7) ? (uint8_t)(glyph[r] << 2) : 0;
            /* normal video (INV, or the T1's lower case): dark on bright */
            bits = (inv || lower) ? (uint8_t)~g : g;
            if (t1 && (pb & GM1)) {
                bits = (uint8_t)~bits;
            }
        }
        for (int j = 7; j >= 0; j--) {
            out = coco_fill(out, (bits >> j) & 1 ? bright : dark, 2);
        }
    }
}

/* The border: black for text, green or buff (CSS) for graphics */
static uint8_t border(uint8_t pb)
{
    if (pb & AG) {
        return s_vdg_map[(pb & CSS) ? 4 : 0];
    }
    return s_vdg_map[8];
}

/* The frame is in PSRAM, slow to compare with: a hash of each line (in
 * internal RAM) says whether it changed, and only then is it copied
 */
static uint32_t s_line_hash[COCO_SCREEN_HEIGHT];

void vdg_forget_lines(void)
{
    memset(s_line_hash, 0, sizeof(s_line_hash));
}

void COCO_HOT vdg_commit(int row, const uint8_t *line)
{
    const uint32_t *w = (const uint32_t *)line;
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < COCO_SCREEN_WIDTH / 4; i += 4) {
        h = (h ^ w[i]) * 0x01000193u;
        h = (h ^ w[i + 1]) * 0x01000193u;
        h = (h ^ w[i + 2]) * 0x01000193u;
        h = (h ^ w[i + 3]) * 0x01000193u;
    }
    h |= 1;                             /* 0: not drawn yet */
    bool changed = h != s_line_hash[row];
    if (changed) {
        s_line_hash[row] = h;
        memcpy(coco.cfg.frame + row * COCO_SCREEN_WIDTH, line, COCO_SCREEN_WIDTH);
    }
    coco_line_rendered(row, changed);
}

void COCO_HOT vdg_render_line(int line)
{
    static uint8_t buf[COCO_SCREEN_WIDTH] __attribute__((aligned(4)));
    int row = line - FIRST_SHOWN_LINE;
    bool shown = row >= 0 && row < COCO_SCREEN_HEIGHT;
    uint8_t pb = pia_output(1, 1);
    uint8_t edge = border(pb);
    if (line >= VDG_TOP_BORDER && line < VDG_TOP_BORDER + VDG_ACTIVE) {
        uint8_t data[32];
        int n = vdg_bytes(pb);
        for (int k = 0; k < n; k++) {
            data[k] = sam_fetch(k & 1);
        }
        if (!shown) {
            return;
        }
        int y = line - VDG_TOP_BORDER;
        memset(buf, edge, 64);
        memset(buf + 576, edge, 64);
        vdg_emit(pb, data, n, y % 12, s_vdg_map, false, coco.cfg.artifacts, buf + 64);
    } else {
        if (!shown) {
            return;
        }
        memset(buf, edge, COCO_SCREEN_WIDTH);
    }
    vdg_commit(row, buf);
}
