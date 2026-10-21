/*
 * Free space on the disks, for the web page (esp32emu's diskspace): a
 * floppy's OS-9 bitmap (its LSNs over both sides) or else Disk BASIC's
 * granule table (side 0); the hard disk's OS-9 bitmap, read from its file.
 *
 * Worked out at insert and after writes are flushed (both with the
 * emulation stopped: the hard disk's file is the one the VHD interface
 * uses), kept for the web page's task.
 */
#include "coco_internal.h"
#include "diskspace.h"

#define DRIVES      (COCO_DRIVES + 1)       /* the hard disk last */

typedef struct {
    volatile bool known;
    volatile uint64_t free, size;
} space_t;

static space_t s_space[DRIVES];

static bool os9_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    return fdc_disk_read((int)(intptr_t)ctx, false, offset, buf, len);
}

static bool rsdos_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    return fdc_disk_read((int)(intptr_t)ctx, true, offset, buf, len);
}

static bool hd_read(void *ctx, uint64_t offset, void *buf, uint32_t len)
{
    return vhd_read_bytes(offset, buf, len);
}

void space_update(int drive)
{
    if (drive < 0 || drive >= DRIVES) {
        return;
    }
    diskspace_t s = { 0 };
    bool known = false;
    if (drive == COCO_DRIVES) {
        diskspace_disk_t hd = { hd_read, NULL, vhd_bytes() };
        known = hd.size > 0 && diskspace_os9(&hd, &s);
    } else {
        void *ctx = (void *)(intptr_t)drive;
        diskspace_disk_t os9 = { os9_read, ctx, fdc_disk_bytes(drive, false) };
        diskspace_disk_t rsdos = { rsdos_read, ctx, fdc_disk_bytes(drive, true) };
        known = os9.size > 0 && (diskspace_os9(&os9, &s) || diskspace_rsdos(&rsdos, &s));
    }
    s_space[drive].known = false;
    s_space[drive].free = s.free;
    s_space[drive].size = s.size;
    s_space[drive].known = known;
}

static bool get(int drive, uint64_t *free_bytes, uint64_t *size)
{
    if (!s_space[drive].known) {
        return false;
    }
    *free_bytes = s_space[drive].free;
    *size = s_space[drive].size;
    return s_space[drive].known && *size > 0;
}

bool coco_disk_space(int drive, uint64_t *free_bytes, uint64_t *size)
{
    return drive >= 0 && drive < COCO_DRIVES && get(drive, free_bytes, size);
}

bool coco_hd_space(uint64_t *free_bytes, uint64_t *size)
{
    return get(COCO_DRIVES, free_bytes, size);
}
