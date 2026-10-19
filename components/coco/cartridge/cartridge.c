/*
 * The cartridge slot: a ROM cartridge (up to 32 KB at $C000-$FEFF; on the
 * CoCo 3 a 32 KB one also fills $8000-$BFFF in its external ROM mode), or
 * else the disk controller with Disk BASIC (fdc.c), or nothing.
 *
 * A ROM cartridge's CART line is tied to the CPU's Q clock: edges on PIA1
 * CB1 all the time, so the ROM's FIRQ (once it enables it) jumps into the
 * cartridge; on the CoCo 3 the same line is the GIME's EI0.
 */
#include <stdio.h>
#include <string.h>
#include "coco_internal.h"

#define CART_MAX        (32 * 1024)

const uint8_t *cart_rom;
uint32_t cart_size;
static uint8_t *s_buffer;
static char s_path[256];
static bool s_level;

void cart_reset(void)
{
    s_level = false;
}

void cart_line(void)
{
    if (!cart_rom) {
        return;
    }
    s_level = !s_level;
    pia_set_c1(1, 1, s_level);
    if (coco.coco3) {
        gime_set_line(GIME_EI0, s_level);
    }
}

bool coco_cart_insert(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    if (!s_buffer) {
        s_buffer = coco_malloc(CART_MAX);
        if (!s_buffer) {
            fclose(f);
            return false;
        }
    }
    size_t n = fread(s_buffer, 1, CART_MAX, f);
    fclose(f);
    if (n == 0) {
        return false;
    }
    /* repeat a small image through 16 KB (as its address lines leave it) */
    uint32_t size = 1;
    while (size < n) {
        size <<= 1;
    }
    for (uint32_t i = (uint32_t)n; i < size; i++) {
        s_buffer[i] = 0xFF;
    }
    cart_rom = s_buffer;
    cart_size = size;
    snprintf(s_path, sizeof(s_path), "%s", path);
    memory_map();
    COCO_LOG("cartridge %s, %u bytes", path, (unsigned)n);
    return true;
}

void coco_cart_eject(void)
{
    cart_rom = NULL;
    cart_size = 0;
    s_path[0] = 0;
    memory_map();
}

const char *coco_cart_path(void)
{
    return cart_rom ? s_path : NULL;
}

bool coco_disk_controller(void)
{
    return coco.cfg.disk_rom && !cart_rom;
}
