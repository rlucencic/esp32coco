/*
 * The memory maps, as the CPU's 256-byte pages (NULL: the bus callbacks).
 *
 * CoCo 2 (the SAM, 64 KB): $0000-$7FFF RAM (the upper 32 KB with P1, in
 * the ROM map), $8000 Extended BASIC, $A000 Color BASIC, $C000-$FEFF the
 * cartridge (a ROM, the disk controller's Disk BASIC, or nothing); with TY
 * (all RAM) $0000-$FEFF RAM. Writes to the ROMs are lost.
 *
 * CoCo 3 (the GIME): eight 8 KB slots, each a block of the 512 KB address
 * space from the MMU (task 0 or 1, $FF91 TR) when it's on ($FF90 MMUEN),
 * else blocks $38-$3F. Blocks $3C-$3F are ROM unless TY: the internal 32 KB
 * or the cartridge's, as $FF90 MC1-MC0 choose (the ROM page goes with the
 * slot, not the block, as MAME has it). $FE00-$FEFF is block $3F's RAM when
 * MC3 is set. 128 KB machines see their RAM repeated.
 */
#include "coco_internal.h"

static void set_pages(int first, int count, const uint8_t *rd, uint8_t *wr)
{
    m6809_t *c = &coco.cpu;
    for (int i = 0; i < count; i++) {
        c->rd[first + i] = rd ? (uint8_t *)rd + i * 256 : NULL;
        c->wr[first + i] = wr ? wr + i * 256 : NULL;
    }
}

/* The slot's ROM at $C000-$FEFF (CoCo 2) */
static const uint8_t *slot_rom(uint32_t offset)
{
    if (cart_rom) {
        return cart_rom + offset % cart_size;
    }
    if (coco.cfg.disk_rom) {
        return coco.cfg.disk_rom + (offset & 0x1FFF);
    }
    return NULL;
}

static void map_coco2(void)
{
    uint8_t *ram = coco.cfg.ram;
    uint16_t st = sam.state;
    bool m64 = (st & SAM_M1) != 0;
    uint32_t low = (m64 && (st & (SAM_TY | SAM_P1)) == SAM_P1) ? 0x8000 : 0;
    set_pages(0x00, 0x80, ram + low, ram + low);
    if (m64 && (st & SAM_TY)) {
        set_pages(0x80, 0x7F, ram + 0x8000, ram + 0x8000);
    } else {
        set_pages(0x80, 0x20, coco.cfg.extended, NULL);
        set_pages(0xA0, 0x20, coco.cfg.basic, NULL);
        for (int p = 0xC0; p < 0xFF; p++) {
            const uint8_t *r = slot_rom((uint32_t)(p - 0xC0) * 256);
            coco.cpu.rd[p] = (uint8_t *)r;
            coco.cpu.wr[p] = NULL;
        }
    }
    coco.cpu.rd[0xFF] = coco.cpu.wr[0xFF] = NULL;
}

/* The CoCo 3's cartridge ROM for a slot (32 KB images: $8000 is their
 * $4000), or the disk controller's
 */
static const uint8_t *cart_page(int slot)
{
    uint32_t offset = (uint32_t)(((slot & 3) ^ 2) * 0x2000);
    if (cart_rom) {
        return cart_rom + offset % cart_size;
    }
    if (coco.cfg.disk_rom) {
        return coco.cfg.disk_rom + (offset & 0x1FFF);
    }
    return NULL;
}

static void map_slot(int slot, int first_page, int pages, uint32_t offset, bool constant_ram)
{
    uint8_t init0 = gime.reg[0];
    int block;
    if ((init0 & 0x40) && !constant_ram) {
        block = gime.mmu[slot + ((gime.reg[1] & 1) ? 8 : 0)] & 0x3F;
    } else {
        block = 0x38 + slot;
    }
    if (block >= 0x3C && !(sam.state & SAM_TY) && !constant_ram) {
        int rom_mode = init0 & 3;
        const uint8_t *rom;
        if (rom_mode == 3 || (rom_mode < 2 && block >= 0x3E)) {
            rom = cart_page(slot);
        } else {
            rom = coco.cfg.coco3_rom + (slot & 3) * 0x2000;
        }
        set_pages(first_page, pages, rom ? rom + offset : NULL, NULL);
        return;
    }
    uint8_t *ram = coco.cfg.ram + ((uint32_t)block * 0x2000) % coco.ram_size + offset;
    set_pages(first_page, pages, ram, ram);
}

static void map_coco3(void)
{
    for (int slot = 0; slot < 7; slot++) {
        map_slot(slot, slot * 32, 32, 0, false);
    }
    map_slot(7, 0xE0, 30, 0, false);                    /* $E000-$FDFF */
    map_slot(7, 0xFE, 1, 0x1E00, (gime.reg[0] & 0x08) != 0);
    coco.cpu.rd[0xFF] = coco.cpu.wr[0xFF] = NULL;
}

void memory_map(void)
{
    if (coco.coco3) {
        map_coco3();
    } else {
        map_coco2();
    }
}

uint8_t memory_peek(uint16_t a)
{
    const uint8_t *page = coco.cpu.rd[a >> 8];
    if (page) {
        return page[a & 0xFF];
    }
    if (a >= 0xFFE0) {
        return coco.coco3 ? coco.cfg.coco3_rom[0x7FE0 + (a & 0x1F)] : coco.cfg.basic[0x1FE0 + (a & 0x1F)];
    }
    if (a >= 0xFF00 && a < 0xFF40 && (coco.coco3 ? (a & 0x10) == 0 : true)) {
        return pia_peek((a >> 5) & 1, a & 3);
    }
    return 0xFF;
}
