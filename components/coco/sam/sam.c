/*
 * The MC6883 SAM: 16 bits set and cleared through $FFC0-$FFDF (an even
 * address clears its bit, the odd one sets it):
 *
 *   V0-V2   the video counter's mode       F0-F6  display offset (x 512)
 *   P1      page #1                        R0-R1  the CPU's rate
 *   M0-M1   memory size                    TY     all RAM
 *
 * The video address counter (as MAME's 6883sam, with Ciaran Anscomb's and
 * Phill Harvey-Smith's findings): each fetch with the VDG's address bit 0
 * changed bumps bits 0-3; their carry toggles bit 4 every 1-3 carries and
 * that one's carry adds $20 every 1-12 (by V); the horizontal sync clears
 * bits 0-3 or 0-4, carrying as it does. Field sync reloads the offset.
 *
 * The CoCo 3's GIME has the same bits (TY, R, V, F for its CoCo 2 video);
 * its own video doesn't use the counter.
 */
#include "coco_internal.h"

sam_t sam;

void sam_reset(void)
{
    sam.state = 0;
    sam.counter = 0;
    sam.xdiv = sam.ydiv = 0;
    coco_rate_changed();
}

void sam_write(int offset)
{
    uint16_t mask = (uint16_t)(1u << (offset >> 1));
    uint16_t old = sam.state;
    sam.state = (offset & 1) ? (sam.state | mask) : (sam.state & ~mask);
    uint16_t changed = old ^ sam.state;
    if (changed & (SAM_TY | SAM_M1 | SAM_M0 | SAM_P1)) {
        memory_map();
    }
    if (changed & (SAM_R1 | SAM_R0)) {
        coco_rate_changed();
    }
}

void sam_counter_reset(void)
{
    sam.counter = sam_display_offset();
    sam.xdiv = sam.ydiv = 0;
}

static void carry_bit4(void)
{
    static const uint8_t ydiv[8] = { 12, 1, 3, 1, 2, 1, 1, 1 };
    if (++sam.ydiv >= ydiv[sam_vmode()]) {
        sam.ydiv = 0;
        sam.counter += 0x20;
    }
}

static void carry_bit3(void)
{
    static const uint8_t xdiv[8] = { 1, 3, 1, 2, 1, 1, 1, 1 };
    if (++sam.xdiv >= xdiv[sam_vmode()]) {
        sam.xdiv = 0;
        sam.counter ^= 0x10;
        if (!(sam.counter & 0x10)) {
            carry_bit4();
        }
    }
}

uint8_t COCO_HOT sam_fetch(bool da0)
{
    if (da0 != (sam.counter & 1)) {
        bool carry = (sam.counter & 0x0F) == 0x0F;
        sam.counter = (uint16_t)((sam.counter & ~0x0F) | ((sam.counter + 1) & 0x0F));
        if (carry) {
            carry_bit3();
        }
    }
    /* 4K and 16K modes repeat their RAM */
    uint16_t mask = (sam.state & SAM_M1) ? 0xFFFF : (sam.state & SAM_M0) ? 0x3FFF : 0x0FFF;
    return coco.cfg.ram[sam.counter & mask];
}

void sam_hsync(void)
{
    switch (sam_vmode()) {
    case 1: case 3: case 5: {
        bool carry = (sam.counter & 0x08) != 0;
        sam.counter &= ~0x0F;
        if (carry) {
            carry_bit3();
        }
        break;
    }
    case 0: case 2: case 4: case 6: {
        bool carry = (sam.counter & 0x10) != 0;
        sam.counter &= ~0x1F;
        if (carry) {
            carry_bit4();
        }
        break;
    }
    default:
        break;                          /* 7: DMA */
    }
}
