/*
 * Between the Color Computer core's modules.
 *
 * Time: the 6809's cycle counter (cpu.cycles) and ticks of the 3.58 MHz
 * colour carrier (279 ns), which don't depend on the CPU's rate. A scan line
 * is 228 ticks: 57 CPU cycles at 0.89 MHz (4 ticks each), 114 at 1.79 MHz.
 * coco.tick_base/cycle_base relate the two since the rate last changed.
 *
 * Each scan line: the line's syncs and interrupts at its start, the CPU up
 * to its end (stopping early for timed events: the disk controller, the
 * GIME's timer), then the VDG or GIME draws it with the registers as they
 * are at its end, and the sound gets its two samples.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "coco.h"
#include "m6809.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#define COCO_LOG(fmt, ...)  ESP_LOGI("coco", fmt, ##__VA_ARGS__)
/* The core's buffers in PSRAM: plain malloc() serves small blocks from
 * internal RAM, which WiFi, Bluetooth and the LCD need
 */
#define coco_malloc(n)      heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define coco_calloc(n, s)   heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM)
#else
#include <stdio.h>
#include <stdlib.h>
#define COCO_LOG(fmt, ...)  printf("coco: " fmt "\n", ##__VA_ARGS__)
#define coco_malloc(n)      malloc(n)
#define coco_calloc(n, s)   calloc((n), (s))
#endif

#define COCO_HOT            M6809_HOT

#define LINES               262
#define LINE_TICKS          228
#define NEVER               UINT64_MAX

/* The VDG's frame (and the GIME's in its 192-line modes): lines from the
 * first top border line. The picture shows lines 1-240.
 */
#define VDG_TOP_BORDER      25
#define VDG_ACTIVE          192
#define FIRST_SHOWN_LINE    1

/* IRQ and FIRQ sources (bits of cpu.irq, cpu.firq) */
enum { INT_PIA = 0x01, INT_GIME = 0x02 };

/* ---- coco.c: the machine ---- */

typedef struct {
    coco_config_t cfg;
    bool coco3;
    m6809_t cpu;
    uint32_t ram_size;
    int line;                           /* 0-261 */
    uint32_t frames;
    uint64_t line_tick;                 /* ticks since power-on when the line began */
    uint64_t tick_base, cycle_base;     /* a tick and the cycle it was (since the rate changed) */
    int tick_per_cycle;                 /* 4 at 0.89 MHz, 2 at 1.79 MHz */
    int tick_shift;                     /* its log2 */
    uint64_t line_end;                  /* the cycle the line ends */
    bool frame_changed;                 /* a line came out different this frame */
    uint32_t signature;
    uint16_t palette[COCO_COLOURS];     /* RGB565 */
    uint8_t ff_page[256];               /* reads of nothing ($FF) */
} coco_machine_t;

extern coco_machine_t coco;

static inline uint64_t coco_now(void)
{
    return coco.tick_base + ((coco.cpu.cycles - coco.cycle_base) << coco.tick_shift);
}

/* The first cycle at or after tick t (shifts: 64-bit division is a slow
 * library call on the boards)
 */
static inline uint64_t coco_cycle_at(uint64_t t)
{
    if (t <= coco.tick_base) {
        return coco.cycle_base;
    }
    return coco.cycle_base + ((t - coco.tick_base + coco.tick_per_cycle - 1) >> coco.tick_shift);
}

/* A run of n pixels of one colour (word stores for the even widths: out
 * aligned to them)
 */
static inline uint8_t *coco_fill(uint8_t *out, uint8_t c, int n)
{
    uint32_t w = c * 0x01010101u;
    switch (n) {
    case 1: out[0] = c; break;
    case 2: *(uint16_t *)out = (uint16_t)w; break;
    case 4: *(uint32_t *)out = w; break;
    case 8: ((uint32_t *)out)[0] = ((uint32_t *)out)[1] = w; break;
    case 16: ((uint32_t *)out)[0] = ((uint32_t *)out)[1] = ((uint32_t *)out)[2] = ((uint32_t *)out)[3] = w; break;
    default:
        for (int i = 0; i < n; i++) {
            out[i] = c;
        }
        break;
    }
    return out + n;
}

/* The CPU's rate from the SAM (both machines): R1 or R0 set is 1.79 MHz */
void coco_rate_changed(void);
/* Interrupt lines from the PIAs and the GIME */
void coco_update_interrupts(void);
/* A timed event moved earlier: stop the CPU's run to look at it */
void coco_event_changed(void);
/* Line rendered: did it change? */
void coco_line_rendered(int frame_row, bool changed);

/* ---- cpu/cpu.c: the CPU's bus ---- */

void cpu_init(void);
uint8_t cpu_floating_bus(void);
uint8_t io_read(uint16_t address);
void io_write(uint16_t address, uint8_t value);

/* ---- memory/memory.c: the memory maps ---- */

void memory_map(void);                  /* recompute the CPU's pages */
uint8_t memory_peek(uint16_t address);

/* ---- sam/sam.c: the MC6883 (CoCo 2) and the GIME's copy of its bits ---- */

enum {
    SAM_V0 = 0x0001, SAM_V1 = 0x0002, SAM_V2 = 0x0004,
    SAM_F0 = 0x0008,                    /* F0-F6: bits 3-9 */
    SAM_P1 = 0x0400, SAM_R0 = 0x0800, SAM_R1 = 0x1000,
    SAM_M0 = 0x2000, SAM_M1 = 0x4000, SAM_TY = 0x8000,
};

typedef struct {
    uint16_t state;
    uint16_t counter;                   /* the video address counter */
    uint8_t xdiv, ydiv;
} sam_t;

extern sam_t sam;

void sam_reset(void);
void sam_write(int offset);             /* $FFC0-$FFDF: offset 0-31 */
static inline uint16_t sam_display_offset(void)
{
    return (uint16_t)(((sam.state >> 3) & 0x7F) << 9);
}
static inline int sam_vmode(void)
{
    return sam.state & 7;
}
/* The video counter: reset (field sync), one fetch (the VDG's address
 * bit 0 for it), the horizontal sync's clearing
 */
void sam_counter_reset(void);
uint8_t sam_fetch(bool da0);
void sam_hsync(void);

/* ---- pia/pia.c: two MC6821 ---- */

typedef struct {
    uint8_t ddr[2], out[2], ctl[2];     /* side 0 = A, 1 = B */
    bool c1[2];                         /* CA1, CB1 levels */
    bool c2_in[2];                      /* CA2, CB2 input levels */
} pia_t;

extern pia_t pia[2];

void pia_reset(void);
uint8_t pia_read(int p, int reg);
void pia_write(int p, int reg, uint8_t value);
uint8_t pia_peek(int p, int reg);
void pia_set_c1(int p, int side, bool level);
bool pia_c2(int p, int side);           /* CA2/CB2 as an output (high when an input) */
bool pia_irq(int p);
/* The port's pins: outputs as written, inputs as pulled up */
static inline uint8_t pia_output(int p, int side)
{
    return (uint8_t)((pia[p].out[side] & pia[p].ddr[side]) | ~pia[p].ddr[side]);
}
/* The machine's side: the input pins, and outputs that changed */
uint8_t coco_pia_input(int p, int side);
void coco_pia_changed(int p);

/* ---- vdg/vdg.c: the MC6847 (CoCo 2) ---- */

void vdg_init(void);
void vdg_render_line(int line);         /* at the line's end */
/* Bytes a line in this mode (PIA1 PB), and the line's 512 pixels from them
 * through map (VDG colour -> colour number); t1: the GIME's version
 */
int vdg_bytes(uint8_t pb);
void vdg_emit(uint8_t pb, const uint8_t *data, int n, int row_in_char, const uint8_t *map, bool t1,
              coco_artifact_t artifacts, uint8_t *out);
/* A finished line of the picture (row 0-239) */
void vdg_commit(int row, const uint8_t *line);
void vdg_forget_lines(void);           /* the picture changed under them (colours) */
extern const uint8_t vdg_font[64][7];   /* 5 x 7, bit 4 the left pixel */
extern const uint8_t vdg_font_lower[32][7];

/* ---- gime/gime.c: the CoCo 3's GIME ---- */

enum {
    GIME_TMR = 0x20, GIME_HBORD = 0x10, GIME_VBORD = 0x08,
    GIME_EI2 = 0x04, GIME_EI1 = 0x02, GIME_EI0 = 0x01,
};

typedef struct {
    uint8_t reg[16];                    /* $FF90-$FF9F */
    uint8_t mmu[16];                    /* $FFA0-$FFAF, 6 bits */
    uint8_t palette[16];                /* $FFB0-$FFBF, 6 bits */
    uint8_t irq, firq;                  /* pending (read at $FF92/$FF93) */
    uint8_t lines;                      /* interrupt inputs' levels (for edges) */
    uint64_t timer_at;                  /* tick the timer next reaches zero, NEVER */
    bool blink;
    /* the frame's video, latched at its start */
    bool legacy;
    uint32_t video_address;
    int line_in_row;
    int top_border, body;               /* lines */
    uint8_t ff22;                       /* the VDG mode lines as PIA1 PB had them */
} gime_t;

extern gime_t gime;

void gime_reset(void);
uint8_t gime_read(uint16_t address);    /* $FF90-$FFDF */
void gime_write(uint16_t address, uint8_t value);
void gime_set_line(uint8_t interrupt, bool level);  /* EI0-EI2, HBORD, VBORD */
void gime_timer_event(void);
void gime_frame_start(void);
void gime_render_line(int line);
bool gime_video_body(int line);         /* the line is in the picture's body */
extern const uint8_t gime_font[128][8]; /* 8 x 8, bit 0 the left pixel */

/* ---- keyboard/keyboard.c ---- */

void keyboard_reset(void);
/* The rows (PA0-6 low bits) the pressed keys pull down for these columns
 * (PB driven low), and the columns for these rows
 */
uint8_t keyboard_rows(uint8_t columns_low);
uint8_t keyboard_columns(uint8_t rows_low);
bool keyboard_any(uint8_t columns_low);
void keyboard_frame(void);              /* the typing queue */

/* ---- joystick/joystick.c ---- */

void joystick_reset(void);
bool joystick_comparator(int mux, int dac);     /* PIA0 PA7 */
uint8_t joystick_buttons(void);         /* PA0-PA3 pulled low */

/* ---- sound/sound.c ---- */

void sound_reset(void);
void sound_update(void);                /* the output level may have changed */
void sound_line_end(void);              /* this line's samples */

/* ---- cartridge/cartridge.c ---- */

extern const uint8_t *cart_rom;         /* NULL: no ROM cartridge */
extern uint32_t cart_size;
void cart_reset(void);
void cart_line(void);                   /* the CART line, once a line */

/* ---- fdc/fdc.c: the disk controller (WD1793 at $FF48, DSKREG $FF40) ---- */

void fdc_reset(void);
uint8_t fdc_read(uint16_t address);
void fdc_write(uint16_t address, uint8_t value);
uint64_t fdc_next_event(void);          /* tick, NEVER */
void fdc_event(void);
void fdc_media_status(coco_media_status_t *status);
bool coco_disks_dirty(void);
void coco_disks_flush(void);

/* ---- cassette/cassette.c ---- */

void cas_reset(void);
bool cas_input(void);                   /* PIA1 PA0 */
void cas_motor(bool on);
void cas_dac_written(uint8_t dac);      /* recording */
uint8_t cas_audio(void);                /* its sound through the mux (0-63) */
bool cas_playing(void);
bool cas_dirty(void);
void cas_flush(void);
void cas_media_status(coco_media_status_t *status);

/* ---- harddisk/vhd.c: the VHD interface at $FF80-$FF86 ---- */

void vhd_reset(void);
uint8_t vhd_read(uint16_t address);
void vhd_write(uint16_t address, uint8_t value);
bool vhd_dirty(void);
void vhd_flush(void);
void vhd_media_status(coco_media_status_t *status);

/* ---- space/space.c: free space on the disks (the web page) ---- */

/* Worked out again for floppy drive 0-1 or the hard disk (COCO_DRIVES);
 * emulation stopped
 */
void space_update(int drive);
/* The images as space.c reads them: fdc.c's (above fdc_disk_read()) and
 * the hard disk's file; false past the end
 */
uint64_t fdc_disk_bytes(int drive, bool one_side);
bool fdc_disk_read(int drive, bool one_side, uint64_t offset, void *buf, uint32_t len);
uint64_t vhd_bytes(void);
bool vhd_read_bytes(uint64_t offset, void *buf, uint32_t len);

/* ---- sound.c ---- */

void sound_silent(bool silent);         /* frames run ahead (fast tape loading) */
