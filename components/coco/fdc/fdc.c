/*
 * The disk controller cartridge: DSKREG at $FF40 and a WD1793 at $FF48-
 * $FF4B, with two drives of .DSK images.
 *
 *   DSKREG  7 HALT enable, 6 drive 3 / side 1, 5 double density + NMI
 *           enable, 4 precompensation, 3 motor, 2-0 drives 2-0
 *
 * The 1793 finishes a command with INTRQ: that clears the HALT enable and,
 * with bit 5, is an NMI. With the HALT enable set the CPU halts whenever no
 * byte is waiting (DRQ), so Disk BASIC's byte loop runs at the drive's pace
 * and the NMI ends it. Bytes come every 32 us, as a double density drive's;
 * seeks and the search for a sector take a few milliseconds (faster than a
 * real drive).
 *
 * Commands: restore, seek, step (in/out), read and write sector (one or
 * more), read address, write track (formatting: the sectors' IDs and data
 * found in the stream), force interrupt; read track gives the sectors' data
 * with no gaps.
 *
 * .DSK (JVC): 256-byte sectors, 18 a track, tracks one after the other (and
 * the sides of a track), an optional header (the file's size modulo 256:
 * sectors a track, sides, sector size code, first sector number). Without
 * one, an OS-9 disk's LSN 0 says whether it has two sides; RS-DOS disks have
 * one. Kept in memory; written tracks go back to the file at
 * coco_media_flush().
 *
 * .DMK (read only, double density, 5.25"): a 16-byte header, then each
 * track (side 0, side 1, next track) as a table of 64 pointers to its IDs'
 * $FE bytes and the track's raw bytes. Read Sector takes the ID that
 * matches the track and sector registers (the ID's track, not the head's),
 * CRCs and the deleted-data mark as stored; Read Address the IDs in turn,
 * Read Track the raw bytes. No rotation: a sector is found after the same
 * short search wherever its ID is on the track.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "coco_internal.h"

#define BYTE_TICKS      115             /* 32 us */
#define SETTLE_TICKS    (3580 * 3)      /* 3 ms */
#define STEP_TICKS      (3580 * 2)      /* 2 ms a track (faster than the drives' 6-30) */
#define SEARCH_TICKS    (3580 * 2)
#define REV_TICKS       (3580 * 200)    /* one revolution, 300 rpm */
#define INDEX_TICKS     (3580 * 4)
#define TRACK_BYTES     6250            /* a double density track (write track) */
#define MAX_TRACKS      86
#define DMK_HEADER      16
#define DMK_ID_TABLE    128
#define DMK_TRACK_MAX   0x1900
#define DMK_DAM_WINDOW  43              /* MFM: bytes after the ID's CRC */
#define FDC_BUFFER_SIZE (DMK_TRACK_MAX - DMK_ID_TABLE)

bool coco_fdc_log;                      /* host builds: print the commands */

typedef struct {
    char path[256];
    uint8_t *data;
    uint32_t size;
    uint32_t header;                    /* bytes */
    int tracks, sides, spt, ssize, first;
    unsigned dmk_track_size;
    unsigned dmk_next_id;
    int dmk_address_track;
    bool read_only;
    uint8_t dirty[MAX_TRACKS * 2 / 8 + 1];
    bool any_dirty;
    int head;                           /* the head's track */
    uint32_t reads, writes;
} drive_t;

static drive_t s_drives[COCO_DRIVES];

typedef enum {
    ST_IDLE,
    ST_STEP,                            /* type I: stepping */
    ST_SEARCH,                          /* type II/III: finding the sector */
    ST_READ,                            /* bytes to the CPU */
    ST_WRITE,                           /* bytes from the CPU */
    ST_WRITE_TRACK,
    ST_DONE,                            /* INTRQ at the event */
} state_t;

static struct {
    uint8_t dskreg;
    uint8_t status, track, sector, data, command;
    bool drq, intrq;
    state_t state;
    uint64_t event;                     /* tick, NEVER */
    uint64_t halt_at;                   /* tick the HALT takes hold (after the CPU took a byte) */
    int taken;                          /* bytes the CPU took this command (the log) */
    uint16_t taken_pc;
    int target;                         /* type I: the track to reach */
    int direction;                      /* +1 in, -1 out */
    uint8_t *buf;                       /* a full raw or formatted track in PSRAM */
    int pos, len;                       /* in buf */
    uint32_t track_pos;                 /* write track: bytes so far */
    int light;
} fdc;

/* ---- Images ---- */

static drive_t *selected(int *side)
{
    uint8_t r = fdc.dskreg;
    int d = (r & 0x04) ? 2 : (r & 0x02) ? 1 : (r & 0x01) ? 0 : (r & 0x40) ? 3 : -1;
    if (side) {
        *side = ((r & 0x40) && d != 3) ? 1 : 0;
    }
    if (d < 0 || d >= COCO_DRIVES) {
        return NULL;
    }
    return &s_drives[d];
}

static int selected_index(void)
{
    drive_t *d = selected(NULL);
    return d ? (int)(d - s_drives) : -1;
}

/* The offset of a sector (number as on the disk), or -1 */
static long sector_offset(const drive_t *d, int track, int side, int sector)
{
    if (!d->data || d->dmk_track_size || track < 0 || track >= d->tracks || side >= d->sides) {
        return -1;
    }
    int s = sector - d->first;
    if (s < 0 || s >= d->spt) {
        return -1;
    }
    return (long)d->header + (((long)track * d->sides + side) * d->spt + s) * d->ssize;
}

static void mark_dirty(drive_t *d, int track, int side)
{
    int t = track * d->sides + side;
    if (t < MAX_TRACKS * 2) {
        d->dirty[t / 8] |= (uint8_t)(1u << (t & 7));
        d->any_dirty = true;
    }
}

static void geometry(drive_t *d)
{
    d->header = d->size % 256;
    d->spt = 18;
    d->sides = 1;
    d->ssize = 256;
    d->first = 1;
    const uint8_t *h = d->data;
    if (d->header >= 1 && h[0]) d->spt = h[0];
    if (d->header >= 2 && (h[1] == 1 || h[1] == 2)) d->sides = h[1];
    if (d->header >= 3 && h[2] <= 3) d->ssize = 128 << h[2];
    if (d->header >= 4) d->first = h[3];
    uint32_t body = d->size - d->header;
    if (d->header == 0 && body >= 256) {
        /* OS-9's LSN 0: total sectors (3 bytes), sectors a track, ... DD.FMT
         * at $10 (bit 0: two sides)
         */
        const uint8_t *l = d->data;
        uint32_t total = (uint32_t)l[0] << 16 | (uint32_t)l[1] << 8 | l[2];
        if (total * 256 == body && l[3] == 18 && (l[0x10] & 1)) {
            d->sides = 2;
        } else if (body > 80u * 18 * 256) {
            d->sides = 2;
        } else if (body == 40u * 2 * 18 * 256 && !(total * 256 == body && l[3] == 18)) {
            d->sides = 2;               /* 360 KB: the common 40-track double sided */
        }
    }
    d->tracks = (int)(body / ((uint32_t)d->spt * d->sides * d->ssize));
    if (d->tracks > MAX_TRACKS) {
        d->tracks = MAX_TRACKS;
    }
}

static unsigned le16(const uint8_t *data)
{
    return data[0] | (unsigned)data[1] << 8;
}

/* 5.25-inch, double-density DMK. The IDAM table locates the FE bytes.
 * Check every offset before the controller reads a table or raw track.
 */
static bool dmk_geometry(drive_t *d)
{
    const uint8_t *image = d->data;
    unsigned track_size = le16(image + 2);
    unsigned tracks = image[1];
    unsigned sides = image[4] & 0x10 ? 1 : 2;
    if (!tracks || tracks > MAX_TRACKS || track_size < DMK_ID_TABLE || track_size > DMK_TRACK_MAX ||
        (image[4] & ~0x10) || image[12] || image[13] || image[14] || image[15] ||
        d->size != DMK_HEADER + tracks * sides * track_size) {
        return false;
    }
    for (unsigned n = 0; n < tracks * sides; n++) {
        const uint8_t *track = image + DMK_HEADER + n * track_size;
        unsigned end = DMK_ID_TABLE;
        for (unsigned i = 0; i < DMK_ID_TABLE; i += 2) {
            unsigned entry = le16(track + i);
            if (!entry) {
                end = track_size;
                continue;
            }
            unsigned offset = entry & 0x3FFF;
            if (!(entry & 0x8000) || offset < end || offset + 7 > track_size || track[offset] != 0xFE) {
                return false;
            }
            end = offset + 7;
        }
    }
    d->tracks = (int)tracks;
    d->sides = (int)sides;
    d->dmk_track_size = track_size;
    d->dmk_address_track = -1;
    d->dmk_next_id = 0;
    d->read_only = true;
    return true;
}

bool coco_disk_insert(int drive, const char *path)
{
    if (drive < 0 || drive >= COCO_DRIVES) {
        return false;
    }
    drive_t *d = &s_drives[drive];
    coco_disk_eject(drive);
    const char *ext = strrchr(path, '.');
    bool dmk = ext && strcasecmp(ext, ".dmk") == 0;
    bool ro = dmk;
    FILE *f = fopen(path, dmk ? "rb" : "r+b");
    if (!f && !dmk) {
        f = fopen(path, "rb");
        ro = true;
    }
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 256 || size > 2 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    d->data = coco_malloc((size_t)size);
    if (!d->data) {
        fclose(f);
        return false;
    }
    bool ok = fread(d->data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(d->data);
        d->data = NULL;
        return false;
    }
    d->size = (uint32_t)size;
    d->read_only = ro;
    if (dmk) {
        if (!dmk_geometry(d)) {
            coco_disk_eject(drive);
            return false;
        }
    } else {
        geometry(d);
    }
    snprintf(d->path, sizeof(d->path), "%s", path);
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
    COCO_LOG("drive %d: %s, %d tracks, %d side%s%s", drive, path, d->tracks, d->sides, d->sides > 1 ? "s" : "",
             ro ? ", read only" : "");
    space_update(drive);
    return true;
}

static void flush_drive(drive_t *d)
{
    if (!d->data || !d->any_dirty || d->read_only) {
        return;
    }
    FILE *f = fopen(d->path, "r+b");
    if (!f) {
        return;
    }
    uint32_t track_bytes = (uint32_t)d->spt * d->ssize;
    for (int t = 0; t < d->tracks * d->sides; t++) {
        if (d->dirty[t / 8] & (1u << (t & 7))) {
            long off = (long)d->header + (long)t * track_bytes;
            fseek(f, off, SEEK_SET);
            fwrite(d->data + off, 1, track_bytes, f);
        }
    }
    fclose(f);
    memset(d->dirty, 0, sizeof(d->dirty));
    d->any_dirty = false;
}

void coco_disk_eject(int drive)
{
    if (drive < 0 || drive >= COCO_DRIVES) {
        return;
    }
    drive_t *d = &s_drives[drive];
    flush_drive(d);
    free(d->data);
    d->data = NULL;
    d->dmk_track_size = 0;
    d->path[0] = 0;
    space_update(drive);
}

const char *coco_disk_path(int drive)
{
    return drive >= 0 && drive < COCO_DRIVES && s_drives[drive].data ? s_drives[drive].path : NULL;
}

bool coco_disk_write_protected(int drive)
{
    return drive >= 0 && drive < COCO_DRIVES && s_drives[drive].read_only;
}

bool coco_disk_create(const char *path, uint32_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = true;
    /* double sided: a header to say so */
    if (bytes == COCO_DISK_40DS_BYTES || bytes == COCO_DISK_80DS_BYTES) {
        static const uint8_t header[4] = { 18, 2, 1, 1 };
        ok = fwrite(header, 1, 4, f) == 4;
    }
    uint8_t ff[256];
    memset(ff, 0xFF, sizeof(ff));
    for (uint32_t done = 0; ok && done < bytes; done += sizeof(ff)) {
        uint32_t n = bytes - done < sizeof(ff) ? bytes - done : (uint32_t)sizeof(ff);
        ok = fwrite(ff, 1, n, f) == n;
    }
    return fclose(f) == 0 && ok;
}

bool coco_disks_dirty(void)
{
    for (int i = 0; i < COCO_DRIVES; i++) {
        if (s_drives[i].any_dirty) {
            return true;
        }
    }
    return false;
}

void coco_disks_flush(void)
{
    for (int i = 0; i < COCO_DRIVES; i++) {
        bool written = s_drives[i].any_dirty;
        flush_drive(&s_drives[i]);
        if (written) {
            space_update(i);
        }
    }
}

/* The disk's 256-byte sectors in a file system's order: OS-9's LSNs (a
 * track's sides one after the other, as the image has them) or, one_side,
 * side 0's tracks (Disk BASIC)
 */
uint64_t fdc_disk_bytes(int drive, bool one_side)
{
    const drive_t *d = drive >= 0 && drive < COCO_DRIVES ? &s_drives[drive] : NULL;
    if (d == NULL || d->data == NULL || d->dmk_track_size || d->ssize != 256) {
        return 0;
    }
    return (uint64_t)d->tracks * (one_side ? 1 : d->sides) * d->spt * 256;
}

bool fdc_disk_read(int drive, bool one_side, uint64_t offset, void *buf, uint32_t len)
{
    uint8_t *out = buf;
    if (offset + len > fdc_disk_bytes(drive, one_side)) {
        return false;
    }
    const drive_t *d = &s_drives[drive];
    while (len > 0) {
        uint32_t lsn = (uint32_t)(offset / 256), at = (uint32_t)(offset % 256);
        uint32_t per_track = (uint32_t)d->spt * (one_side ? 1 : d->sides);
        int track = (int)(lsn / per_track), side = (int)(lsn % per_track) / d->spt, s = (int)(lsn % d->spt);
        long off = sector_offset(d, track, side, d->first + s);
        if (off < 0) {
            return false;
        }
        uint32_t n = 256 - at < len ? 256 - at : len;
        memcpy(out, d->data + off + at, n);
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

/* ---- The lines ---- */

static void update_lines(void)
{
    if (fdc.intrq) {
        fdc.dskreg &= 0x7F;             /* INTRQ ends the HALT enable */
    }
    coco.cpu.halt = !fdc.drq && (fdc.dskreg & 0x80);
}

static void set_intrq(bool on)
{
    bool was = fdc.intrq && (fdc.dskreg & 0x20);
    fdc.intrq = on;
    if (on && !was && (fdc.dskreg & 0x20)) {
        m6809_nmi(&coco.cpu);
    }
    update_lines();
}

static void set_drq(bool on)
{
    fdc.drq = on;
    if (on) {
        fdc.status |= 0x02;
        fdc.halt_at = NEVER;
    } else {
        fdc.status &= ~0x02;
    }
    update_lines();
}

/* The CPU read or wrote the data register: DRQ drops during that
 * instruction, too late for the HALT to stop the next one (Disk BASIC's
 * loops store the byte they read before they halt; at a sector's end the
 * NMI comes after that store). The HALT takes hold a moment later.
 */
static void cpu_took_byte(void)
{
    fdc.taken++;
    fdc.taken_pc = coco.cpu.ppc;
    fdc.drq = false;
    fdc.status &= ~0x02;
    if (fdc.dskreg & 0x80) {
        /* now is the start of the instruction that took it (cycles count
         * when one ends): six cycles on is after the next one began
         */
        fdc.halt_at = coco_now() + (6u << coco.tick_shift);
        coco_event_changed();
    }
}

static void schedule(uint64_t delay)
{
    fdc.event = coco_now() + delay;
    coco_event_changed();
}

static void finish(uint8_t extra_status)
{
    if (coco_fdc_log) {
        COCO_LOG("fdc: done, status %02X, the CPU took %d bytes (last at $%04X)", (fdc.status & ~0x03) | extra_status,
                 fdc.taken, fdc.taken_pc);
    }
    fdc.status = (uint8_t)((fdc.status & ~0x03) | extra_status);
    fdc.state = ST_IDLE;
    fdc.event = NEVER;
    set_drq(false);
    set_intrq(true);
    fdc.light = -1;
}

/* ---- Commands ---- */

static bool ready(void)
{
    drive_t *d = selected(NULL);
    return d && d->data;
}

static uint8_t type1_status(void)
{
    drive_t *d = selected(NULL);
    uint8_t s = 0;
    if (!ready()) {
        s |= 0x80;
    }
    if (d && d->read_only) {
        s |= 0x40;
    }
    s |= 0x20;                          /* head loaded */
    if (d && d->head == 0) {
        s |= 0x04;
    }
    if (ready() && (fdc.dskreg & 0x08) && (coco_now() % REV_TICKS) < INDEX_TICKS) {
        s |= 0x02;                      /* index pulse */
    }
    return s;
}

static void start_type1(uint8_t cmd)
{
    drive_t *d = selected(NULL);
    switch (cmd >> 4) {
    case 0x0:                           /* restore */
        fdc.track = 0xFF;
        fdc.target = 0;
        fdc.direction = -1;
        break;
    case 0x1:                           /* seek */
        fdc.target = fdc.data;
        fdc.direction = fdc.data > fdc.track ? 1 : -1;
        break;
    case 0x2: case 0x3:                 /* step, the last direction */
        fdc.target = -1;
        break;
    case 0x4: case 0x5:                 /* step in */
        fdc.direction = 1;
        fdc.target = -1;
        break;
    default:                            /* step out */
        fdc.direction = -1;
        fdc.target = -1;
        break;
    }
    int steps;
    if (fdc.target >= 0) {
        if ((cmd >> 4) == 0) {
            steps = d ? d->head : 0;
            fdc.track = 0;
            if (d) {
                d->head = 0;
            }
        } else {
            steps = fdc.target - fdc.track;
            if (steps < 0) {
                steps = -steps;
            }
            if (d) {
                d->head += fdc.target - fdc.track;
                if (d->head < 0) d->head = 0;
                if (d->head > 85) d->head = 85;
            }
            fdc.track = (uint8_t)fdc.target;
        }
    } else {
        steps = 1;
        if (cmd & 0x10) {
            fdc.track = (uint8_t)(fdc.track + fdc.direction);
        }
        if (d) {
            d->head += fdc.direction;
            if (d->head < 0) d->head = 0;
            if (d->head > 85) d->head = 85;
        }
    }
    fdc.status = 0x01;
    fdc.state = ST_STEP;
    schedule((uint64_t)steps * STEP_TICKS + ((cmd & 0x04) ? SETTLE_TICKS : 0) + 100);
}

static void start_type2(uint8_t cmd)
{
    drive_t *d = selected(NULL);
    fdc.status = 0x01;
    if (!ready()) {
        finish(0x80);
        return;
    }
    if (d->read_only && ((cmd & 0xE0) == 0xA0 || (cmd & 0xF0) == 0xF0)) {
        finish(0x40);
        return;
    }
    fdc.state = ST_SEARCH;
    fdc.light = selected_index();
    schedule(SEARCH_TICKS + ((cmd & 0x04) ? SETTLE_TICKS : 0));
}

/* MFM CRC-CCITT, primed with the three A1 sync marks. A valid stream
 * including its two stored CRC bytes has a zero remainder.
 */
static bool dmk_crc_ok(const uint8_t *bytes, unsigned len)
{
    uint16_t crc = 0xCDB4;
    while (len--) {
        crc ^= (uint16_t)*bytes++ << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    return crc == 0;
}

static void start_read(const uint8_t *bytes, int len, uint8_t status)
{
    memcpy(fdc.buf, bytes, (size_t)len);
    fdc.pos = 0;
    fdc.len = len;
    fdc.status = (uint8_t)((fdc.status & ~0x20) | status);
    fdc.state = ST_READ;
    schedule(BYTE_TICKS);
}

static void dmk_read(drive_t *d, int side, uint8_t cmd)
{
    if (d->head >= d->tracks || side >= d->sides) {
        finish(0x10);
        return;
    }
    int physical_track = d->head * d->sides + side;
    unsigned track_size = d->dmk_track_size;
    const uint8_t *track = d->data + DMK_HEADER + physical_track * track_size;
    if ((cmd & 0xF0) == 0xE0) {
        start_read(track + DMK_ID_TABLE, (int)track_size - DMK_ID_TABLE, 0);
        return;
    }
    if ((cmd & 0xF0) == 0xC0) {
        if (d->dmk_address_track != physical_track) {
            d->dmk_address_track = physical_track;
            d->dmk_next_id = 0;
        }
        unsigned entry = le16(track + d->dmk_next_id);
        if (!entry) {
            d->dmk_next_id = 0;
            entry = le16(track);
        }
        if (entry) {
            const uint8_t *id = track + (entry & 0x3FFF);
            d->dmk_next_id = (d->dmk_next_id + 2) % DMK_ID_TABLE;
            fdc.sector = id[1];
            start_read(id + 1, 6, dmk_crc_ok(id, 7) ? 0 : 0x08);
            return;
        }
        finish(0x10);
        return;
    }
    uint8_t error = 0x10;
    for (unsigned i = 0; i < DMK_ID_TABLE && le16(track + i); i += 2) {
        unsigned offset = le16(track + i) & 0x3FFF;
        const uint8_t *id = track + offset;
        if (id[1] != fdc.track || id[3] != fdc.sector ||
            ((cmd & 0x02) && id[2] != ((cmd >> 3) & 1))) {
            continue;
        }
        if (!dmk_crc_ok(id, 7)) {
            error |= 0x08;
            continue;
        }
        /* The 1793 knows sizes 0-3 and uses the low two bits (as MAME) */
        unsigned len = 128u << (id[4] & 3);
        /* The data mark (after three A1s) must come within 43 bytes of
         * the ID's last CRC byte, else the 1793 looks for the next ID. The
         * field is then read whatever follows: it may run over the next
         * sector's ID (overlapping sectors), but not past the track's end.
         */
        unsigned window = offset + 6 + DMK_DAM_WINDOW;
        for (unsigned m = offset + 10; m <= window && m + 1 + len + 2 <= track_size; m++) {
            uint8_t mark = track[m];
            if ((mark != 0xFB && mark != 0xF8) || track[m - 1] != 0xA1 ||
                track[m - 2] != 0xA1 || track[m - 3] != 0xA1) {
                continue;
            }
            uint8_t status = mark == 0xF8 ? 0x20 : 0;
            if (!dmk_crc_ok(track + m, 1 + len + 2)) {
                status |= 0x08;
            }
            d->reads++;
            start_read(track + m + 1, (int)len, status);
            return;
        }
    }
    finish(error);
}

static void begin_sector(void)
{
    int side;
    drive_t *d = selected(&side);
    uint8_t cmd = fdc.command;
    if (!d || !d->data) {
        finish(0x80);
        return;
    }
    if (d->dmk_track_size) {
        if ((cmd & 0xE0) == 0xA0 || (cmd & 0xF0) == 0xF0) {
            finish(0x40);
        } else {
            dmk_read(d, side, cmd);
        }
        return;
    }
    if ((cmd & 0xF0) == 0xC0) {
        /* read address: the next sector's ID */
        static int next;
        next = (next + 1) % (d->spt ? d->spt : 18);
        fdc.buf[0] = (uint8_t)d->head;
        fdc.buf[1] = (uint8_t)side;
        fdc.buf[2] = (uint8_t)(d->first + next);
        fdc.buf[3] = d->ssize == 128 ? 0 : d->ssize == 256 ? 1 : d->ssize == 512 ? 2 : 3;
        fdc.buf[4] = fdc.buf[5] = 0;
        fdc.sector = (uint8_t)d->head;  /* the 1793 puts the track in the sector register */
        fdc.pos = 0;
        fdc.len = 6;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
        return;
    }
    if ((cmd & 0xF0) == 0xE0) {
        /* read track: the sectors' data */
        int n = 0;
        for (int s = 0; s < d->spt && n + d->ssize <= TRACK_BYTES; s++) {
            long off = sector_offset(d, d->head, side, d->first + s);
            if (off >= 0) {
                memcpy(fdc.buf + n, d->data + off, (size_t)d->ssize);
                n += d->ssize;
            }
        }
        fdc.pos = 0;
        fdc.len = n;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
        return;
    }
    if ((cmd & 0xF0) == 0xF0) {
        fdc.track_pos = 0;
        fdc.pos = 0;
        fdc.state = ST_WRITE_TRACK;
        set_drq(true);
        schedule(BYTE_TICKS);
        return;
    }
    /* the track register must match the head's track (JVC: IDs as the
     * physical tracks); side compare (bit 1) against bit 3
     */
    bool side_ok = !(cmd & 0x02) || (((cmd >> 3) & 1) == side);
    long off = (fdc.track == d->head && side_ok) ? sector_offset(d, d->head, side, fdc.sector) : -1;
    if (off < 0) {
        finish(0x10);                   /* record not found */
        return;
    }
    fdc.pos = 0;
    fdc.len = d->ssize;
    if ((cmd & 0xE0) == 0x80) {
        memcpy(fdc.buf, d->data + off, (size_t)d->ssize);
        d->reads++;
        fdc.state = ST_READ;
        schedule(BYTE_TICKS);
    } else {
        fdc.state = ST_WRITE;
        set_drq(true);
        schedule(BYTE_TICKS * 2);
    }
}

/* Write track: IDs (after $FE: track, side, sector, size) and the data
 * after each $FB go into the image
 */
static void write_track_end(void)
{
    int side;
    drive_t *d = selected(&side);
    const uint8_t *b = fdc.buf;
    int id_sector = -1;
    for (int i = 0; i < fdc.pos; i++) {
        if (b[i] == 0xFE && i + 4 < fdc.pos && i > 0 && (b[i - 1] == 0xF5 || b[i - 1] == 0x00)) {
            id_sector = b[i + 3];
            i += 4;
        } else if (b[i] == 0xFB && id_sector >= 0 && i + d->ssize < fdc.pos) {
            long off = sector_offset(d, d->head, side, id_sector);
            if (off >= 0) {
                memcpy(d->data + off, b + i + 1, (size_t)d->ssize);
                mark_dirty(d, d->head, side);
            }
            i += d->ssize;
            id_sector = -1;
        }
    }
    d->writes++;
}

static void state_event(void);

void fdc_event(void)
{
    uint64_t now = coco_now();
    if (fdc.halt_at <= now) {
        fdc.halt_at = NEVER;
        update_lines();
    }
    if (fdc.event <= now) {
        state_event();
    }
}

static void state_event(void)
{
    fdc.event = NEVER;
    drive_t *d = selected(NULL);
    switch (fdc.state) {
    case ST_STEP: {
        uint8_t s = type1_status();
        if ((fdc.command & 0x04) && !ready()) {
            s |= 0x10;                  /* verify: seek error */
        }
        fdc.state = ST_IDLE;
        fdc.status = s & ~0x01;
        set_intrq(true);
        break;
    }
    case ST_SEARCH:
        begin_sector();
        break;
    case ST_READ:
        if (fdc.drq) {
            fdc.status |= 0x04;         /* not read in time: lost data */
        }
        if (fdc.pos < fdc.len) {
            fdc.data = fdc.buf[fdc.pos++];
            set_drq(true);
            schedule(BYTE_TICKS);
            break;
        }
        if ((fdc.command & 0xF0) == 0x90 && !(fdc.status & 0x08)) {
            /* multiple: on to the next sector */
            fdc.sector++;
            begin_sector();
            break;
        }
        finish(fdc.status & 0x04);
        break;
    case ST_WRITE:
        if (fdc.drq) {
            fdc.status |= 0x04;         /* no byte from the CPU: lost data, a zero */
            fdc.data = 0;
        }
        fdc.buf[fdc.pos++] = fdc.data;
        if (fdc.pos < fdc.len) {
            set_drq(true);
            schedule(BYTE_TICKS);
            break;
        }
        {
            int side;
            d = selected(&side);
            long off = sector_offset(d, d->head, side, fdc.sector);
            if (off >= 0) {
                memcpy(d->data + off, fdc.buf, (size_t)d->ssize);
                mark_dirty(d, d->head, side);
                d->writes++;
            }
        }
        if ((fdc.command & 0xF0) == 0xB0) {
            fdc.sector++;
            begin_sector();
            break;
        }
        finish(fdc.status & 0x04);
        break;
    case ST_WRITE_TRACK:
        if (fdc.drq) {
            fdc.status |= 0x04;
            fdc.data = 0;
        }
        if (fdc.pos < TRACK_BYTES) {
            fdc.buf[fdc.pos++] = fdc.data;
        }
        if (++fdc.track_pos >= TRACK_BYTES) {
            write_track_end();
            finish(0);
            break;
        }
        set_drq(true);
        schedule(BYTE_TICKS);
        break;
    default:
        break;
    }
}

static void command(uint8_t cmd)
{
    if (coco_fdc_log) {
        COCO_LOG("fdc: frame %u line %d command %02X track %02X sector %02X data %02X dskreg %02X head %d",
                 (unsigned)coco.frames, coco.line, cmd, fdc.track, fdc.sector, fdc.data, fdc.dskreg,
                 selected(NULL) ? selected(NULL)->head : -1);
    }
    if ((cmd & 0xF0) == 0xD0) {
        /* force interrupt: stop; with bit 3 an interrupt now */
        bool was_busy = fdc.state != ST_IDLE;
        fdc.state = ST_IDLE;
        fdc.event = NEVER;
        set_drq(false);
        fdc.status = was_busy ? (uint8_t)(fdc.status & ~0x01) : type1_status();
        fdc.light = -1;
        if (cmd & 0x08) {
            set_intrq(true);
        }
        return;
    }
    if (fdc.state != ST_IDLE) {
        return;                         /* busy: ignored */
    }
    fdc.command = cmd;
    fdc.taken = 0;
    set_intrq(false);
    set_drq(false);
    if (!(cmd & 0x80)) {
        start_type1(cmd);
    } else {
        start_type2(cmd);
    }
}

void fdc_reset(void)
{
    if (!fdc.buf) {
        fdc.buf = coco_calloc(1, FDC_BUFFER_SIZE);
    }
    fdc.dskreg = 0;
    fdc.status = 0;
    fdc.track = fdc.sector = fdc.data = 0;
    fdc.drq = fdc.intrq = false;
    fdc.state = ST_IDLE;
    fdc.event = NEVER;
    fdc.halt_at = NEVER;
    fdc.light = -1;
    coco.cpu.halt = false;
}

uint8_t fdc_read(uint16_t a)
{
    switch (a & 0x0F) {
    case 0x8: {
        uint8_t s = (fdc.state == ST_IDLE && !(fdc.command & 0x80)) ? type1_status() | (fdc.status & 0x18)
                                                                    : fdc.status;
        if (fdc.state == ST_IDLE && (fdc.command & 0x80) && !ready()) {
            s |= 0x80;
        }
        if (fdc.intrq) {
            set_intrq(false);
        }
        return s;
    }
    case 0x9: return fdc.track;
    case 0xA: return fdc.sector;
    case 0xB:
        if (fdc.drq) {
            cpu_took_byte();
        }
        return fdc.data;
    default:
        return 0;
    }
}

void fdc_write(uint16_t a, uint8_t v)
{
    switch (a & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
    case 0x4: case 0x5: case 0x6: case 0x7:
    {
        bool nmi_was = fdc.intrq && (fdc.dskreg & 0x20);
        fdc.dskreg = v;
        if (fdc.intrq && (v & 0x20) && !nmi_was) {
            m6809_nmi(&coco.cpu);       /* NMI enabled while INTRQ is up */
        }
        update_lines();
        break;
    }
    case 0x8: command(v); break;
    case 0x9: fdc.track = v; break;
    case 0xA: fdc.sector = v; break;
    case 0xB:
        fdc.data = v;
        if (fdc.drq) {
            cpu_took_byte();
        }
        break;
    default:
        break;
    }
}

uint64_t fdc_next_event(void)
{
    return fdc.event < fdc.halt_at ? fdc.event : fdc.halt_at;
}

/* ---- Status ---- */

void fdc_media_status(coco_media_status_t *s)
{
    s->light = fdc.light;
    s->motor = (fdc.dskreg & 0x08) != 0;
    for (int i = 0; i < COCO_DRIVES; i++) {
        s->reads[i] = s_drives[i].reads;
        s->writes[i] = s_drives[i].writes;
        s->track[i] = (uint8_t)s_drives[i].head;
    }
}
