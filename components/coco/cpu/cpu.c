/*
 * The 6809's bus: RAM and ROM through the page tables (memory.c), the rest
 * here. Page $FF is always I/O:
 *
 *   CoCo 2: $FF00 PIA0 and $FF20 PIA1 (each repeated through its 32 bytes),
 *           $FF40 the cartridge's (the disk controller), $FF80 the VHD
 *           interface, $FFC0 the SAM (written), $FFE0 the vectors (from
 *           Color BASIC's $BFE0)
 *   CoCo 3: $FF00-$FF0F PIA0, $FF20-$FF2F PIA1, $FF40, $FF80 as above,
 *           $FF90-$FFDF the GIME (and its SAM bits), $FFE0 the vectors
 *           (from the ROM's $FFE0, whatever is mapped at $E000)
 *
 * Anything else reads the floating bus (MAME's coco.cpp, from Darren A):
 * the next instruction byte for a ,R indexed read, else the byte at $FFFF.
 */
#include "coco_internal.h"

static uint8_t vector_byte(uint16_t address)
{
    if (coco.coco3) {
        return coco.cfg.coco3_rom[0x7FE0 + (address & 0x1F)];
    }
    return coco.cfg.basic[0x1FE0 + (address & 0x1F)];
}

uint8_t cpu_floating_bus(void)
{
    const m6809_t *c = &coco.cpu;
    uint16_t at = c->ppc;
    uint8_t op = memory_peek(at);
    if (op == 0x10 || op == 0x11) {
        op = memory_peek(++at);
    }
    uint8_t row = op & 0xF0;
    bool no_offset = (row == 0x60 || row == 0xA0 || row == 0xE0) && (memory_peek((uint16_t)(at + 1)) & 0xBF) == 0x84;
    return no_offset ? memory_peek(c->pc) : vector_byte(0xFFFF);
}

static bool in_disk_controller(void)
{
    return coco.cfg.disk_rom && !cart_rom;
}

uint8_t COCO_HOT io_read(uint16_t a)
{
    if (a >= 0xFFE0) {
        return vector_byte(a);
    }
    if (coco.coco3) {
        switch (a & 0xFFF0) {
        case 0xFF00: return pia_read(0, a & 3);
        case 0xFF20: return pia_read(1, a & 3);
        case 0xFF40:
        case 0xFF50: return in_disk_controller() ? fdc_read(a) : cpu_floating_bus();
        case 0xFF80: return a <= 0xFF86 ? vhd_read(a) : cpu_floating_bus();
        case 0xFF90:
        case 0xFFA0:
        case 0xFFB0: return gime_read(a);
        default: return cpu_floating_bus();
        }
    }
    switch (a & 0xFFE0) {
    case 0xFF00: return pia_read(0, a & 3);
    case 0xFF20: return pia_read(1, a & 3);
    case 0xFF40: return in_disk_controller() ? fdc_read(a) : cpu_floating_bus();
    case 0xFF80: return a <= 0xFF86 ? vhd_read(a) : cpu_floating_bus();
    default: return cpu_floating_bus();
    }
}

void COCO_HOT io_write(uint16_t a, uint8_t v)
{
    if (coco.coco3) {
        switch (a & 0xFFF0) {
        case 0xFF00: pia_write(0, a & 3, v); return;
        case 0xFF20: pia_write(1, a & 3, v); return;
        case 0xFF40:
        case 0xFF50:
            if (in_disk_controller()) {
                fdc_write(a, v);
            }
            return;
        case 0xFF80:
            if (a <= 0xFF86) {
                vhd_write(a, v);
            }
            return;
        default:
            if (a >= 0xFF90 && a < 0xFFE0) {
                gime_write(a, v);
            }
            return;
        }
    }
    switch (a & 0xFFE0) {
    case 0xFF00: pia_write(0, a & 3, v); return;
    case 0xFF20: pia_write(1, a & 3, v); return;
    case 0xFF40:
        if (in_disk_controller()) {
            fdc_write(a, v);
        }
        return;
    case 0xFF80:
        if (a <= 0xFF86) {
            vhd_write(a, v);
        }
        return;
    case 0xFFC0: sam_write(a & 0x1F); return;
    default: return;
    }
}

static uint8_t COCO_HOT bus_read(void *ctx, uint16_t a)
{
    (void)ctx;
    if (a >= 0xFF00) {
        return io_read(a);
    }
    return cpu_floating_bus();          /* an empty cartridge slot */
}

static void COCO_HOT bus_write(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    if (a >= 0xFF00) {
        io_write(a, v);
    }
    /* ROM and the empty slot: nothing */
}

void cpu_init(void)
{
    static const m6809_bus_t bus = { .read = bus_read, .write = bus_write, .ctx = NULL };
    m6809_init(&coco.cpu, &bus, coco.cfg.h6309);
}
