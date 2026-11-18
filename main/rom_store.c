#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "emu/sdcard.h"
#include "rom_store.h"

#define ROM_PARTITION_TYPE  0x40
#define ROM_FOLDER          SDCARD_MOUNT_POINT "/coco/roms"
#define SLOT                8192

static const char *TAG = "rom";

static const struct {
    const char *name, *other;
    uint8_t slot;
    uint32_t size;
} roms[ROM_COUNT] = {
    { "bas13.rom", "bas12.rom", 0, 8192 },
    { "extbas11.rom", NULL, 1, 8192 },
    { "disk11.rom", "disk10.rom", 2, 8192 },
    { "coco3.rom", NULL, 4, 32768 },
};

static const char *s_found[ROM_COUNT];

const char *rom_store_name(rom_id_t id)
{
    if (id >= ROM_COUNT) {
        return "";
    }
    return s_found[id] ? s_found[id] : roms[id].name;
}

uint32_t rom_store_size(rom_id_t id)
{
    return id < ROM_COUNT ? roms[id].size : 0;
}

/* Not erased flash, not zeros: something at the start of most 2 KB blocks */
static bool plausible(const uint8_t *p, uint32_t size)
{
    int blank = 0, blocks = 0;
    for (uint32_t off = 0; off < size; off += 2048, blocks++) {
        bool ff = true, zero = true;
        for (int i = 0; i < 32; i++) {
            ff &= p[off + i] == 0xFF;
            zero &= p[off + i] == 0x00;
        }
        blank += ff || zero;
    }
    return blank * 4 <= blocks;
}

/* Through a mapping, not esp_partition_read(): that turns the caches off
 * while it reads, which stalls the LCD's refill (picture artefacts)
 */
static bool from_partition(uint32_t offset, uint32_t size, uint8_t *buf)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ROM_PARTITION_TYPE,
                                                           "coco_roms");
    if (part == NULL || offset + size > part->size) {
        return false;
    }
    const void *map;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, offset, size, ESP_PARTITION_MMAP_DATA, &map, &handle) != ESP_OK) {
        return false;
    }
    memcpy(buf, map, size);
    esp_partition_munmap(handle);
    return true;
}

/* coco/roms/<name> on the SD card (any case) */
static long from_sd(const char *name, uint8_t *buf, uint32_t max)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", ROM_FOLDER, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        for (char *p = path + strlen(ROM_FOLDER) + 1; *p; p++) {
            *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        }
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        return -1;
    }
    long n = fread(buf, 1, max, f);
    fclose(f);
    if (n > 0) {
        ESP_LOGI(TAG, "%s from the SD card", name);
    }
    return n;
}

bool rom_store_load(rom_id_t id, uint8_t *buf)
{
    if (id >= ROM_COUNT) {
        return false;
    }
    uint32_t size = rom_store_size(id);
    /* the card's overrides the built-in one */
    if (from_sd(roms[id].name, buf, size) == (long)size && plausible(buf, size)) {
        s_found[id] = roms[id].name;
        return true;
    }
    if (roms[id].other && from_sd(roms[id].other, buf, size) == (long)size && plausible(buf, size)) {
        s_found[id] = roms[id].other;
        return true;
    }
    s_found[id] = NULL;
    return from_partition(roms[id].slot * SLOT, size, buf) && plausible(buf, size);
}

bool rom_store_is_rom(const char *name)
{
    for (int i = 0; i < ROM_COUNT; i++) {
        if (strcasecmp(name, roms[i].name) == 0 || (roms[i].other && strcasecmp(name, roms[i].other) == 0)) {
            return true;
        }
    }
    return false;
}
