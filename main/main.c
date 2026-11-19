/*
 * Tandy Color Computer 2 and 3 emulator on the Makerfabs MaTouch ESP32-S3
 * 7" or the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, USB, setup screen,
 * web page, console) is the shared runtime in ../esp32emu; this file plugs
 * the CoCo into it: memory, ROMs, settings, keyboard map, joystick, the
 * drives (two floppies, the hard disk, the cassette, the cartridge), status.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "emu/emu.h"
#include "coco.h"
#include "rom_store.h"
#include "settings.h"

static const char *TAG = "main";

static coco_config_t s_cfg;
static uint8_t *s_rom[ROM_COUNT];
static bool s_started;

/* ---- Memory ----
 *
 * Everything at power-on, before the screen, WiFi and Bluetooth fragment
 * PSRAM: 512 KB of RAM (the CoCo 3's most; the CoCo 2 uses 64), the ROMs,
 * the picture.
 */
static void reserve_memory(void)
{
    settings_load();
    s_cfg.ram = heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < ROM_COUNT; i++) {
        s_rom[i] = heap_caps_malloc(rom_store_size(i), MALLOC_CAP_SPIRAM);
    }
    s_cfg.frame = heap_caps_calloc(COCO_SCREEN_WIDTH * COCO_SCREEN_HEIGHT, 1, MALLOC_CAP_SPIRAM);
}

static void storage(bool sd_card)
{
    emu_set_drive_info(sd_card ? "Disks and tapes in coco/, cartridges in coco/roms on the SD card" : "No SD card: no disks");
}

/* ---- Starting ---- */

static bool load(rom_id_t id)
{
    return s_rom[id] && rom_store_load(id, s_rom[id]);
}

static bool start(void)
{
    if (s_cfg.ram == NULL || s_cfg.frame == NULL) {
        emu_set_message("Not enough memory: see the log.");
        return false;
    }
    bool coco3 = settings.model == COCO_MODEL_3;
    s_cfg.model = settings.model;
    s_cfg.h6309 = settings.cpu == 1;
    s_cfg.ram_kb = coco3 ? (settings.ram ? 512 : 128) : 64;
    s_cfg.monitor = settings.monitor;
    s_cfg.artifacts = settings.artifacts == 0 ? COCO_ARTIFACT_BLUE_RED
                      : settings.artifacts == 1 ? COCO_ARTIFACT_RED_BLUE : COCO_ARTIFACT_OFF;
    s_cfg.basic = s_cfg.extended = s_cfg.coco3_rom = NULL;
    if (coco3) {
        if (!load(ROM_COCO3)) {
            emu_set_message("No CoCo 3 ROM: put coco3.rom into roms/ and flash with idf.py flash.");
            return false;
        }
        s_cfg.coco3_rom = s_rom[ROM_COCO3];
    } else {
        if (!load(ROM_BASIC)) {
            emu_set_message("No Color BASIC ROM: put bas13.rom into roms/ and flash with idf.py flash.");
            return false;
        }
        s_cfg.basic = s_rom[ROM_BASIC];
        s_cfg.extended = load(ROM_EXTENDED) ? s_rom[ROM_EXTENDED] : NULL;
    }
    s_cfg.disk_rom = load(ROM_DISK) ? s_rom[ROM_DISK] : NULL;
    if (!coco_init(&s_cfg)) {
        emu_set_message("The CoCo could not be set up: see the log.");
        return false;
    }
    coco_set_tape_speed(settings.tape_speed);
    s_started = true;
    ESP_LOGI(TAG, "%s, %s, %lu KB", coco3 ? "CoCo 3" : "CoCo 2", s_cfg.h6309 ? "6309" : "6809",
             (unsigned long)s_cfg.ram_kb);
    return true;
}

static void screen_mode(emu_screen_t *mode)
{
    mode->width = COCO_SCREEN_WIDTH;
    mode->height = COCO_SCREEN_HEIGHT;
    mode->hz = coco_screen_hz();
}

/* ---- Keyboard ----
 *
 * Letters by position. Digits and symbols by the character they give on a
 * US keyboard, with SHIFT as the CoCo needs it (Shift+' is ", which is
 * SHIFT+2 on the CoCo). Return ENTER, Backspace and the cursor keys the
 * arrows, Esc or Pause BREAK, Home CLEAR, Caps Lock SHIFT+0 (the case
 * toggle); Ctrl and Alt are the CoCo 3's CTRL and ALT, F1 and F2 its F1 and
 * F2. F5 is the reset button. The numeric keypad is the joystick (8 4 6 2
 * and the diagonals, 0 or 5 the button); F11 switches between the right and
 * the left one.
 */
typedef struct {
    int8_t key;                 /* CoCo key, -1 none */
    int8_t shift;               /* forced for it: -1 as held */
} held_t;

static held_t s_held[256];      /* per HID usage */
static bool s_pc_shift_l, s_pc_shift_r;
static bool s_shift;            /* as the CoCo has it now */

/* US layout: the characters of the digit and symbol keys */
static const char us_plain[0x39] = {
    [0x1E] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x2D] = '-', '=', '[', ']', '\\', '#', ';', '\'', '`', ',', '.', '/',
};
static const char us_shift[0x39] = {
    [0x1E] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    [0x2D] = '_', '+', '{', '}', '|', '~', ':', '"', '~', '<', '>', '?',
};

static void set_shift(bool on)
{
    if (on != s_shift) {
        s_shift = on;
        coco_key(COCO_KEY_SHIFT, on);
    }
}

static void shift_as_held(void)
{
    set_shift(s_pc_shift_l || s_pc_shift_r);
}

static void press(uint8_t usage, int key, int shift)
{
    if (shift >= 0) {
        set_shift(shift);
    }
    coco_key(key, true);
    s_held[usage] = (held_t){ (int8_t)key, (int8_t)shift };
}

static void release(uint8_t usage)
{
    if (s_held[usage].key >= 0) {
        coco_key(s_held[usage].key, false);
        bool forced = s_held[usage].shift >= 0;
        s_held[usage].key = -1;
        if (forced) {
            shift_as_held();
        }
    }
}

/* ---- Joystick: the keypad, the mouse, the touchpad ---- */

static uint8_t s_pad;                   /* keypad: 1 up 2 down 4 left 8 right 16 button */
static int s_mouse_x = 32, s_mouse_y = 32;
static uint8_t s_mouse_buttons;

static void joystick_update(void)
{
    int x = s_mouse_x, y = s_mouse_y;
    if (s_pad & 0x04) x = 0;
    if (s_pad & 0x08) x = 63;
    if (s_pad & 0x01) y = 0;
    if (s_pad & 0x02) y = 63;
    uint8_t b = (uint8_t)((s_mouse_buttons & 1) | ((s_mouse_buttons >> 1) & 1) << 1 | ((s_pad & 0x10) ? 1 : 0));
    coco_joystick(settings.joystick, x, y, b);
    coco_joystick(1 - settings.joystick, 32, 32, 0);
}

static bool keypad_joystick(uint8_t usage, bool pressed)
{
    static const uint8_t bits[] = {
        0x02 | 0x04, 0x02, 0x02 | 0x08,     /* 1 2 3 */
        0x04, 0x10, 0x08,                   /* 4 5 6 */
        0x01 | 0x04, 0x01, 0x01 | 0x08,     /* 7 8 9 */
        0x10,                               /* 0 */
        0x10,                               /* . */
    };
    if (usage < 0x59 || usage > 0x63) {
        return false;
    }
    uint8_t b = bits[usage - 0x59];
    s_pad = pressed ? s_pad | b : s_pad & ~b;
    joystick_update();
    return true;
}

static void hid_mouse(int dx, int dy, uint8_t buttons)
{
    /* the CoCo's mouse is a joystick: a position, 0-63 */
    static int fx = 32 * 8, fy = 32 * 8;
    fx += dx;
    fy += dy;
    fx = fx < 0 ? 0 : fx > 63 * 8 ? 63 * 8 : fx;
    fy = fy < 0 ? 0 : fy > 63 * 8 ? 63 * 8 : fy;
    s_mouse_x = fx / 8;
    s_mouse_y = fy / 8;
    s_mouse_buttons = buttons;
    joystick_update();
}

/* The touch screen as a touchpad (the runtime's): the joystick follows the
 * finger's moves, the picture's 512 x 192 being its 64 x 64 steps (in the
 * mouse's eighths of a step: a pixel one across, 8/3 down); a tap is the
 * button, the long press the second one
 */
static void touchpad(int dx, int dy, uint8_t buttons)
{
    static int thirds;                  /* of an eighth, down */
    thirds += dy * 8;
    int ey = thirds / 3;
    thirds -= ey * 3;
    hid_mouse(dx, ey, buttons);
}

static void hid_key(uint8_t usage, bool pressed)
{
    if (usage >= 0xE0 && usage <= 0xE7) {
        /* LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
        switch (usage) {
        case 0xE1: s_pc_shift_l = pressed; shift_as_held(); return;
        case 0xE5: s_pc_shift_r = pressed; shift_as_held(); return;
        case 0xE0: case 0xE4: coco_key(COCO_KEY_CTRL, pressed); return;
        case 0xE2: case 0xE6: coco_key(COCO_KEY_ALT, pressed); return;
        default: return;
        }
    }
    if (!pressed) {
        release(usage);
        keypad_joystick(usage, false);
        return;
    }
    if (keypad_joystick(usage, true)) {
        return;
    }
    if (usage >= 0x04 && usage <= 0x1D) {
        press(usage, COCO_KEY_A + (usage - 0x04), -1);
        return;
    }
    switch (usage) {
    case 0x28: case 0x58: press(usage, COCO_KEY_ENTER, -1); return;
    case 0x29: case 0x48: press(usage, COCO_KEY_BREAK, -1); return;     /* Esc, Pause */
    case 0x2A: case 0x50: press(usage, COCO_KEY_LEFT, -1); return;      /* Backspace, left */
    case 0x4F: press(usage, COCO_KEY_RIGHT, -1); return;
    case 0x51: press(usage, COCO_KEY_DOWN, -1); return;
    case 0x52: press(usage, COCO_KEY_UP, -1); return;
    case 0x2C: press(usage, COCO_KEY_SPACE, -1); return;
    case 0x4A: press(usage, COCO_KEY_CLEAR, -1); return;                /* Home */
    case 0x39: press(usage, COCO_KEY_0, 1); return;                     /* Caps Lock: SHIFT+0 */
    case 0x3A: press(usage, COCO_KEY_F1, -1); return;
    case 0x3B: press(usage, COCO_KEY_F2, -1); return;
    case 0x3E: coco_warm_reset(); return;                               /* F5: reset */
    case 0x44:                                                          /* F11: the other joystick */
        coco_joystick(settings.joystick, 32, 32, 0);
        settings.joystick ^= 1;
        settings_save();
        joystick_update();
        ESP_LOGI(TAG, "joystick: the %s one", settings.joystick ? "left" : "right");
        return;
    default:
        break;
    }
    char c = 0;
    bool pc_shift = s_pc_shift_l || s_pc_shift_r;
    if (usage == 0x64) {
        c = pc_shift ? '|' : '\\';      /* the ISO key */
    } else if (usage < sizeof(us_plain)) {
        c = pc_shift ? us_shift[usage] : us_plain[usage];
    }
    bool shift;
    int key = c ? coco_ascii_key(c, &shift) : -1;
    if (key >= 0) {
        press(usage, key, shift);
    }
}

/* ---- Setup screen ---- */

static const char *const model_names[] = { "CoCo 2", "CoCo 3", "" };
static const char *const cpu_names[] = { "Motorola 6809", "Hitachi 6309", "" };
static const char *const ram_names_2[] = { "64 KB", "" };
static const char *const ram_names_3[] = { "128 KB", "512 KB", "" };
static const char *const monitor_names_2[] = { "TV, blue-red", "TV, red-blue", "No artifacts", "" };
static const char *const monitor_names_3[] = { "RGB (CM-8)", "TV (composite)", "" };

enum { CHOICE_MODEL, CHOICE_CPU, CHOICE_MEMORY, CHOICE_MONITOR, CHOICES };

static int model_get(void)
{
    return settings.model;
}

static void model_set(int i)
{
    if (i >= 0 && i < COCO_MODEL_COUNT) {
        settings.model = (uint8_t)i;
        settings_save();
    }
}

static int cpu_get(void)
{
    return settings.cpu;
}

static void cpu_set(int i)
{
    if (i == 0 || i == 1) {
        settings.cpu = (uint8_t)i;
        settings_save();
    }
}

static int ram_get(void)
{
    return settings.model == COCO_MODEL_3 ? settings.ram : 0;
}

static void ram_set(int i)
{
    if (settings.model == COCO_MODEL_3 && (i == 0 || i == 1)) {
        settings.ram = (uint8_t)i;
        settings_save();
    }
}

/* Live: the colours change at once */
static int monitor_get(void)
{
    return settings.model == COCO_MODEL_3 ? settings.monitor : settings.artifacts;
}

static void monitor_set(int i)
{
    if (settings.model == COCO_MODEL_3) {
        if (i == 0 || i == 1) {
            settings.monitor = (uint8_t)i;
            if (s_started && s_cfg.model == COCO_MODEL_3) {
                coco_set_monitor(i);
            }
        }
    } else if (i >= 0 && i <= 2) {
        settings.artifacts = (uint8_t)i;
        if (s_started && s_cfg.model == COCO_MODEL_2) {
            coco_set_artifacts(i == 0 ? COCO_ARTIFACT_BLUE_RED : i == 1 ? COCO_ARTIFACT_RED_BLUE : COCO_ARTIFACT_OFF);
        }
    }
    settings_save();
}

static const emu_choice_t choices[CHOICES] = {
    { "Model", model_names, model_get, model_set, false },
    { "CPU", cpu_names, cpu_get, cpu_set, false },
    { "Memory", ram_names_3, ram_get, ram_set, false },
    { "Monitor", monitor_names_3, monitor_get, monitor_set, true },
};

/* The Memory and Monitor rows follow the model */
static const char *const *choice_names(int i)
{
    bool coco3 = settings.model == COCO_MODEL_3;
    if (i == CHOICE_MEMORY) {
        return coco3 ? ram_names_3 : ram_names_2;
    }
    if (i == CHOICE_MONITOR) {
        return coco3 ? monitor_names_3 : monitor_names_2;
    }
    return NULL;
}

/* ---- Drives: two floppies, the hard disk, the cassette, the cartridge ---- */

enum { DRIVE_0, DRIVE_1, DRIVE_HD, DRIVE_CAS, DRIVE_CART, DRIVES };

static const char *const extensions[] = { ".dsk", ".os9", ".vhd", ".cas", ".rom", ".ccc", ".bin", NULL };
static const char *const cart_extensions[] = { ".rom", ".ccc", ".bin", NULL };     /* in coco/roms */
static const char *const drive_names[] = { "Floppy 0", "Floppy 1", "Hard disk", "Cassette", "Cartridge" };

#define HD_BYTES        (32u * 1024 * 1024)
#define BLANK_TAPE      1u

static const disks_size_t sizes[] = {
    { COCO_DISK_35_BYTES, "156 KB floppy (35 tracks)" },
    { COCO_DISK_40DS_BYTES, "360 KB floppy (40 tracks, 2 sides)" },
    { COCO_DISK_80DS_BYTES, "720 KB floppy (80 tracks, 2 sides)" },
    { HD_BYTES, "32 MB hard disk (.VHD)" },
    { BLANK_TAPE, "Empty tape (.CAS)" },
};

static bool has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    for (int i = 0; exts[i]; i++) {
        if (!strcasecmp(dot, exts[i])) {
            return true;
        }
    }
    return false;
}

/* A cartridge in coco/roms, not a file there that overrides a built-in ROM */
static bool is_cart(const char *name)
{
    const char *base = strrchr(name, '/');
    return has_ext(name, cart_extensions) && !rom_store_is_rom(base ? base + 1 : name);
}

static bool accepts(int drive, const char *name)
{
    static const char *const floppy[] = { ".dsk", ".os9", NULL };
    static const char *const hd[] = { ".vhd", NULL };
    static const char *const tape[] = { ".cas", NULL };
    switch (drive) {
    case DRIVE_HD: return has_ext(name, hd);
    case DRIVE_CAS: return has_ext(name, tape);
    case DRIVE_CART: return is_cart(name);
    default: return has_ext(name, floppy);
    }
}

static bool takes_blank(int drive)
{
    return drive != DRIVE_CART;
}

static uint32_t default_size(int drive)
{
    return drive == DRIVE_HD ? HD_BYTES : drive == DRIVE_CAS ? BLANK_TAPE : COCO_DISK_35_BYTES;
}

static const char *extension_for(uint32_t bytes)
{
    return bytes == HD_BYTES ? ".vhd" : bytes == BLANK_TAPE ? ".cas" : ".dsk";
}

static bool create(const char *path, uint32_t bytes)
{
    if (bytes == HD_BYTES) {
        return coco_hd_create(path, bytes);
    }
    if (bytes == BLANK_TAPE) {
        return coco_cas_create(path);
    }
    return coco_disk_create(path, bytes);
}

/* The cartridge is part of the machine: it starts again */
static void restart_if_started(void)
{
    if (s_started) {
        coco_reset();
    }
}

static bool insert(int drive, const char *path)
{
    if (!accepts(drive, path)) {
        return false;
    }
    switch (drive) {
    case DRIVE_HD:
        return coco_hd_insert(path);
    case DRIVE_CAS:
        return coco_cas_insert(path);
    case DRIVE_CART:
        if (!coco_cart_insert(path)) {
            return false;
        }
        restart_if_started();
        return true;
    default:
        return coco_disk_insert(drive, path);
    }
}

static void eject(int drive)
{
    switch (drive) {
    case DRIVE_HD: coco_hd_eject(); break;
    case DRIVE_CAS: coco_cas_eject(); break;
    case DRIVE_CART:
        coco_cart_eject();
        restart_if_started();
        break;
    default: coco_disk_eject(drive); break;
    }
}

static const char *path(int drive)
{
    switch (drive) {
    case DRIVE_HD: return coco_hd_path();
    case DRIVE_CAS: return coco_cas_path();
    case DRIVE_CART: return coco_cart_path();
    default: return coco_disk_path(drive);
    }
}

static bool write_protected(int drive)
{
    switch (drive) {
    case DRIVE_HD: return coco_hd_write_protected();
    case DRIVE_CAS: return coco_cas_write_protected();
    case DRIVE_CART: return true;
    default: return coco_disk_write_protected(drive);
    }
}

static const disks_config_t disks = {
    .folder = "coco",
    .extensions = extensions,
    .cart_extensions = cart_extensions,
    .description = "Disks (.DSK .VHD), tapes (.CAS), cartridges (.ROM .CCC)",
    .title = "Drives",
    .button = "Drives",
    .nvs_namespace = "cocodisks",
    .drives = DRIVES,
    .drive_names = drive_names,
    .new_prefix = "BLANK",
    .new_extension = ".dsk",
    .sizes = sizes,
    .size_count = sizeof(sizes) / sizeof(sizes[0]),
    .max_upload = 512u * 1024 * 1024,
    .insert = insert,
    .eject = eject,
    .path = path,
    .write_protected = write_protected,
    .create = create,
    .accepts = accepts,
    .takes_blank = takes_blank,
    .default_size = default_size,
    .extension_for = extension_for,
};

/* Floppy, tape and hard disk writes go to the SD card from here (core 0),
 * between frames, once nothing has been written for a second
 */
static void flush(void *arg)
{
    coco_media_flush();
}

static void every_second(void)
{
    static int dirty_for;
    if (coco_media_dirty()) {
        if (++dirty_for >= 2) {
            emu_paused(flush, NULL);
            dirty_for = 0;
        }
    } else {
        dirty_for = 0;
    }
}

/* ---- Status ----
 *
 * The machine card's rows: the chips' registers now (coco_get_chips()).
 * The whole reply has esp32emu's 4 KB: the rows stay short.
 */

static void add(char *text, size_t size, size_t *n, const char *fmt, ...)
{
    if (*n >= size - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(text + *n, size - *n, fmt, ap);
    va_end(ap);
    *n = k < 0 ? *n : *n + k >= size ? size - 1 : *n + k;
}

/* A colour number's colour, as the picture has it */
static void swatch(char *text, size_t size, size_t *n, uint8_t colour)
{
    uint16_t v = coco_palette()[colour < COCO_COLOURS ? colour : 0];
    unsigned r = (v >> 11) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
    add(text, size, n, " <b style=\"background:#%02x%02x%02x\">&nbsp;&nbsp;&nbsp;</b>", r, g, b);
}

static void rom_row(emu_json_t *j, const coco_chips_t *c, bool coco3)
{
    static const struct {
        rom_id_t id;
        const char *what;
        uint8_t version;        /* where the file's name has it: bas13.rom 1.3, disk10.rom 1.0 */
    } roms[] = {
        { ROM_COCO3, "Super Extended BASIC", 0 }, { ROM_BASIC, "Color BASIC", 3 },
        { ROM_EXTENDED, "Extended BASIC 1.1", 0 }, { ROM_DISK, "Disk BASIC", 4 },
    };
    const uint8_t *loaded[] = { s_cfg.coco3_rom, s_cfg.basic, s_cfg.extended, s_cfg.disk_rom };
    char text[256];
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {      /* those from the SD card, then the flash's */
        int listed = 0;
        for (int i = 0; i < 4; i++) {
            if (!loaded[i] || rom_store_from_sd(roms[i].id) != (pass == 0)) {
                continue;
            }
            const char *name = rom_store_name(roms[i].id);
            add(text, sizeof(text), &n, "%s%s", listed++ ? ", " : n ? "; " : "", roms[i].what);
            if (roms[i].version && strlen(name) > roms[i].version + 1u) {
                add(text, sizeof(text), &n, " %c.%c", name[roms[i].version], name[roms[i].version + 1]);
            }
        }
        if (listed) {
            add(text, sizeof(text), &n, " from the %s", pass == 0 ? "SD card" : "flash");
        }
    }
    if (!coco3 && !s_cfg.extended) {
        add(text, sizeof(text), &n, "; no Extended BASIC");
    }
    if (!s_cfg.disk_rom) {
        add(text, sizeof(text), &n, "; no Disk BASIC");
    }
    /* in the CPU's map now: none with the SAM's TY (all RAM) */
    if (c->sam & 0x8000) {
        add(text, sizeof(text), &n, "; in now: none (all RAM)");
    } else if (coco3) {
        static const char *const maps[4] = {
            "16 KB internal + 16 KB cartridge", "16 KB internal + 16 KB cartridge", "32 KB internal", "32 KB cartridge",
        };
        add(text, sizeof(text), &n, "; in now: %s", maps[c->gime[0] & 3]);
    } else {
        add(text, sizeof(text), &n, "; in now: all, $8000-$FEFF");
    }
    emu_json_row(j, "ROM", text);
}

static void memory_row(emu_json_t *j, const coco_chips_t *c, bool coco3)
{
    char text[200];
    size_t n = 0;
    if (coco3) {
        uint8_t init0 = c->gime[0];
        int task = c->gime[1] & 1;
        add(text, sizeof(text), &n, "INIT0 $%02X: ", init0);
        if (init0 & 0x40) {
            add(text, sizeof(text), &n, "MMU on, task %d:", task);
            for (int i = 0; i < 8; i++) {
                add(text, sizeof(text), &n, " $%02X", c->mmu[task * 8 + i] & 0x3F);
            }
        } else {
            add(text, sizeof(text), &n, "MMU off, blocks $38-$3F");
        }
        if (init0 & 0x08) {
            add(text, sizeof(text), &n, "; $FExx constant");
        }
    } else {
        static const char *const sizes[4] = { "4", "16", "64", "64" };
        add(text, sizeof(text), &n, "SAM $%04X: %s KB RAM, map type %d (%s)%s", c->sam,
            sizes[(c->sam >> 13) & 3], (c->sam >> 15) & 1, (c->sam & 0x8000) ? "all RAM" : "ROMs at $8000",
            (c->sam & 0x0400) ? ", page 1" : "");
    }
    emu_json_row(j, "Memory", text);
}

/* The VDG's mode (the CoCo 2's, or the CoCo 3's in CoCo video); its colours
 * as VDG colour numbers (n of them into colours)
 */
static void vdg_mode(char *text, size_t size, size_t *n, uint8_t pb, uint32_t video, bool coco3, uint8_t *colours,
                     int *count)
{
    bool css = pb & 0x08;
    if (pb & 0x80) {
        static const struct {
            uint16_t w, h;
            uint8_t colours;
            int8_t pmode;
            const char *name;
        } modes[8] = {
            { 64, 64, 4, -1, "CG1" }, { 128, 64, 2, -1, "RG1" }, { 128, 64, 4, -1, "CG2" }, { 128, 96, 2, 0, "RG2" },
            { 128, 96, 4, 1, "CG3" }, { 128, 192, 2, 2, "RG3" }, { 128, 192, 4, 3, "CG6" }, { 256, 192, 2, 4, "RG6" },
        };
        int m = (pb >> 4) & 7;
        add(text, size, n, "Graphics %u × %u, %u colours (%s", modes[m].w, modes[m].h, modes[m].colours, modes[m].name);
        if (modes[m].pmode >= 0) {
            add(text, size, n, ", PMODE %d", modes[m].pmode);
        }
        add(text, size, n, ")");
        if (modes[m].colours == 4) {
            for (int i = 0; i < 4; i++) {
                colours[(*count)++] = (css ? 4 : 0) + i;
            }
        } else {
            colours[(*count)++] = css ? 10 : 8;
            colours[(*count)++] = css ? 11 : 9;
        }
    } else {
        add(text, size, n, "Text 32 × 16%s, %s", coco3 ? ((pb & 0x10) ? ", lower case" : "") :
            (pb & 0x10) ? " with SG6 blocks" : " with SG4 blocks", css ? "orange" : "green");
        colours[(*count)++] = css ? 14 : 12;
        colours[(*count)++] = css ? 15 : 13;
    }
    add(text, size, n, "; screen at $%04lX", (unsigned long)video);
}

static void screen_rows(emu_json_t *j, const coco_chips_t *c, bool coco3)
{
    char text[1100];
    size_t n = 0;
    uint8_t colours[16];
    int count = 0;
    bool legacy = !coco3 || (c->gime[0] & 0x80);
    if (legacy) {
        vdg_mode(text, sizeof(text), &n, c->pia1_b, c->video, coco3, colours, &count);
        if (!coco3 && (c->pia1_b & 0xF0) == 0xF0 && s_cfg.artifacts != COCO_ARTIFACT_OFF) {
            add(text, sizeof(text), &n, "; artifact colours");
        }
    } else {
        static const uint8_t bytes_for[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
        static const uint8_t lpr[8] = { 1, 1, 2, 8, 9, 10, 11, 0 };
        uint8_t vmode = c->gime[8], vres = c->gime[9];
        unsigned lines = (vres & 0x60) == 0x20 ? 200 : (vres & 0x60) == 0x60 ? 225 : 192;
        if (vmode & 0x80) {
            unsigned bytes = bytes_for[(vres >> 2) & 7], cres = vres & 3;
            unsigned bpp = cres == 0 ? 1 : cres == 1 ? 2 : 4;
            if (bytes >= 128 && bpp == 1) {
                bpp = 2;
            }
            unsigned width = bytes * 8 / bpp, ncolours = 1u << bpp, rows = lpr[vmode & 7] ? lines / lpr[vmode & 7] : 1;
            add(text, sizeof(text), &n, "Graphics %u × %u, %u colours", width, rows, ncolours);
            int hscreen = width == 320 ? (ncolours == 4 ? 1 : ncolours == 16 ? 2 : 0) :
                          width == 640 ? (ncolours == 2 ? 3 : ncolours == 4 ? 4 : 0) : 0;
            if (hscreen && rows == 192) {
                add(text, sizeof(text), &n, " (HSCREEN %d)", hscreen);
            }
            for (unsigned i = 0; i < ncolours; i++) {
                colours[count++] = i;
            }
        } else {
            static const uint8_t cols_for[4] = { 32, 40, 64, 80 };
            unsigned cols = cols_for[((vres >> 2) & 1) | ((vres >> 3) & 2)];
            unsigned rows = lpr[vmode & 7] ? lines / lpr[vmode & 7] : 1;
            add(text, sizeof(text), &n, "Text %u × %u%s", cols, rows, (vres & 1) ? " with attributes" : "");
            if (cols == 40 || cols == 80) {
                add(text, sizeof(text), &n, " (WIDTH %u)", cols);
            }
            for (int i = 0; i < ((vres & 1) ? 16 : 2); i++) {
                colours[count++] = i;
            }
        }
        add(text, sizeof(text), &n, "; screen at $%05lX", (unsigned long)c->video);
    }
    emu_json_row(j, "Screen", text);

    /* the mode's colours: the VDG's (the CoCo 3's from its palette) */
    n = 0;
    if (coco3 && !legacy) {
        add(text, sizeof(text), &n, "Border");
        swatch(text, sizeof(text), &n, c->gime[0xA] & 0x3F);
        add(text, sizeof(text), &n, "; palette");
    } else {
        add(text, sizeof(text), &n, "Colours");
    }
    for (int i = 0; i < count; i++) {
        swatch(text, sizeof(text), &n, coco3 ? c->palette[colours[i]] & 0x3F : 64 + colours[i]);
    }
    if (!coco3 && (c->pia1_b & 0xF0) == 0xF0 && s_cfg.artifacts != COCO_ARTIFACT_OFF) {
        add(text, sizeof(text), &n, ", artifacts");
        swatch(text, sizeof(text), &n, (c->pia1_b & 0x08) ? 80 : 82);
        swatch(text, sizeof(text), &n, (c->pia1_b & 0x08) ? 81 : 83);
    }
    emu_json_row(j, "Palette", text);
}

static void sound_row(emu_json_t *j, const coco_chips_t *c)
{
    static const char *const sources[4] = { "the DAC", "the cassette", "the cartridge", "nothing" };
    char text[200];
    size_t n = 0;
    add(text, sizeof(text), &n, "%s%s; DAC %u of 63%s", c->sound_on ? "On, from " : "Off",
        c->sound_on ? sources[c->mux & 3] : "", c->dac, c->single_bit ? ", single bit high" : "");
    if (c->hz) {
        add(text, sizeof(text), &n, "; about %u Hz, loudness %u%%", c->hz, c->loudness);
    } else if (c->loudness) {
        add(text, sizeof(text), &n, "; loudness %u%%", c->loudness);
    } else {
        add(text, sizeof(text), &n, "; silent");
    }
    emu_json_row(j, "Sound", text);
}

/* The PIAs' interrupts (FS, HS on PIA0 to IRQ; CD, CART on PIA1 to FIRQ)
 * and the GIME's
 */
static void interrupts_row(emu_json_t *j, const coco_chips_t *c, bool coco3)
{
    static const char *const gime_names[6] = { "EI0", "EI1", "EI2", "VBORD", "HBORD", "TMR" };
    char text[300];
    size_t n = 0;
    bool fs = c->pia_ctl[0][1] & 1, hs = c->pia_ctl[0][0] & 1, cd = c->pia_ctl[1][0] & 1, cart = c->pia_ctl[1][1] & 1;
    add(text, sizeof(text), &n, "PIA IRQ:%s%s%s; FIRQ:%s%s%s", fs ? " FS" : "", hs ? " HS" : "",
        fs || hs ? "" : " none", cd ? " CD" : "", cart ? " CART" : "", cd || cart ? "" : " none");
    if (coco3) {
        for (int f = 0; f < 2; f++) {
            uint8_t on = (c->gime[0] & (f ? 0x10 : 0x20)) ? c->gime[2 + f] & 0x3F : 0;
            add(text, sizeof(text), &n, f ? "; FIRQ:" : "; GIME IRQ:");
            for (int i = 5; i >= 0; i--) {
                if (on & (1 << i)) {
                    add(text, sizeof(text), &n, " %s", gime_names[i]);
                }
            }
            if (!on) {
                add(text, sizeof(text), &n, " none");
            }
        }
        unsigned value = ((c->gime[4] & 15) << 8) | c->gime[5];
        if (value) {
            /* from value + 2 to 0: 63.5 us or 279 ns a count */
            unsigned long ns = (value + 2ul) * ((c->gime[1] & 0x20) ? 279ul : 63500ul);
            add(text, sizeof(text), &n, "; timer $%03X (%lu.%lu ms)", value, ns / 1000000, ns / 100000 % 10);
        }
    }
    emu_json_row(j, "Interrupts", text);
}

static void status(emu_json_t *j, int section)
{
    char text[300];
    coco_status_t s;
    coco_get_status(&s);
    coco_media_status_t m;
    coco_media_status(&m);
    if (section == EMU_STATUS_ROWS) {
        coco_chips_t c;
        coco_get_chips(&c);
        bool coco3 = s_cfg.model == COCO_MODEL_3;
        snprintf(text, sizeof(text), "Color Computer %d: a %s at %s MHz%s, %lu KB RAM, %s", coco3 ? 3 : 2,
                 s_cfg.h6309 ? "Hitachi 6309" : "Motorola 6809", s.fast ? "1.79" : "0.89",
                 s.native ? " (native)" : "", (unsigned long)s_cfg.ram_kb,
                 coco3 ? "GIME, two PIAs" : "SAM, VDG, two PIAs");
        emu_json_row(j, "System", text);
        rom_row(j, &c, coco3);
        memory_row(j, &c, coco3);
        screen_rows(j, &c, coco3);
        sound_row(j, &c);
        interrupts_row(j, &c, coco3);
        size_t n = 0;
        add(text, sizeof(text), &n, "Letters by position, symbols as typed (US); Esc or Pause BREAK, Home CLEAR, "
            "Caps Lock SHIFT+0; F5 reset");
        emu_json_row(j, "Keyboard", text);
        int x = s_mouse_x, y = s_mouse_y;
        x = (s_pad & 0x04) ? 0 : (s_pad & 0x08) ? 63 : x;
        y = (s_pad & 0x01) ? 0 : (s_pad & 0x02) ? 63 : y;
        snprintf(text, sizeof(text), "The %s one: mouse, touchpad, keypad (0, 5 button), F11 swaps; at %d, %d%s",
                 settings.joystick ? "left" : "right", x, y,
                 (s_mouse_buttons & 3) || (s_pad & 0x10) ? ", button down" : "");
        emu_json_row(j, "Joystick", text);
        n = 0;
        add(text, sizeof(text), &n, "%s; .VHD hard disk at $FF80; .CAS tapes", coco_cart_path() ?
            "A ROM cartridge in the slot" : coco_disk_controller() ? "Disk controller (WD1793): two .DSK drives" :
            "The slot empty");
        emu_json_row(j, "Disks", text);
        return;
    }
    uint64_t free_bytes, size;
    for (int d = 0; d < COCO_DRIVES; d++) {
        const char *name = disks_name(d);
        free_bytes = size = 0;
        coco_disk_space(d, &free_bytes, &size);
        snprintf(text, sizeof(text), "track %u", m.track[d]);
        emu_json_drive_space(j, drive_names[d], name, coco_disk_write_protected(d), m.light == d, m.reads[d],
                             m.writes[d], name ? text : NULL, free_bytes, size);
    }
    free_bytes = size = 0;
    coco_hd_space(&free_bytes, &size);
    snprintf(text, sizeof(text), "sector %lu", (unsigned long)m.hd_sector);
    emu_json_drive_space(j, drive_names[DRIVE_HD], disks_name(DRIVE_HD), coco_hd_write_protected(), m.hd_busy,
                         m.hd_reads, m.hd_writes, disks_name(DRIVE_HD) ? text : NULL, free_bytes, size);
    snprintf(text, sizeof(text), "%lu of %lu bytes%s", (unsigned long)m.cas_position, (unsigned long)m.cas_size,
             m.cas_recording ? ", recording" : m.cas_motor ? ", playing" : "");
    emu_json_drive(j, drive_names[DRIVE_CAS], disks_name(DRIVE_CAS), coco_cas_write_protected(), m.cas_motor, 0, 0,
                   disks_name(DRIVE_CAS) ? text : NULL);
    emu_json_drive(j, drive_names[DRIVE_CART], disks_name(DRIVE_CART), true, false, 0, 0, NULL);
}

/* ---- Console ---- */

static void console_info(char *buf, size_t len)
{
    coco_status_t s;
    coco_get_status(&s);
    snprintf(buf, len, "frames %lu PC $%04X %s%s SAM $%04X PB $%02X INIT0 $%02X VMODE $%02X VRES $%02X%s",
             (unsigned long)s.frames, s.pc, s.fast ? "1.79 MHz" : "0.89 MHz", s.native ? " native" : "", s.sam,
             s.pia1_b, s.gime[0], s.gime[8], s.gime[9], s.halted ? " HALT" : "");
}

/*   k <n>              tap a key (row * 8 + column: 1 A, 48 ENTER, 35 SPACE)
 *   K <n> <0|1>        key down (1) or up (0); 55 SHIFT
 *   T <text>           type text (\n is ENTER)
 *   j <x> <y> <b>      the joystick (0-63, buttons 1 2)
 *   W                  the reset button
 *   w                  rewind the tape
 *   S <1-8>            fast tape loading: frames a frame while a tape plays
 *   R <model> [cpu] [ram]  restart as 2 or 3; cpu 6809 or 6309; ram 128 or 512
 */
static bool console_command(const char *line)
{
    switch (line[0]) {
    case 'k': {
        int key = atoi(line + 1);
        coco_key(key, true);
        vTaskDelay(pdMS_TO_TICKS(80));
        coco_key(key, false);
        return true;
    }
    case 'K': {
        int key = 0, down = 0;
        if (sscanf(line + 1, "%d %d", &key, &down) == 2) {
            coco_key(key, down);
        }
        return true;
    }
    case 'T': {
        char text[256];
        const char *s = line + 1;
        while (*s == ' ') {
            s++;
        }
        size_t n = 0;
        for (; *s && n < sizeof(text) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') {
                text[n++] = '\n';
                s++;
            } else {
                text[n++] = *s;
            }
        }
        text[n] = 0;
        coco_type(text);
        return true;
    }
    case 'j': {
        int x = 32, y = 32, b = 0;
        sscanf(line + 1, "%d %d %d", &x, &y, &b);
        coco_joystick(settings.joystick, x, y, (uint8_t)b);
        return true;
    }
    case 'W':
        coco_warm_reset();
        return true;
    case 'w':
        coco_cas_rewind();
        return true;
    case 'S': {
        int n = atoi(line + 1);
        if (n >= 1 && n <= 8) {
            settings.tape_speed = (uint8_t)n;
            settings_save();
            coco_set_tape_speed(n);
        }
        return true;
    }
    case 'R': {
        int model = 0, cpu = settings.cpu ? 6309 : 6809, ram = settings.ram ? 512 : 128;
        int n = sscanf(line + 1, "%d %d %d", &model, &cpu, &ram);
        if (n >= 1 && (model == 2 || model == 3) && (cpu == 6809 || cpu == 6309) && (ram == 128 || ram == 512)) {
            settings.model = model == 3 ? COCO_MODEL_3 : COCO_MODEL_2;
            settings.cpu = cpu == 6309;
            settings.ram = ram == 512;
            settings_save();
            emu_restart();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ---- The machine ---- */

static const emu_machine_t machine = {
    .name = "Tandy Color Computer",
    .short_name = "CoCo",
    .view_width = COCO_SCREEN_WIDTH * 5 / 4,    /* 640 x 240: 800 x 600, 960 x 720 on the Tab5 (4:3) */
    .scale = EMU_SCALE_NEAREST,
    .reserve_memory = reserve_memory,
    .storage = storage,
    .start = start,
    .reset = coco_reset,
    .run_frame = coco_run_frame,
    .screen_mode = screen_mode,
    .screen_signature = coco_screen_signature,
    .render_line = coco_render_line_rgb565,
    .audio_hz = COCO_AUDIO_HZ,
    .audio_enable = coco_audio_enable,
    .audio_render = coco_audio_render,
    .audio_pending = coco_audio_pending,
    .hid_key = hid_key,
    .hid_mouse = hid_mouse,
    .touchpad = touchpad,
    .choices = choices,
    .choice_count = CHOICES,
    .choice_names = choice_names,
    .disks = &disks,
    .status = status,
    .console_info = console_info,
    .console_command = console_command,
    .every_second = every_second,
};

void app_main(void)
{
    for (int i = 0; i < 256; i++) {
        s_held[i].key = -1;
    }
    emu_run(&machine);
}
