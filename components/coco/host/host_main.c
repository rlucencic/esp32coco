/*
 * Host build of the Color Computer core, for testing on a desktop:
 *
 *   build/coco_host <2|3> [frames] [out.ppm]
 *
 * Environment:
 *   COCO_ROMS=<dir>          the ROM images (default ../../../roms): bas13.rom
 *                            or bas12.rom, extbas11.rom, coco3.rom, disk11.rom
 *   COCO_RAM=128|512         the CoCo 3's memory (KB, default 512)
 *   COCO_CPU=6309            a Hitachi 6309 (else the 6809)
 *   COCO_NODISK=1            no disk controller in the slot
 *   COCO_CART=<file>         a ROM cartridge
 *   COCO_FLOPPY0=, _FLOPPY1= disk images (.DSK)
 *   COCO_HD=<file>           the VHD hard disk
 *   COCO_CAS=<file>          the cassette (.CAS)
 *   COCO_MONITOR=composite   the CoCo 3 on a TV (else RGB)
 *   COCO_ARTIFACTS=0|1|2     the CoCo 2's PMODE 4 colours (off, blue-red, red-blue)
 *   COCO_SCRIPT=<file>       lines "<frame> <command> ..." (not before the
 *                            text typed before them is in):
 *                              type <text> (\n is ENTER), key <n> 0|1,
 *                              joy <port> <x> <y> <buttons>, shot <file.ppm>,
 *                              text, insert <floppy0|floppy1|cart|cas|hd> <path>,
 *                              eject <...>, rewind, reset, warm, peek <hex>,
 *                              dump <hex> <n>, dis <hex> <n>, status
 *   COCO_TEXT=1              print the text screen at the end
 *   COCO_WAV=<file>          write the sound
 *   COCO_TRACE=<hex pc>:<n>  print n instructions from when pc is reached
 *   COCO_PCLOG=<hex pc>:<n>  the registers the first n times pc runs
 *   COCO_STATUS=<n>          machine state every n frames
 *   COCO_DEBUG=<port>        serve the debugger (esp32emu's docs/debugger.md) on
 *                            127.0.0.1:<port> and run in real time until killed
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../coco_internal.h"
#include "emudbg.h"

static uint8_t *load_rom(const char *dir, const char *name, size_t want, bool required)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (required) {
            fprintf(stderr, "can't open %s\n", path);
            exit(1);
        }
        return NULL;
    }
    uint8_t *buf = calloc(1, want);
    size_t n = fread(buf, 1, want, f);
    fclose(f);
    if (n != want) {
        fprintf(stderr, "%s: %zu bytes, expected %zu\n", path, n, want);
        exit(1);
    }
    return buf;
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", COCO_SCREEN_WIDTH, COCO_SCREEN_HEIGHT);
    uint16_t line[COCO_SCREEN_WIDTH];
    for (int y = 0; y < COCO_SCREEN_HEIGHT; y++) {
        coco_render_line_rgb565(y, line);
        for (int x = 0; x < COCO_SCREEN_WIDTH; x++) {
            uint16_t c = line[x];
            uint8_t rgb[3] = {
                (uint8_t)(((c >> 11) & 31) * 255 / 31),
                (uint8_t)(((c >> 5) & 63) * 255 / 63),
                (uint8_t)((c & 31) * 255 / 31),
            };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("host: screenshot %s\n", path);
}

/* The text on the screen: the VDG's (32 x 16 at the SAM's offset or the
 * GIME's CoCo video address) or the GIME's own text modes
 */
static void print_text(uint32_t frame)
{
    if (coco.coco3 && !(gime.reg[0] & 0x80) && !(gime.reg[8] & 0x80)) {
        static const uint8_t cols_for[4] = { 32, 40, 64, 80 };
        uint8_t vres = gime.reg[9];
        int cols = cols_for[((vres >> 2) & 1) | ((vres >> 3) & 2)];
        int stride = (vres & 1) ? 2 : 1;
        int lpr = (gime.reg[8] & 7) <= 2 ? 1 : (gime.reg[8] & 7) + 5;
        int rows = (gime.reg[9] & 0x60) == 0x60 ? 225 / lpr : (gime.reg[9] & 0x60) == 0x20 ? 200 / lpr : 192 / lpr;
        uint32_t base = (uint32_t)gime.reg[0xE] << 3 | (uint32_t)gime.reg[0xD] << 11;
        printf("---- frame %u, %d x %d text at $%05X\n", frame, cols, rows, base);
        for (int r = 0; r < rows; r++) {
            char s[81];
            for (int c = 0; c < cols; c++) {
                uint8_t ch = coco.cfg.ram[(base + (uint32_t)(r * cols * stride + c * stride)) % coco.ram_size] & 0x7F;
                s[c] = isprint(ch) ? (char)ch : '.';
            }
            s[cols] = 0;
            int n = cols;
            while (n > 0 && s[n - 1] == ' ') {
                s[--n] = 0;
            }
            printf("|%s\n", s);
        }
        return;
    }
    uint32_t base = coco.coco3 ? sam_display_offset() + ((uint32_t)(gime.reg[0xE] & 0x3F) << 3) +
                                 ((uint32_t)(gime.reg[0xD] & 0xE0) << 11)
                               : sam_display_offset();
    printf("---- frame %u, screen at $%04X\n", frame, base);
    for (int row = 0; row < 16; row++) {
        char s[33];
        for (int col = 0; col < 32; col++) {
            uint8_t c = coco.cfg.ram[(base + (uint32_t)(row * 32 + col)) % coco.ram_size];
            if (c & 0x80) {
                s[col] = '#';           /* semigraphics */
            } else {
                int k = c & 0x3F;
                char a = (char)(k < 32 ? '@' + k : k);
                if (!(c & 0x40) && k < 32 && k > 0 && k <= 26) {
                    a = (char)('a' + k - 1);   /* lower case (shown inverse on a CoCo 2) */
                }
                s[col] = a;
            }
        }
        s[32] = 0;
        int n = 32;
        while (n > 0 && s[n - 1] == ' ') {
            s[--n] = 0;
        }
        printf("|%s\n", s);
    }
}

/* ---- Script ---- */

typedef struct {
    uint32_t frame;
    char cmd[16];
    char arg[1024];
} line_t;

static line_t *s_script;
static int s_script_n, s_script_pos;

static void load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        exit(1);
    }
    char buf[1200];
    s_script = calloc(8192, sizeof(line_t));
    while (fgets(buf, sizeof(buf), f) && s_script_n < 8192) {
        buf[strcspn(buf, "\r\n")] = 0;
        line_t *l = &s_script[s_script_n];
        int used = 0;
        if (sscanf(buf, "%u %15s %n", &l->frame, l->cmd, &used) >= 2) {
            snprintf(l->arg, sizeof(l->arg), "%s", buf + used);
            s_script_n++;
        }
    }
    fclose(f);
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static bool media(const char *what, const char *path)
{
    if (!strcmp(what, "floppy0")) return path ? coco_disk_insert(0, path) : (coco_disk_eject(0), true);
    if (!strcmp(what, "floppy1")) return path ? coco_disk_insert(1, path) : (coco_disk_eject(1), true);
    if (!strcmp(what, "cart")) return path ? coco_cart_insert(path) : (coco_cart_eject(), true);
    if (!strcmp(what, "cas")) return path ? coco_cas_insert(path) : (coco_cas_eject(), true);
    if (!strcmp(what, "hd")) return path ? coco_hd_insert(path) : (coco_hd_eject(), true);
    return false;
}

static void print_status(uint32_t frame)
{
    coco_status_t s;
    coco_get_status(&s);
    printf("host: frame %u PC $%04X %s%s SAM $%04X PB $%02X", frame, s.pc, s.fast ? "1.79 MHz" : "0.89 MHz",
           s.native ? " native" : "", s.sam, s.pia1_b);
    if (coco.coco3) {
        printf(" GIME");
        for (int i = 0; i < 16; i++) printf(" %02X", s.gime[i]);
        printf(" MMU");
        for (int i = 0; i < 16; i++) printf(" %02X", s.mmu[i]);
    }
    coco_media_status_t m;
    coco_media_status(&m);
    printf("%s tape %u/%u%s%s hd %u/%u last %u\n", s.halted ? " HALT" : "", m.cas_position, m.cas_size,
           m.cas_motor ? " motor" : "", m.cas_recording ? " recording" : "", m.hd_reads, m.hd_writes, m.hd_sector);
}

/* A line waits for its frame and for the text typed before it; the lines
 * after it keep their distance
 */
static uint32_t s_script_delay;

static void run_script(uint32_t frame)
{
    if (s_script_pos < s_script_n && s_script[s_script_pos].frame + s_script_delay <= frame && coco_typing()) {
        s_script_delay++;
        return;
    }
    while (s_script_pos < s_script_n && s_script[s_script_pos].frame + s_script_delay <= frame) {
        if (coco_typing()) {
            s_script_delay++;
            return;
        }
        line_t *l = &s_script[s_script_pos++];
        if (!strcmp(l->cmd, "type")) {
            unescape(l->arg);
            coco_type(l->arg);
        } else if (!strcmp(l->cmd, "key")) {
            int k = 0, d = 0;
            sscanf(l->arg, "%d %d", &k, &d);
            coco_key(k, d);
        } else if (!strcmp(l->cmd, "joy")) {
            int port = 0, x = 32, y = 32;
            unsigned b = 0;
            sscanf(l->arg, "%d %d %d %u", &port, &x, &y, &b);
            coco_joystick(port, x, y, (uint8_t)b);
        } else if (!strcmp(l->cmd, "shot")) {
            write_ppm(l->arg);
        } else if (!strcmp(l->cmd, "text")) {
            print_text(frame);
        } else if (!strcmp(l->cmd, "insert")) {
            char what[16], path[512];
            if (sscanf(l->arg, "%15s %511s", what, path) == 2 && !media(what, path)) {
                printf("host: can't insert %s\n", path);
            }
        } else if (!strcmp(l->cmd, "eject")) {
            media(l->arg, NULL);
        } else if (!strcmp(l->cmd, "rewind")) {
            coco_cas_rewind();
        } else if (!strcmp(l->cmd, "reset")) {
            coco_reset();
        } else if (!strcmp(l->cmd, "warm")) {
            coco_warm_reset();
        } else if (!strcmp(l->cmd, "status")) {
            print_status(frame);
        } else if (!strcmp(l->cmd, "peek")) {
            unsigned a = (unsigned)strtoul(l->arg, NULL, 16);
            printf("host: frame %u $%04X = $%02X\n", frame, a, coco_peek((uint16_t)a));
        } else if (!strcmp(l->cmd, "dis")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            for (unsigned i = 0; i < n; i++) {
                char buf[48];
                int len = m6809_disassemble(&coco.cpu, (uint16_t)a, buf, sizeof(buf));
                printf("host: %04X  %s\n", a, buf);
                a = (uint16_t)(a + (len > 0 ? len : 1));
            }
        } else if (!strcmp(l->cmd, "dump")) {
            unsigned a = 0, n = 16;
            sscanf(l->arg, "%x %u", &a, &n);
            printf("host: frame %u $%04X:", frame, a);
            for (unsigned i = 0; i < n; i++) {
                printf(" %02X", coco_peek((uint16_t)(a + i)));
            }
            printf("\n");
        } else {
            printf("host: unknown command %s\n", l->cmd);
        }
    }
}

/* ---- Tracing ---- */

static int s_trace_pc = -1, s_trace_n;
static bool s_tracing;
static int s_pclog_pc = -1, s_pclog_n;

void m6809_host_hook(m6809_t *c)
{
    if (c->halt || c->wait) {
        return;
    }
    if (s_pclog_pc >= 0 && c->pc == s_pclog_pc && s_pclog_n > 0) {
        printf("%6u/%3d %04X A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X CC=%02X%s cycle %llu\n", (unsigned)coco.frames,
               coco.line, c->pc, c->a, c->b, c->x, c->y, c->u, c->s, c->cc, c->nmi_pending ? " NMI" : "",
               (unsigned long long)c->cycles);
        s_pclog_n--;
    }
    if (s_trace_pc < 0) {
        return;
    }
    if (!s_tracing && c->pc == s_trace_pc) {
        s_tracing = true;
    }
    if (s_tracing && s_trace_n > 0) {
        char dis[40];
        m6809_disassemble(c, c->pc, dis, sizeof(dis));
        printf("%6u/%3d %04X  %-20s A=%02X B=%02X X=%04X Y=%04X U=%04X S=%04X DP=%02X CC=%02X\n",
               (unsigned)coco.frames, coco.line, c->pc, dis, c->a, c->b, c->x, c->y, c->u, c->s, c->dp, c->cc);
        s_trace_n--;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: coco_host <2|3> [frames] [out.ppm]\n");
        return 1;
    }
    int model = atoi(argv[1]);
    int frames = argc > 2 ? atoi(argv[2]) : 300;
    const char *out = argc > 3 ? argv[3] : NULL;
    const char *dir = getenv("COCO_ROMS") ? getenv("COCO_ROMS") : "../../../roms";

    static coco_config_t cfg;
    cfg.model = model == 3 ? COCO_MODEL_3 : COCO_MODEL_2;
    cfg.h6309 = getenv("COCO_CPU") && !strcmp(getenv("COCO_CPU"), "6309");
    if (cfg.model == COCO_MODEL_3) {
        cfg.ram_kb = getenv("COCO_RAM") ? (uint32_t)atoi(getenv("COCO_RAM")) : 512;
        cfg.coco3_rom = load_rom(dir, "coco3.rom", 32768, true);
    } else {
        cfg.ram_kb = 64;
        cfg.basic = load_rom(dir, "bas13.rom", 8192, false);
        if (!cfg.basic) {
            cfg.basic = load_rom(dir, "bas12.rom", 8192, true);
        }
        cfg.extended = load_rom(dir, "extbas11.rom", 8192, false);
    }
    if (!getenv("COCO_NODISK")) {
        cfg.disk_rom = load_rom(dir, "disk11.rom", 8192, false);
    }
    cfg.monitor = getenv("COCO_MONITOR") && !strcmp(getenv("COCO_MONITOR"), "composite") ? COCO_MONITOR_COMPOSITE
                                                                                         : COCO_MONITOR_RGB;
    cfg.artifacts = getenv("COCO_ARTIFACTS") ? (coco_artifact_t)atoi(getenv("COCO_ARTIFACTS")) : COCO_ARTIFACT_BLUE_RED;
    cfg.ram = calloc(1, cfg.ram_kb * 1024);
    cfg.frame = calloc(COCO_SCREEN_WIDTH, COCO_SCREEN_HEIGHT);

    FILE *wav = NULL;
    if (getenv("COCO_WAV")) {
        wav = fopen(getenv("COCO_WAV"), "wb");
        coco_audio_enable(true);
        if (wav) {
            uint8_t hdr[44] = { 0 };
            fwrite(hdr, 1, 44, wav);
        }
    }
    if (getenv("COCO_TRACE")) {
        unsigned pc = 0;
        int n = 200;
        sscanf(getenv("COCO_TRACE"), "%x:%d", &pc, &n);
        s_trace_pc = (int)pc;
        s_trace_n = n;
    }
    if (getenv("COCO_FDCLOG")) {
        extern bool coco_fdc_log;
        coco_fdc_log = true;
    }
    if (getenv("COCO_PCLOG")) {
        unsigned pc = 0;
        int n = 100;
        sscanf(getenv("COCO_PCLOG"), "%x:%d", &pc, &n);
        s_pclog_pc = (int)pc;
        s_pclog_n = n;
    }
    if (!coco_init(&cfg)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    static const char *const media_env[][2] = {
        { "COCO_CART", "cart" }, { "COCO_FLOPPY0", "floppy0" }, { "COCO_FLOPPY1", "floppy1" },
        { "COCO_HD", "hd" }, { "COCO_CAS", "cas" },
    };
    for (size_t i = 0; i < sizeof(media_env) / sizeof(media_env[0]); i++) {
        const char *p = getenv(media_env[i][0]);
        if (p && !media(media_env[i][1], p)) {
            fprintf(stderr, "can't load %s\n", p);
        }
    }
    if (getenv("COCO_CART")) {
        coco_reset();
    }
    if (getenv("COCO_SCRIPT")) {
        load_script(getenv("COCO_SCRIPT"));
    }
    emudbg_host_run("COCO_DEBUG", coco_run_frame, coco_screen_hz());
    int status_every = getenv("COCO_STATUS") ? atoi(getenv("COCO_STATUS")) : 0;
    long samples = 0;
    for (int f = 0; f < frames; f++) {
        run_script((uint32_t)f);
        coco_run_frame();
        if (wav) {
            int16_t buf[2 * 1024];
            int n;
            while ((n = coco_audio_render(buf, 1024)) > 0) {
                fwrite(buf, 4, (size_t)n, wav);
                samples += n;
            }
        }
        if (status_every && f % status_every == 0) {
            print_status((uint32_t)f);
        }
    }
    run_script((uint32_t)frames);
    if (getenv("COCO_TEXT")) {
        print_text((uint32_t)frames);
    }
    if (out) {
        write_ppm(out);
    }
    coco_media_flush();
    for (int d = 0; d <= COCO_DRIVES; d++) {
        uint64_t free_bytes, size;
        if (d < COCO_DRIVES ? coco_disk_space(d, &free_bytes, &size) : coco_hd_space(&free_bytes, &size)) {
            if (d < COCO_DRIVES) {
                printf("Floppy %d: ", d);
            } else {
                printf("Hard disk: ");
            }
            printf("%llu bytes free of %llu\n", (unsigned long long)free_bytes, (unsigned long long)size);
        }
    }
    if (wav) {
        uint32_t data = (uint32_t)samples * 4;
        uint8_t hdr[44] = { 'R', 'I', 'F', 'F' };
        uint32_t v;
        v = 36 + data; memcpy(hdr + 4, &v, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        v = 16; memcpy(hdr + 16, &v, 4);
        uint16_t w = 1; memcpy(hdr + 20, &w, 2);
        w = 2; memcpy(hdr + 22, &w, 2);
        v = COCO_AUDIO_HZ; memcpy(hdr + 24, &v, 4);
        v = COCO_AUDIO_HZ * 4; memcpy(hdr + 28, &v, 4);
        w = 4; memcpy(hdr + 32, &w, 2);
        w = 16; memcpy(hdr + 34, &w, 2);
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &data, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(hdr, 1, 44, wav);
        fclose(wav);
    }
    return 0;
}
