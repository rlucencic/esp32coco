/*
 * Sound: the 6-bit DAC (PIA1 PA2-7) through the analogue mux when the
 * sound is enabled (PIA1 CB2; the mux on the DAC, the cassette or the
 * cartridge), plus the single-bit sound (PIA1 PB1). The level is averaged
 * over each half scan line (two samples a line, COCO_AUDIO_HZ), a DC
 * blocker takes out the DAC's resting level, and the samples wait in a ring
 * for the speaker's task.
 */
#include "coco_internal.h"

#define SAMPLE_TICKS    (LINE_TICKS / 2)
#define RING            8192

static struct {
    bool enabled, silent;
    int level;
    uint64_t last, sample_end;
    int32_t acc;                        /* level x ticks: fits (no 64-bit division) */
    int32_t dc_x, dc_y;                 /* DC blocker, x 256 */
    int16_t *ring;                      /* RING samples, in PSRAM (internal RAM is short) */
    volatile uint32_t head, tail;
} snd;

static int output_level(void)
{
    int level = 0;
    if (pia_c2(1, 1)) {
        int mux = (pia_c2(0, 0) ? 1 : 0) | (pia_c2(0, 1) ? 2 : 0);
        if (mux == 0) {
            level = (pia_output(1, 0) >> 2) - 32;
        } else if (mux == 1) {
            level = cas_audio() - 32;
        }
    }
    level *= 512;
    if ((pia[1].ddr[1] & 0x02) && (pia[1].out[1] & 0x02)) {
        level += 6000;
    }
    return level;
}

static void emit(int x)
{
    /* y = x - x' + 0.996 y' */
    int32_t y = (x - snd.dc_x) * 256 + (snd.dc_y * 255) / 256;
    snd.dc_x = x;
    snd.dc_y = y;
    if (!snd.enabled || snd.silent) {
        return;
    }
    int s = y / 256;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    uint32_t head = snd.head;
    if (snd.ring && head - __atomic_load_n(&snd.tail, __ATOMIC_ACQUIRE) < RING) {
        snd.ring[head % RING] = (int16_t)s;
        __atomic_store_n(&snd.head, head + 1, __ATOMIC_RELEASE);
    }
}

static void COCO_HOT advance(uint64_t now)
{
    while (now >= snd.sample_end) {
        snd.acc += snd.level * (int32_t)(snd.sample_end - snd.last);
        emit(snd.acc / SAMPLE_TICKS);
        snd.acc = 0;
        snd.last = snd.sample_end;
        snd.sample_end += SAMPLE_TICKS;
    }
    snd.acc += snd.level * (int32_t)(now - snd.last);
    snd.last = now;
}

void sound_reset(void)
{
    if (!snd.ring) {
        snd.ring = coco_calloc(RING, sizeof(int16_t));
    }
    snd.last = coco_now();
    snd.sample_end = snd.last + SAMPLE_TICKS;
    snd.acc = 0;
    snd.level = 0;
    snd.dc_x = snd.dc_y = 0;
}

void COCO_HOT sound_update(void)
{
    int level = output_level();
    if (level != snd.level) {
        advance(coco_now());
        snd.level = level;
    }
}

void COCO_HOT sound_line_end(void)
{
    advance(coco.line_tick + LINE_TICKS);
}

void coco_audio_enable(bool enable)
{
    snd.enabled = enable;
}

void sound_silent(bool silent)
{
    snd.silent = silent;
}

int coco_audio_render(int16_t *dst, int frames)
{
    uint32_t tail = snd.tail;
    uint32_t head = __atomic_load_n(&snd.head, __ATOMIC_ACQUIRE);
    int n = 0;
    while (n < frames && tail != head) {
        int16_t s = snd.ring[tail % RING];
        *dst++ = s;
        *dst++ = s;
        tail++;
        n++;
    }
    __atomic_store_n(&snd.tail, tail, __ATOMIC_RELEASE);
    return n;
}

int coco_audio_pending(void)
{
    return (int)(__atomic_load_n(&snd.head, __ATOMIC_ACQUIRE) - __atomic_load_n(&snd.tail, __ATOMIC_ACQUIRE));
}
