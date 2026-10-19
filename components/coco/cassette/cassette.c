/*
 * The cassette. A .CAS file is the bytes on the tape; each bit is one
 * cycle of a tone, 2400 Hz for a 1 and 1200 Hz for a 0, least significant
 * bit first (the leader is $55s).
 *
 * Playing: while the motor (PIA1 CA2) runs, the tape moves with real time
 * (ticks, whatever the CPU's rate) and PIA1 PA0 is the tone's square wave.
 * A .CAS has no silence, where a real tape has some before each leader: the
 * ROM waits half a second for the motor before it listens, which would take
 * most of a leader. So the tape stands still while the input isn't read
 * (for more than 3 ms), and moves on when the ROM reads it again.
 * Recording (CSAVE): the ROM writes a sine through the DAC; while the motor
 * runs, each cycle (from one upward crossing of the middle to the next) is
 * a bit by its length. Recording starts once the bits line up with a
 * leader byte, and writes at the tape's position.
 *
 * The tape is kept in memory; coco_media_flush() writes it back.
 */
#include <stdio.h>
#include <string.h>
#include "coco_internal.h"

#define TICK_HZ         3579545
#define ONE_TICKS       (TICK_HZ / 2400)
#define ZERO_TICKS      (TICK_HZ / 1200)
#define CAS_MAX         (1024 * 1024)
#define CHUNK           (16 * 1024)
#define IDLE_TICKS      (TICK_HZ * 3 / 1000)

static struct {
    char path[256];
    uint8_t *data;
    uint32_t size, cap, pos;
    int bit;                            /* 0-7 within data[pos] */
    bool read_only, dirty;
    bool motor;
    bool moving;                        /* the motor on and the input read since */
    uint64_t last_read;                 /* tick of the input's last read */
    uint64_t bit_start;                 /* tick the current bit began */
    /* recording */
    bool recording, aligned;
    int last_dac;
    uint64_t last_rise;
    uint8_t shift;                      /* the last 8 bits, the newest at bit 7 */
    int bits;
} cas;

void cas_reset(void)
{
    cas.motor = false;
    cas.recording = false;
}

/* After the last byte, two more $55s (the trailer that ends a block's
 * last bit, if the file lacks it), then nothing
 */
#define TAIL            2

static uint32_t bit_ticks(void)
{
    uint8_t b = cas.pos < cas.size ? cas.data[cas.pos] : 0x55;
    return (b >> cas.bit) & 1 ? ONE_TICKS : ZERO_TICKS;
}

/* Move the tape up to now */
static void play_to(uint64_t now)
{
    if (!cas.motor || !cas.moving || !cas.data || cas.recording) {
        return;
    }
    for (int guard = 0; guard < 4096 && cas.pos < cas.size + TAIL; guard++) {
        uint32_t len = bit_ticks();
        if (now < cas.bit_start + len) {
            return;
        }
        cas.bit_start += len;
        if (++cas.bit == 8) {
            cas.bit = 0;
            cas.pos++;
        }
    }
    if (cas.pos < cas.size + TAIL && now > cas.bit_start + 4096u * ZERO_TICKS) {
        cas.bit_start = now;            /* far behind: catch up */
    }
}

bool COCO_HOT cas_input(void)
{
    if (!cas.motor || !cas.data || cas.pos >= cas.size + TAIL) {
        return false;
    }
    uint64_t now = coco_now();
    if (!cas.moving) {
        cas.moving = true;
        cas.bit_start = now;
    } else if (now - cas.last_read > IDLE_TICKS) {
        /* not listened to meanwhile: the tape didn't move */
        play_to(cas.last_read);
        cas.bit_start += now - cas.last_read;
    }
    cas.last_read = now;
    play_to(now);
    if (cas.pos >= cas.size + TAIL) {
        return false;
    }
    uint32_t len = bit_ticks();
    return now - cas.bit_start < len / 2;
}

uint8_t cas_audio(void)
{
    if (!cas.motor || !cas.data || cas.recording) {
        return 32;
    }
    return cas_input() ? 48 : 16;
}

static void put_byte(uint8_t b)
{
    if (cas.pos >= cas.cap) {
        uint32_t cap = cas.cap + CHUNK;
        if (cap > CAS_MAX) {
            return;
        }
        uint8_t *n = coco_malloc(cap);
        if (!n) {
            return;
        }
        if (cas.data) {
            memcpy(n, cas.data, cas.size);
            free(cas.data);
        }
        cas.data = n;
        cas.cap = cap;
    }
    cas.data[cas.pos++] = b;
    if (cas.pos > cas.size) {
        cas.size = cas.pos;
    }
    cas.dirty = true;
}

void cas_motor(bool on)
{
    if (on == cas.motor) {
        return;
    }
    if (on) {
        play_to(coco_now());
    }
    cas.motor = on;
    if (on) {
        cas.bit_start = coco_now();
        cas.moving = false;
        cas.recording = false;
        cas.aligned = false;
        cas.last_dac = -1;
        cas.bits = 0;
    } else if (cas.recording) {
        /* The last bit's cycle has no rise after it: the block's trailer
         * ($55) ends with a 0
         */
        if (cas.aligned && cas.bits == 7) {
            put_byte((uint8_t)(cas.shift >> 1));
        }
        cas.recording = false;
        cas.bit = 0;
    } else if (cas.bit) {
        cas.bit = 0;
        cas.pos++;
    }
}

void COCO_HOT cas_dac_written(uint8_t dac)
{
    if (!cas.motor || !cas.path[0] || cas.read_only) {
        return;
    }
    int last = cas.last_dac;
    cas.last_dac = dac;
    if (last < 0 || !(last < 32 && dac >= 32)) {
        return;
    }
    uint64_t now = coco_now();
    uint64_t len = now - cas.last_rise;
    cas.last_rise = now;
    if (len > 2 * ZERO_TICKS || len < ONE_TICKS / 2) {
        return;                         /* the first rise, or not a tone */
    }
    if (!cas.recording) {
        cas.recording = true;           /* CSAVE: at the tape's position */
        if (cas.bit) {
            cas.bit = 0;
            cas.pos++;
        }
    }
    int bit = len < (ONE_TICKS + ZERO_TICKS) / 2 ? 1 : 0;
    cas.shift = (uint8_t)((cas.shift >> 1) | (bit << 7));
    if (!cas.aligned) {
        if (cas.shift == 0x55) {
            cas.aligned = true;
            put_byte(0x55);
            cas.bits = 0;
        }
        return;
    }
    if (++cas.bits == 8) {
        put_byte(cas.shift);
        cas.bits = 0;
    }
}

/* ---- The file ---- */

bool coco_cas_insert(const char *path)
{
    coco_cas_eject();
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
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > CAS_MAX) {
        fclose(f);
        return false;
    }
    uint32_t cap = ((uint32_t)size / CHUNK + 1) * CHUNK;
    cas.data = coco_malloc(cap);
    if (!cas.data) {
        fclose(f);
        return false;
    }
    bool ok = fread(cas.data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) {
        free(cas.data);
        cas.data = NULL;
        return false;
    }
    cas.size = (uint32_t)size;
    cas.cap = cap;
    cas.pos = 0;
    cas.bit = 0;
    cas.read_only = ro;
    cas.dirty = false;
    snprintf(cas.path, sizeof(cas.path), "%s", path);
    COCO_LOG("cassette %s, %ld bytes%s", path, size, ro ? ", read only" : "");
    return true;
}

static void flush(void)
{
    if (!cas.dirty || !cas.data || cas.read_only) {
        return;
    }
    FILE *f = fopen(cas.path, "wb");
    if (f) {
        fwrite(cas.data, 1, cas.size, f);
        fclose(f);
        cas.dirty = false;
    }
}

void coco_cas_eject(void)
{
    flush();
    free(cas.data);
    cas.data = NULL;
    cas.size = cas.cap = cas.pos = 0;
    cas.bit = 0;
    cas.path[0] = 0;
    cas.recording = false;
}

const char *coco_cas_path(void)
{
    return cas.path[0] ? cas.path : NULL;
}

bool coco_cas_write_protected(void)
{
    return cas.path[0] && cas.read_only;
}

void coco_cas_rewind(void)
{
    cas.pos = 0;
    cas.bit = 0;
    cas.bit_start = coco_now();
}

bool coco_cas_create(const char *path)
{
    FILE *f = fopen(path, "wb");
    return f && fclose(f) == 0;
}

bool cas_dirty(void)
{
    return cas.dirty;
}

void cas_flush(void)
{
    flush();
}

void cas_media_status(coco_media_status_t *s)
{
    s->cas_motor = cas.motor && cas.path[0];
    s->cas_recording = cas.recording;
    s->cas_position = cas.pos;
    s->cas_size = cas.size;
}

bool cas_playing(void)
{
    return cas.motor && cas.data && !cas.recording && cas.pos < cas.size + TAIL;
}
