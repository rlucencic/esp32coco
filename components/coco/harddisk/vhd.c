/*
 * The VHD interface (as MAME's coco_vhd.cpp, and the emulators' emudsk
 * driver for NitrOS-9 uses it): a hard disk of 256-byte sectors.
 *
 *   $FF80-82  the sector number (24 bits, high byte first)
 *   $FF83     command: 0 read the sector into memory, 1 write it from
 *             memory, 2 flush; status (read): 0 done, 2 no disk, 5 can't
 *             (read only, or the file failed), $FE unknown command, $FF
 *             nothing yet since the reset
 *   $FF84-85  the buffer's address, as the CPU sees memory now
 *   $FF86     the drive (0; 1 has none here)
 *
 * The image is read and written through to its file (it can be large);
 * writing past its end makes it longer. coco_media_flush() syncs the file.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "coco_internal.h"

enum { VHD_OK = 0x00, VHD_NONE = 0x02, VHD_DENIED = 0x05, VHD_UNKNOWN = 0xFE, VHD_POWER_ON = 0xFF };

static struct {
    FILE *fp;
    char path[256];
    bool read_only;
    uint32_t sectors;                   /* the file's size / 256 */
    uint32_t lsn;
    uint16_t buffer;
    uint8_t status, drive;
    bool dirty;
    uint32_t reads, writes;
    uint32_t last;
    uint32_t busy_frames;               /* recently used (the status' light) */
} vhd;

void vhd_reset(void)
{
    vhd.status = VHD_POWER_ON;
    vhd.lsn = 0;
    vhd.buffer = 0;
    vhd.drive = 0;
}

static void cpu_write(uint16_t a, uint8_t v)
{
    uint8_t *page = coco.cpu.wr[a >> 8];
    if (page) {
        page[a & 0xFF] = v;
    } else if (a >= 0xFF00) {
        io_write(a, v);
    }
}

static uint8_t cpu_read(uint16_t a)
{
    const uint8_t *page = coco.cpu.rd[a >> 8];
    return page ? page[a & 0xFF] : 0xFF;
}

static uint8_t command(uint8_t cmd)
{
    if (vhd.drive != 0 || !vhd.fp) {
        return VHD_NONE;
    }
    uint8_t buf[256];
    long pos = (long)vhd.lsn * 256;
    switch (cmd) {
    case 0:
        memset(buf, 0, sizeof(buf));
        if (vhd.lsn < vhd.sectors) {
            if (fseek(vhd.fp, pos, SEEK_SET) != 0 || fread(buf, 1, 256, vhd.fp) != 256) {
                return VHD_DENIED;
            }
        }
        for (int i = 0; i < 256; i++) {
            cpu_write((uint16_t)(vhd.buffer + i), buf[i]);
        }
        vhd.reads++;
        break;
    case 1:
        if (vhd.read_only) {
            return VHD_DENIED;
        }
        for (int i = 0; i < 256; i++) {
            buf[i] = cpu_read((uint16_t)(vhd.buffer + i));
        }
        if (vhd.lsn > vhd.sectors) {
            /* past the end: zeros up to it */
            static const uint8_t zero[256];
            fseek(vhd.fp, (long)vhd.sectors * 256, SEEK_SET);
            while (vhd.sectors < vhd.lsn) {
                if (fwrite(zero, 1, 256, vhd.fp) != 256) {
                    return VHD_DENIED;
                }
                vhd.sectors++;
            }
        }
        if (fseek(vhd.fp, pos, SEEK_SET) != 0 || fwrite(buf, 1, 256, vhd.fp) != 256) {
            return VHD_DENIED;
        }
        if (vhd.lsn >= vhd.sectors) {
            vhd.sectors = vhd.lsn + 1;
        }
        vhd.dirty = true;
        vhd.writes++;
        break;
    case 2:
        fflush(vhd.fp);
        break;
    default:
        return VHD_UNKNOWN;
    }
    vhd.last = vhd.lsn;
    vhd.busy_frames = coco.frames + 30;
    return VHD_OK;
}

uint8_t vhd_read(uint16_t a)
{
    return a == 0xFF83 ? vhd.status : 0;
}

void vhd_write(uint16_t a, uint8_t v)
{
    switch (a) {
    case 0xFF80: vhd.lsn = (vhd.lsn & 0x00FFFF) | (uint32_t)v << 16; break;
    case 0xFF81: vhd.lsn = (vhd.lsn & 0xFF00FF) | (uint32_t)v << 8; break;
    case 0xFF82: vhd.lsn = (vhd.lsn & 0xFFFF00) | v; break;
    case 0xFF83: vhd.status = command(v); break;
    case 0xFF84: vhd.buffer = (uint16_t)((vhd.buffer & 0x00FF) | v << 8); break;
    case 0xFF85: vhd.buffer = (uint16_t)((vhd.buffer & 0xFF00) | v); break;
    case 0xFF86: vhd.drive = v; break;
    default: break;
    }
}

bool coco_hd_insert(const char *path)
{
    coco_hd_eject();
    bool ro = false;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        f = fopen(path, "rb");
        ro = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    vhd.sectors = (uint32_t)(ftell(f) / 256);
    vhd.fp = f;
    vhd.read_only = ro;
    vhd.dirty = false;
    snprintf(vhd.path, sizeof(vhd.path), "%s", path);
    COCO_LOG("hard disk %s, %lu sectors%s", path, (unsigned long)vhd.sectors, ro ? ", read only" : "");
    return true;
}

void coco_hd_eject(void)
{
    if (vhd.fp) {
        fclose(vhd.fp);
        vhd.fp = NULL;
    }
    vhd.path[0] = 0;
    vhd.dirty = false;
}

const char *coco_hd_path(void)
{
    return vhd.fp ? vhd.path : NULL;
}

bool coco_hd_write_protected(void)
{
    return vhd.fp && vhd.read_only;
}

/* A blank disk of `bytes`: its last sector written, the rest as the file
 * system gives it (formatted by OS-9's format)
 */
bool coco_hd_create(const char *path, uint32_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    static const uint8_t zero[256];
    bool ok = bytes >= 256 && fseek(f, (long)(bytes - 256), SEEK_SET) == 0 && fwrite(zero, 1, 256, f) == 256;
    return fclose(f) == 0 && ok;
}

bool vhd_dirty(void)
{
    return vhd.dirty;
}

void vhd_flush(void)
{
    if (vhd.fp && vhd.dirty) {
        fflush(vhd.fp);
        fsync(fileno(vhd.fp));
        vhd.dirty = false;
    }
}

void vhd_media_status(coco_media_status_t *s)
{
    s->hd_busy = vhd.fp && coco.frames < vhd.busy_frames;
    s->hd_reads = vhd.reads;
    s->hd_writes = vhd.writes;
    s->hd_sector = vhd.last;
}
