/*
 * The CoCo 3's GIME (1986): its registers, the MMU, the palette, the timer
 * and its interrupts. After MAME's gime.cpp (Nathan Woods) and Kevin
 * Darling's and Sock Master's GIME references.
 *
 *   $FF90 INIT0  7 CoCo video, 6 MMU on, 5 IRQ on, 4 FIRQ on, 3 $FExx
 *                constant RAM, 2 standard SCS, 1-0 ROM map (0x 16 KB
 *                internal + 16 KB cartridge, 10 32 KB internal, 11 32 KB
 *                cartridge)
 *   $FF91 INIT1  5 timer input (1: 279 ns, 0: 63.5 us), 0 task
 *   $FF92 IRQ, $FF93 FIRQ: written the enables, read the pending (which
 *                clears them): 5 timer, 4 HBORD, 3 VBORD, 2 serial,
 *                1 keyboard, 0 cartridge
 *   $FF94-95     the 12-bit timer (writing $FF94 restarts it)
 *   $FF98-9F     video (gime_video.c)
 *   $FFA0-AF     MMU: 8 slots x 2 tasks, 6-bit block numbers
 *   $FFB0-BF     palette: 16 x 6 bits
 *   $FFC0-DF     the SAM's bits
 *
 * Interrupts are flagged on a rising edge of their line, if enabled; the
 * timer counts from its value + 2 to zero, flags, and starts again.
 */
#include <string.h>
#include "coco_internal.h"

gime_t gime;

static void restart_timer(uint64_t from)
{
    int value = ((gime.reg[4] & 0x0F) << 8) | gime.reg[5];
    if (value > 0) {
        uint64_t unit = (gime.reg[1] & 0x20) ? 1 : LINE_TICKS;
        gime.timer_at = from + (uint64_t)(value + 2) * unit;
    } else {
        gime.timer_at = NEVER;
    }
    coco_event_changed();
}

void gime_reset(void)
{
    memset(gime.reg, 0, sizeof(gime.reg));
    for (int i = 0; i < 8; i++) {
        gime.mmu[i] = gime.mmu[i + 8] = (uint8_t)(0x38 + i);
    }
    memset(gime.palette, 0, sizeof(gime.palette));
    memset(gime.line_colours, 0, sizeof(gime.line_colours));
    gime.changes = 0;
    gime.irq = gime.firq = 0;
    gime.lines = 0;
    gime.timer_at = NEVER;
    gime.blink = false;
    gime.legacy = false;
    gime.video_address = 0;
    gime.line_in_row = 0;
    gime.top_border = VDG_TOP_BORDER;
    gime.body = VDG_ACTIVE;
    memory_map();
    coco_update_interrupts();
}

void gime_set_line(uint8_t interrupt, bool level)
{
    bool was = (gime.lines & interrupt) != 0;
    if (level) {
        gime.lines |= interrupt;
    } else {
        gime.lines &= (uint8_t)~interrupt;
    }
    if (level && !was) {
        bool any = false;
        if (gime.reg[2] & interrupt) {
            gime.irq |= interrupt;
            any = true;
        }
        if (gime.reg[3] & interrupt) {
            gime.firq |= interrupt;
            any = true;
        }
        if (any) {
            coco_update_interrupts();
        }
    }
}

void gime_timer_event(void)
{
    restart_timer(gime.timer_at);
    gime_set_line(GIME_TMR, true);
    gime_set_line(GIME_TMR, false);
}

uint8_t gime_read(uint16_t a)
{
    switch (a & 0xFFF0) {
    case 0xFF90:
        if (a == 0xFF92) {
            uint8_t v = gime.irq;
            gime.irq = 0;
            coco_update_interrupts();
            return v;
        }
        if (a == 0xFF93) {
            uint8_t v = gime.firq;
            gime.firq = 0;
            coco_update_interrupts();
            return v;
        }
        return cpu_floating_bus();
    case 0xFFA0:
        return (uint8_t)((gime.mmu[a & 15] & 0x3F) | (cpu_floating_bus() & 0xC0));
    case 0xFFB0:
        return (uint8_t)((gime.palette[a & 15] & 0x3F) | (cpu_floating_bus() & 0xC0));
    default:
        return cpu_floating_bus();
    }
}

static void write_register(int r, uint8_t v)
{
    uint8_t changed = gime.reg[r] ^ v;
    gime.reg[r] = v;
    switch (r) {
    case 0:
        if (changed & 0x4B) {
            memory_map();
        }
        if (changed & 0x30) {
            coco_update_interrupts();
        }
        break;
    case 1:
        if (changed & 0x01) {
            memory_map();
        }
        if (changed & 0x20) {
            restart_timer(coco_now());
        }
        break;
    case 2:
    case 3:
        /* disabling an interrupt also clears it */
        if (r == 2) {
            gime.irq &= v;
        } else {
            gime.firq &= v;
        }
        coco_update_interrupts();
        if (gime.timer_at == NEVER && (changed & GIME_TMR)) {
            gime_set_line(GIME_TMR, true);      /* a timer of 0 */
            gime_set_line(GIME_TMR, false);
        }
        break;
    case 4:
        restart_timer(coco_now());
        break;
    case 0xA:
        gime_colour_written(16, v & 0x3F);
        break;
    default:
        break;
    }
}

void gime_write(uint16_t a, uint8_t v)
{
    if (a < 0xFFA0) {
        write_register(a & 15, v);
    } else if (a < 0xFFB0) {
        uint8_t block = v & 0x3F;
        if (gime.mmu[a & 15] != block) {
            gime.mmu[a & 15] = block;
            memory_map();
        }
    } else if (a < 0xFFC0) {
        gime.palette[a & 15] = v & 0x3F;
        gime_colour_written(a & 15, v & 0x3F);
    } else if (a < 0xFFE0) {
        sam_write(a - 0xFFC0);
    }
}
