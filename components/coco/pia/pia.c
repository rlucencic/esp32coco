/*
 * MC6821 PIA, two of them. Registers 0 and 2 the data (or the direction,
 * with control bit 2 clear), 1 and 3 the controls:
 *
 *   bit 0  C1 interrupt enable             bit 3-5  C2: input (bit 5 clear:
 *   bit 1  C1 edge (1 rising)                       3 enable, 4 edge) or
 *   bit 2  data / direction                         output (bit 5 set: 4 set
 *   bit 6  C2 flag, bit 7 C1 flag (read only)       = manual, the level in 3)
 *
 * Reading a data register clears its side's flags. The machine (coco.c)
 * gives the input pins and hears about changed outputs. The handshake and
 * pulse modes of C2 are idle high (the CoCo doesn't use them).
 */
#include "coco_internal.h"

pia_t pia[2];

void pia_reset(void)
{
    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < 2; s++) {
            pia[p].ddr[s] = pia[p].out[s] = pia[p].ctl[s] = 0;
            pia[p].c2_in[s] = true;
        }
    }
    coco_pia_changed(0);
    coco_pia_changed(1);
    coco_update_interrupts();
}

static uint8_t port_value(int p, int side)
{
    pia_t *x = &pia[p];
    uint8_t in = coco_pia_input(p, side);
    return (uint8_t)((x->out[side] & x->ddr[side]) | (in & ~x->ddr[side]));
}

uint8_t COCO_HOT pia_read(int p, int reg)
{
    pia_t *x = &pia[p];
    int side = reg >> 1;
    if (reg & 1) {
        return x->ctl[side];
    }
    if (!(x->ctl[side] & 0x04)) {
        return x->ddr[side];
    }
    uint8_t v = port_value(p, side);
    if (x->ctl[side] & 0xC0) {
        x->ctl[side] &= 0x3F;
        coco_update_interrupts();
    }
    return v;
}

uint8_t pia_peek(int p, int reg)
{
    pia_t *x = &pia[p];
    int side = reg >> 1;
    if (reg & 1) {
        return x->ctl[side];
    }
    return (x->ctl[side] & 0x04) ? port_value(p, side) : x->ddr[side];
}

void COCO_HOT pia_write(int p, int reg, uint8_t v)
{
    pia_t *x = &pia[p];
    int side = reg >> 1;
    if (reg & 1) {
        x->ctl[side] = (uint8_t)((x->ctl[side] & 0xC0) | (v & 0x3F));
        if (v & 0x20) {
            x->ctl[side] &= ~0x40;      /* C2 an output: no C2 interrupts */
        }
        coco_update_interrupts();
    } else if (x->ctl[side] & 0x04) {
        x->out[side] = v;
    } else {
        x->ddr[side] = v;
    }
    coco_pia_changed(p);
}

void COCO_HOT pia_set_c1(int p, int side, bool level)
{
    pia_t *x = &pia[p];
    if (x->c1[side] == level) {
        return;
    }
    x->c1[side] = level;
    bool rising_active = (x->ctl[side] & 0x02) != 0;
    if (level == rising_active && !(x->ctl[side] & 0x80)) {
        x->ctl[side] |= 0x80;
        if (x->ctl[side] & 0x01) {
            coco_update_interrupts();
        }
    }
}

bool pia_c2(int p, int side)
{
    uint8_t c = pia[p].ctl[side];
    if ((c & 0x30) == 0x30) {
        return (c & 0x08) != 0;
    }
    return true;
}

bool pia_irq(int p)
{
    for (int side = 0; side < 2; side++) {
        uint8_t c = pia[p].ctl[side];
        if ((c & 0x81) == 0x81) {
            return true;
        }
        if ((c & 0x68) == 0x48) {
            return true;
        }
    }
    return false;
}
