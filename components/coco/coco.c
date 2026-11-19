/*
 * The Color Computer: the frame and its scan lines, the syncs and their
 * interrupts, the PIAs' wiring, the CPU's rate, the colours, the debugger.
 */
#include <math.h>
#include <string.h>
#include "coco_internal.h"
#include "emudbg.h"

coco_machine_t coco;

static volatile bool s_warm_reset;
static uint64_t s_power_ticks;          /* the ticks at the last reset */

/* ---- Colours ----
 *
 * The VDG's: as MAME measures them (mc6847.cpp). The CoCo 3's 64: RGB from
 * the 6 bits (R1 G1 B1 R0 G0 B0, two bits each), or composite: 4 levels
 * (bits 4-5) and 15 hues around the colour wheel (bits 0-3; 0 grey), from
 * green through yellow, red, magenta, blue and cyan, in YUV.
 */
static const uint32_t vdg_rgb[16] = {
    0x30D200, 0xC1E500, 0x4C3AB4, 0x9A3236,     /* green yellow blue red */
    0xBFC8AD, 0x41AF71, 0xC84EF0, 0xD47F00,     /* buff cyan magenta orange */
    0x263016, 0x30D200, 0x263016, 0xBFC8AD,     /* black green, black buff (graphics) */
    0x007C00, 0x30D200, 0x6B2700, 0xFFB700,     /* text: dark/bright green, dark/bright orange */
};
static const uint32_t artifact_rgb[4] = {
    0x2E7DFF, 0xFF7A24,                         /* blue, red-orange (black and buff) */
    0x1C6E30, 0x8EE000,                         /* dark and light green (black and green) */
};

static uint16_t rgb565(uint32_t c)
{
    return (uint16_t)((((c >> 16) & 0xF8) << 8) | (((c >> 8) & 0xFC) << 3) | ((c & 0xF8) >> 3));
}

static uint32_t coco3_rgb(int c)
{
    int r = ((c >> 4) & 2) | ((c >> 2) & 1);
    int g = ((c >> 3) & 2) | ((c >> 1) & 1);
    int b = ((c >> 2) & 2) | (c & 1);
    return (uint32_t)(r * 0x55) << 16 | (uint32_t)(g * 0x55) << 8 | (uint32_t)(b * 0x55);
}

static uint32_t coco3_composite(int c)
{
    int level = (c >> 4) & 3, hue = c & 15;
    static const double grey[4] = { 0.0, 0.18, 0.45, 1.0 };
    static const double luma[4] = { 0.14, 0.32, 0.56, 0.80 };
    double y, u = 0, v = 0;
    if (hue == 0) {
        y = grey[level];
    } else {
        y = luma[level];
        double a = (225.0 - (hue - 1) * 24.0) * M_PI / 180.0;
        double s = level == 3 ? 0.20 : 0.24;
        u = s * cos(a);
        v = s * sin(a);
    }
    double rgb[3] = { y + 1.140 * v, y - 0.395 * u - 0.581 * v, y + 2.032 * u };
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        double x = rgb[i] < 0 ? 0 : rgb[i] > 1 ? 1 : rgb[i];
        out = out << 8 | (uint32_t)(x * 255 + 0.5);
    }
    return out;
}

static void build_palette(void)
{
    for (int i = 0; i < 64; i++) {
        coco.palette[i] = rgb565(coco.cfg.monitor == COCO_MONITOR_COMPOSITE ? coco3_composite(i) : coco3_rgb(i));
    }
    for (int i = 0; i < 16; i++) {
        coco.palette[64 + i] = rgb565(vdg_rgb[i]);
    }
    for (int i = 0; i < 4; i++) {
        coco.palette[80 + i] = rgb565(artifact_rgb[i]);
    }
    coco.signature++;
}

const uint16_t *coco_palette(void)
{
    return coco.palette;
}

void coco_set_monitor(coco_monitor_t monitor)
{
    coco.cfg.monitor = monitor;
    build_palette();
}

void coco_set_artifacts(coco_artifact_t artifacts)
{
    coco.cfg.artifacts = artifacts;
    coco.signature++;
}

/* ---- The CPU's rate, interrupts, events ---- */

void coco_rate_changed(void)
{
    int tpc = (sam.state & (SAM_R1 | SAM_R0)) ? 2 : 4;
    if (tpc != coco.tick_per_cycle) {
        coco.tick_base = coco_now();
        coco.cycle_base = coco.cpu.cycles;
        coco.tick_per_cycle = tpc;
        coco.tick_shift = tpc == 2 ? 1 : 2;
        m6809_end_run(&coco.cpu);       /* the line's end moved */
    }
}

void coco_update_interrupts(void)
{
    uint32_t irq = pia_irq(0) ? INT_PIA : 0;
    uint32_t firq = pia_irq(1) ? INT_PIA : 0;
    if (coco.coco3) {
        if ((gime.reg[0] & 0x20) && gime.irq) {
            irq |= INT_GIME;
        }
        if ((gime.reg[0] & 0x10) && gime.firq) {
            firq |= INT_GIME;
        }
    }
    coco.cpu.irq = irq;
    coco.cpu.firq = firq;
}

void coco_event_changed(void)
{
    m6809_end_run(&coco.cpu);
}

static uint64_t next_event(void)
{
    uint64_t t = fdc_next_event();
    if (coco.coco3 && gime.timer_at < t) {
        t = gime.timer_at;
    }
    return t;
}

static void service_events(void)
{
    uint64_t now = coco_now();
    for (int guard = 0; guard < 64; guard++) {
        bool any = false;
        if (fdc_next_event() <= now) {
            fdc_event();
            any = true;
        }
        if (coco.coco3 && gime.timer_at <= now) {
            gime_timer_event();
            any = true;
        }
        if (!any) {
            break;
        }
    }
}

/* ---- The PIAs' wiring ----
 *
 *   PIA0 ($FF00)  PA0-6 keyboard rows (and the joystick buttons), PA7 the
 *                 joystick comparator; PB0-7 keyboard columns; CA1 HS, CB1
 *                 FS (IRQ); CA2/CB2 the analogue mux's select lines
 *   PIA1 ($FF20)  PA0 cassette in, PA2-7 the DAC; PB1 single-bit sound,
 *                 PB2 RAM size, PB3-7 the VDG's mode (CSS, GM0 + INT/EXT,
 *                 GM1, GM2, A/G); CA2 cassette motor, CB1 CART (FIRQ), CB2
 *                 sound enable
 */
static int mux_select(void)
{
    return (pia_c2(0, 0) ? 1 : 0) | (pia_c2(0, 1) ? 2 : 0);
}

uint8_t coco_pia_input(int p, int side)
{
    if (p == 0) {
        if (side == 0) {
            uint8_t columns_low = (uint8_t)~pia_output(0, 1);
            uint8_t rows = (uint8_t)(~keyboard_rows(columns_low) & ~joystick_buttons() & 0x7F);
            int dac = pia_output(1, 0) >> 2;
            return rows | (joystick_comparator(mux_select(), dac) ? 0x80 : 0);
        }
        uint8_t rows_low = (uint8_t)(~pia_output(0, 0) & 0x7F);
        return (uint8_t)~keyboard_columns(rows_low);
    }
    if (side == 0) {
        return 0xFE | (cas_input() ? 1 : 0);
    }
    /* PB2: 64K RAM (PIA0 PB6 fed back), as the 64 KB board's jumper */
    bool sense = !coco.coco3 && (pia_output(0, 1) & 0x40);
    return (uint8_t)(0xFB | (sense ? 0x04 : 0x00));
}

void coco_pia_changed(int p)
{
    if (p == 1) {
        cas_motor(pia_c2(1, 0));
        cas_dac_written(pia_output(1, 0) >> 2);
    } else if (coco.coco3) {
        gime_set_line(GIME_EI1, keyboard_any((uint8_t)~pia_output(0, 1)));
    }
    sound_update();
}

/* ---- Lines ---- */

void coco_line_rendered(int frame_row, bool changed)
{
    (void)frame_row;
    if (changed) {
        coco.frame_changed = true;
    }
}

static void line_start(int line)
{
    if (coco.coco3) {
        if (line == 0) {
            gime_frame_start();
        }
        if (line == gime.top_border + gime.body) {
            gime_set_line(GIME_VBORD, true);
            gime_set_line(GIME_VBORD, false);
        }
        if (line == 243) {
            pia_set_c1(0, 1, false);    /* field sync: low during the retrace */
        } else if (line == 249) {
            pia_set_c1(0, 1, true);
        }
        gime_set_line(GIME_HBORD, true);
        gime_set_line(GIME_HBORD, false);
    } else {
        if (line == VDG_TOP_BORDER + VDG_ACTIVE - 1) {
            pia_set_c1(0, 1, false);    /* field sync falls after the picture */
        } else if (line == 248) {
            pia_set_c1(0, 1, true);     /* and rises at the end of the retrace */
            sam_counter_reset();
        }
    }
    pia_set_c1(0, 0, false);            /* horizontal sync */
    pia_set_c1(0, 0, true);
    cart_line();
    if (coco.coco3) {
        gime_set_line(GIME_EI1, keyboard_any((uint8_t)~pia_output(0, 1)));
    }
}

static void COCO_HOT run_line(void)
{
    uint64_t end_tick = coco.line_tick + LINE_TICKS;
    for (;;) {
        coco.line_end = coco_cycle_at(end_tick);
        service_events();
        uint64_t now = coco.cpu.cycles;
        if (now >= coco.line_end) {
            break;
        }
        uint64_t stop = coco.line_end;
        uint64_t ev = next_event();
        if (ev != NEVER) {
            uint64_t c = coco_cycle_at(ev);
            if (c < stop) {
                stop = c > now ? c : now + 1;
            }
        }
        coco.cpu.until = stop;
        m6809_run(&coco.cpu);
    }
}

static void line_end(int line)
{
    if (coco.coco3) {
        gime_render_line(line);
    } else {
        vdg_render_line(line);
        sam_hsync();
    }
    sound_line_end();
}

static void warm_reset_now(void)
{
    sam_reset();
    pia_reset();
    if (coco.coco3) {
        gime_reset();
    }
    fdc_reset();
    vhd_reset();
    cart_reset();
    memory_map();
    coco.tick_base = coco_now();
    coco.cycle_base = coco.cpu.cycles;
    coco.tick_per_cycle = 4;
    coco.tick_shift = 2;
    m6809_reset(&coco.cpu);
    coco_update_interrupts();
}

static void COCO_HOT one_frame(void)
{
    keyboard_frame();
    for (int line = 0; line < LINES; line++) {
        coco.line = line;
        line_start(line);
        run_line();
        line_end(line);
        coco.line_tick += LINE_TICKS;
    }
    coco.frames++;
}

static int s_tape_speed = 1;

void coco_set_tape_speed(int factor)
{
    s_tape_speed = factor < 1 ? 1 : factor > 16 ? 16 : factor;
}

void COCO_HOT coco_run_frame(void)
{
    if (s_warm_reset) {
        s_warm_reset = false;
        warm_reset_now();
    }
    coco.frame_changed = false;
    /* A tape playing: more frames, the extra ones silent */
    for (int i = 1; i < s_tape_speed && cas_playing(); i++) {
        sound_silent(true);
        one_frame();
    }
    sound_silent(false);
    one_frame();
    if (coco.frame_changed) {
        coco.signature++;
    }
}

/* ---- Media ---- */

void coco_media_status(coco_media_status_t *s)
{
    memset(s, 0, sizeof(*s));
    fdc_media_status(s);
    cas_media_status(s);
    vhd_media_status(s);
}

bool coco_media_dirty(void)
{
    return coco_disks_dirty() || cas_dirty() || vhd_dirty();
}

void coco_media_flush(void)
{
    coco_disks_flush();
    cas_flush();
    vhd_flush();
}

/* ---- Screen ---- */

int coco_screen_hz(void)
{
    return 60;
}

void COCO_HOT coco_render_line_rgb565(int y, uint16_t *dst)
{
    const uint8_t *src = coco.cfg.frame + y * COCO_SCREEN_WIDTH;
    const uint16_t *pal = coco.palette;
    for (int x = 0; x < COCO_SCREEN_WIDTH; x++) {
        dst[x] = pal[src[x]];
    }
}

uint32_t coco_screen_signature(void)
{
    return coco.signature;
}

/* ---- The debugger (emudbg.h) ---- */

static emudbg_cpu_t s_dbg_cpus[] = {
    { .name = "6809", .arch = &emudbg_arch_6809, .core = &coco.cpu },
};

static void dbg_where(char *buf, size_t len)
{
    snprintf(buf, len, "frame %lu, line %d, cycle %d", (unsigned long)coco.frames, coco.line,
             (int)(coco.cpu.cycles - coco_cycle_at(coco.line_tick)));
}

static void dbg_screen(int *width, int *height)
{
    *width = COCO_SCREEN_WIDTH;
    *height = COCO_SCREEN_HEIGHT;
}

static emudbg_machine_t s_dbg = {
    .cpus = s_dbg_cpus,
    .cpu_count = 1,
    .where = dbg_where,
    .reset = coco_reset,
    .screen = dbg_screen,
    .render_line = coco_render_line_rgb565,
    .type = coco_type,
};

/* ---- Starting ---- */

bool coco_init(const coco_config_t *config)
{
    coco_config_t cfg = *config;
    bool coco3 = cfg.model == COCO_MODEL_3;
    if (!cfg.ram || !cfg.frame || (coco3 ? !cfg.coco3_rom : !cfg.basic)) {
        return false;
    }
    if (coco3) {
        cfg.ram_kb = cfg.ram_kb >= 512 ? 512 : 128;
    } else {
        cfg.ram_kb = 64;
    }
    memset(&coco, 0, sizeof(coco));
    coco.cfg = cfg;
    coco.coco3 = coco3;
    coco.ram_size = cfg.ram_kb * 1024;
    memset(coco.ff_page, 0xFF, sizeof(coco.ff_page));
    memset(cfg.frame, 64 + 8, COCO_SCREEN_WIDTH * COCO_SCREEN_HEIGHT);
    build_palette();
    cpu_init();
    vdg_init();
    keyboard_reset();
    joystick_reset();
    sound_reset();
    cas_reset();
    coco_reset();
    s_dbg.name = coco3 ? "Tandy Color Computer 3" : "Tandy Color Computer 2";
    emudbg_set_machine(&s_dbg);
    COCO_LOG("%s, %s, %lu KB%s", coco3 ? "CoCo 3" : "CoCo 2", cfg.h6309 ? "6309" : "6809",
             (unsigned long)cfg.ram_kb, cfg.disk_rom ? ", Disk BASIC" : "");
    return true;
}

void coco_reset(void)
{
    /* Power off and on: the RAM's contents gone (BASIC's cold start) */
    memset(coco.cfg.ram, 0, coco.ram_size);
    s_power_ticks = coco_now();
    keyboard_reset();
    warm_reset_now();
}

void coco_warm_reset(void)
{
    s_warm_reset = true;
}

void coco_get_status(coco_status_t *s)
{
    s->frames = coco.frames;
    s->pc = coco.cpu.pc;
    s->native = coco.cfg.h6309 && (coco.cpu.md & M6309_NM);
    s->fast = coco.tick_per_cycle == 2;
    s->sam = sam.state;
    s->pia1_b = pia_output(1, 1);
    memcpy(s->gime, gime.reg, sizeof(s->gime));
    memcpy(s->mmu, gime.mmu, sizeof(s->mmu));
    s->halted = coco.cpu.halt;
}

void coco_get_chips(coco_chips_t *c)
{
    memset(c, 0, sizeof(*c));
    c->sam = sam.state;
    for (int p = 0; p < 2; p++) {
        c->pia_ctl[p][0] = pia[p].ctl[0];
        c->pia_ctl[p][1] = pia[p].ctl[1];
    }
    c->pia1_b = pia_output(1, 1);
    c->video = coco.coco3 ? gime.video_address : sam_display_offset();
    if (coco.coco3) {
        memcpy(c->gime, gime.reg, sizeof(c->gime));
        memcpy(c->mmu, gime.mmu, sizeof(c->mmu));
        memcpy(c->palette, gime.palette, sizeof(c->palette));
    }
    sound_get_state(c);
}

uint8_t coco_peek(uint16_t address)
{
    return memory_peek(address);
}
