/*
 * Tandy Color Computer 2 and 3 emulator on the Makerfabs MaTouch ESP32-S3
 * 7" or the M5Stack Tab5.
 *
 * The board side (tasks, LCD, touch, WiFi, Bluetooth, USB, setup screen,
 * web page, console) is the shared runtime in ../esp32emu; this file plugs
 * the CoCo into it: memory, ROMs, settings, keyboard map, joystick, the
 * drives (two floppies, the hard disk, the cassette, the cartridge), status.
 */
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
static char s_rom_text[128];
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
    emu_set_drive_info(sd_card ? "Disks, tapes and cartridges in coco/ on the SD card" : "No SD card: no disks");
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
            emu_set_message("No CoCo 3 ROM: put coco3.rom into roms/ and flash with idf.py flash, "
                            "or copy it into coco/roms on the SD card.");
            return false;
        }
        s_cfg.coco3_rom = s_rom[ROM_COCO3];
    } else {
        if (!load(ROM_BASIC)) {
            emu_set_message("No Color BASIC ROM: put bas13.rom into roms/ and flash with idf.py flash, "
                            "or copy it into coco/roms on the SD card.");
            return false;
        }
        s_cfg.basic = s_rom[ROM_BASIC];
        s_cfg.extended = load(ROM_EXTENDED) ? s_rom[ROM_EXTENDED] : NULL;
    }
    s_cfg.disk_rom = load(ROM_DISK) ? s_rom[ROM_DISK] : NULL;
    if (coco3) {
        snprintf(s_rom_text, sizeof(s_rom_text), "%s%s%s", rom_store_name(ROM_COCO3),
                 s_cfg.disk_rom ? ", " : " (no Disk BASIC)", s_cfg.disk_rom ? rom_store_name(ROM_DISK) : "");
    } else {
        snprintf(s_rom_text, sizeof(s_rom_text), "%s%s%s%s%s", rom_store_name(ROM_BASIC),
                 s_cfg.extended ? ", " : " (no Extended BASIC)", s_cfg.extended ? rom_store_name(ROM_EXTENDED) : "",
                 s_cfg.disk_rom ? ", " : " (no Disk BASIC)", s_cfg.disk_rom ? rom_store_name(ROM_DISK) : "");
    }
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

static bool accepts(int drive, const char *name)
{
    static const char *const floppy[] = { ".dsk", ".os9", NULL };
    static const char *const hd[] = { ".vhd", NULL };
    static const char *const tape[] = { ".cas", NULL };
    static const char *const cart[] = { ".rom", ".ccc", ".bin", NULL };
    switch (drive) {
    case DRIVE_HD: return has_ext(name, hd);
    case DRIVE_CAS: return has_ext(name, tape);
    case DRIVE_CART: return has_ext(name, cart);
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

/* ---- Status ---- */

static void status(emu_json_t *j, int section)
{
    char text[160];
    coco_status_t s;
    coco_get_status(&s);
    coco_media_status_t m;
    coco_media_status(&m);
    if (section == EMU_STATUS_ROWS) {
        snprintf(text, sizeof(text), "%s, %s, %lu KB", s_cfg.model == COCO_MODEL_3 ? "Color Computer 3" : "Color Computer 2",
                 s_cfg.h6309 ? "Hitachi 6309" : "Motorola 6809", (unsigned long)s_cfg.ram_kb);
        emu_json_row(j, "System", text);
        emu_json_row(j, "ROMs", s_rom_text);
        snprintf(text, sizeof(text), "%s%s", s.fast ? "1.79 MHz" : "0.89 MHz", s.native ? ", 6309 native mode" : "");
        emu_json_row(j, "CPU", text);
        snprintf(text, sizeof(text), "the %s one: mouse, touchpad and the numeric keypad (F11 switches)",
                 settings.joystick ? "left" : "right");
        emu_json_row(j, "Joystick", text);
        emu_json_row(j, "Slot", coco_cart_path() ? "ROM cartridge" : coco_disk_controller() ? "Disk controller"
                                                                                             : "empty");
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
