#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * The Color Computer ROMs: built in, from the coco_roms flash partition
 * (roms/<name> in the project, written by idf.py flash into its slots). A
 * file in the SD card's "coco/roms" folder (the cartridges' folder)
 * overrides one: its name or, for some, another version that also does
 * (Color BASIC 1.2 for 1.3, Disk BASIC 1.0 for 1.1).
 */
typedef enum {
    ROM_BASIC,                  /* bas13.rom (or bas12.rom): Color BASIC, 8 KB */
    ROM_EXTENDED,               /* extbas11.rom: Extended Color BASIC 1.1, 8 KB */
    ROM_DISK,                   /* disk11.rom (or disk10.rom): Disk BASIC, 8 KB */
    ROM_COCO3,                  /* coco3.rom: the CoCo 3's, 32 KB */
    ROM_COUNT
} rom_id_t;

const char *rom_store_name(rom_id_t id);    /* "bas13.rom", or the one that was found */
uint32_t rom_store_size(rom_id_t id);

/* The whole ROM into buf; false if there's none */
bool rom_store_load(rom_id_t id, uint8_t *buf);

/* A built-in ROM's file name (either version): an override, not a cartridge */
bool rom_store_is_rom(const char *name);
