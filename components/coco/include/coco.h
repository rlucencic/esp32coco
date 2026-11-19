/*
 * Tandy Color Computer 2 and 3.
 *
 *   CoCo 2: a 6809E at 0.89 MHz (1.79 with the speed-up poke), 64 KB, the
 *           MC6883 SAM (memory map, video address counter, CPU rate), the
 *           MC6847 VDG (text, semigraphics, the eight graphics modes, PMODE 4
 *           artifact colours), two MC6821 PIAs (keyboard, joysticks, the
 *           6-bit DAC and single-bit sound, cassette, VDG mode, interrupts)
 *   CoCo 3: a 68B09E (0.89 or 1.79 MHz), 128 or 512 KB, the GIME: an MMU of
 *           8 KB blocks in two tasks, 64 colours in 16 palette registers (RGB
 *           or composite), its text (32-80 columns, attributes) and graphics
 *           modes (up to 640 x 225), the CoCo 2's video for compatibility,
 *           its timer and interrupts (IRQ and FIRQ), the enhanced keyboard
 *
 * Either can have a Hitachi 6309 instead (esp32emu's 6809 core does both):
 * its emulation mode runs everything a 6809 does, and software that knows it
 * (NitrOS-9's 6309 version, programs that switch it to native mode) gets
 * its extra registers, instructions and faster native mode.
 *
 * Both: a cartridge slot (ROM cartridges, autostarting through the CART
 * line, or the disk controller: Disk BASIC and a WD1793 with two drives of
 * .DSK images), a cassette (.CAS: played into the cassette input, CSAVE
 * recorded from the DAC), two analogue joysticks, and a hard disk through
 * the VHD interface at $FF80 (the one NitrOS-9's emudsk driver uses).
 *
 * Platform independent: the caller provides the ROMs and the big buffers
 * and drives the emulation one video frame (262 lines, 59.92 Hz) at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COCO_MODEL_2,               /* 64 KB */
    COCO_MODEL_3,               /* 128 or 512 KB */
    COCO_MODEL_COUNT
} coco_model_t;

/* The CoCo 3's monitor: the CM-8 (RGB) or a TV (composite) */
typedef enum {
    COCO_MONITOR_RGB,
    COCO_MONITOR_COMPOSITE,
} coco_monitor_t;

/* The CoCo 2's PMODE 4 on a TV: pairs of pixels as colours, in either
 * phase (a real CoCo starts in one or the other)
 */
typedef enum {
    COCO_ARTIFACT_OFF,
    COCO_ARTIFACT_BLUE_RED,
    COCO_ARTIFACT_RED_BLUE,
} coco_artifact_t;

/* The picture: 640 x 240, 24 border lines above and below the 192 lines of
 * the CoCo 2's picture (the CoCo 3's 200 and 225 line modes use more of
 * them), 64 pixels of border left and right of its 512 (the CoCo 3's wide
 * modes fill all 640). Each pixel is a colour number: 0-63 the CoCo 3's
 * colours, 64-79 the VDG's, 80-83 its artifact colours.
 */
#define COCO_SCREEN_WIDTH       640
#define COCO_SCREEN_HEIGHT      240
#define COCO_COLOURS            84

/* Two samples a scan line: 262 lines at 59.92 Hz */
#define COCO_AUDIO_HZ           31399

typedef struct {
    coco_model_t model;
    bool h6309;                 /* a Hitachi 6309 instead of the 6809 (the common upgrade) */
    uint32_t ram_kb;            /* CoCo 2: 64; CoCo 3: 128 or 512 */
    coco_monitor_t monitor;     /* CoCo 3 */
    coco_artifact_t artifacts;  /* CoCo 2 */
    uint8_t *ram;               /* ram_kb */
    const uint8_t *basic;       /* CoCo 2: Color BASIC 1.2 or 1.3, 8 KB */
    const uint8_t *extended;    /* CoCo 2: Extended Color BASIC 1.1, 8 KB (NULL: none) */
    const uint8_t *coco3_rom;   /* CoCo 3: its 32 KB ROM */
    const uint8_t *disk_rom;    /* Disk BASIC 1.0 or 1.1, 8 KB (NULL: no disk controller) */
    uint8_t *frame;             /* COCO_SCREEN_WIDTH x COCO_SCREEN_HEIGHT colour numbers */
} coco_config_t;

typedef struct {
    uint32_t frames;
    uint16_t pc;
    bool native;                /* a 6309 in native mode */
    bool fast;                  /* 1.79 MHz */
    uint16_t sam;               /* the SAM's 16 bits */
    uint8_t pia1_b;             /* the VDG's mode lines (CoCo 2) */
    uint8_t gime[16];           /* $FF90-$FF9F (CoCo 3) */
    uint8_t mmu[16];            /* $FFA0-$FFAF */
    bool halted;                /* the disk controller holds HALT */
} coco_status_t;

bool coco_init(const coco_config_t *config);
void coco_reset(void);          /* power off and on */
void coco_warm_reset(void);     /* the reset button (at the next frame) */
void coco_run_frame(void);
void coco_get_status(coco_status_t *status);

/* The chips as they are now, for the web page's status: plain copies of
 * their registers (any task may ask; a few cycles old at worst)
 */
typedef struct {
    uint16_t sam;               /* the SAM's 16 bits (the GIME keeps them too) */
    uint8_t pia_ctl[2][2];      /* PIA0 and PIA1 CRA, CRB (bit 0: the interrupt on) */
    uint8_t pia1_b;             /* the VDG's mode lines (PIA1 PB) */
    uint32_t video;             /* the picture's address as this frame began (CoCo 3; CoCo 2: the SAM's F bits) */
    uint8_t gime[16];           /* CoCo 3: $FF90-$FF9F */
    uint8_t mmu[16];            /* $FFA0-$FFAF */
    uint8_t palette[16];        /* $FFB0-$FFBF */
    /* the sound */
    uint8_t dac;                /* the 6-bit DAC (PIA1 PA7-2) */
    bool sound_on;              /* SNDEN (PIA1 CB2) */
    uint8_t mux;                /* the source (PIA0 CA2, CB2): 0 the DAC, 1 the cassette, 2 the cartridge, 3 none */
    bool single_bit;            /* PIA1 PB1, an output, high */
    uint16_t hz;                /* the sound played over the last ~65 ms: its pitch (0: none) */
    uint8_t loudness;           /* and its loudness, 0-100 */
} coco_chips_t;

void coco_get_chips(coco_chips_t *chips);
uint8_t coco_peek(uint16_t address);    /* as the CPU sees it, no side effects */
void coco_set_monitor(coco_monitor_t monitor);
void coco_set_artifacts(coco_artifact_t artifacts);

/* ---- Screen ---- */

int coco_screen_hz(void);       /* 60 */
void coco_render_line_rgb565(int y, uint16_t *dst);
uint32_t coco_screen_signature(void);
/* RGB565 of the colour numbers (COCO_COLOURS entries) */
const uint16_t *coco_palette(void);

/* ---- Sound: 16-bit stereo, the same in both channels ---- */

void coco_audio_enable(bool enable);
int coco_audio_render(int16_t *dst, int frames);
int coco_audio_pending(void);

/* ---- Keyboard: safe from another task ----
 *
 * Keys are matrix positions, row * 8 + column (the row read on PIA0 PA0-6,
 * the column driven on PB0-7): @ A-G, H-O, P-W, X Y Z and the arrows and
 * space, 0-7, 8 9 : ; , - . /, and ENTER CLEAR BREAK ALT CTRL F1 F2 SHIFT
 * (ALT, CTRL, F1 and F2 the CoCo 3's).
 */
#define COCO_KEY(row, col)      ((row) * 8 + (col))
enum {
    COCO_KEY_AT = COCO_KEY(0, 0),
    COCO_KEY_A = COCO_KEY(0, 1),            /* ... Z = A + 25 */
    COCO_KEY_UP = COCO_KEY(3, 3), COCO_KEY_DOWN, COCO_KEY_LEFT, COCO_KEY_RIGHT, COCO_KEY_SPACE,
    COCO_KEY_0 = COCO_KEY(4, 0),            /* ... 7 = 0 + 7, 8 9 follow */
    COCO_KEY_COLON = COCO_KEY(5, 2), COCO_KEY_SEMICOLON, COCO_KEY_COMMA, COCO_KEY_MINUS,
    COCO_KEY_PERIOD, COCO_KEY_SLASH,
    COCO_KEY_ENTER = COCO_KEY(6, 0), COCO_KEY_CLEAR, COCO_KEY_BREAK, COCO_KEY_ALT, COCO_KEY_CTRL,
    COCO_KEY_F1, COCO_KEY_F2, COCO_KEY_SHIFT,
    COCO_KEYS = 56
};
void coco_key(int key, bool down);
void coco_keys_up(void);

/* Type text: one key every few frames, as the ROM scans them. ASCII ('\n'
 * is ENTER; lower case as upper case), after what's still queued.
 */
void coco_type(const char *text);
bool coco_typing(void);

/* The key for an ASCII character (*shift: with SHIFT); -1 if none */
int coco_ascii_key(char c, bool *shift);

/* ---- Joysticks: 0 right, 1 left; x and y 0-63; buttons bit 0 the first,
 * bit 1 the second (the CoCo 3's two-button joysticks)
 */
void coco_joystick(int port, int x, int y, uint8_t buttons);

/* ---- Media: not while coco_run_frame() runs ----
 *
 * Cartridge: a ROM image (.ROM .CCC .BIN, up to 32 KB) in the slot instead
 * of the disk controller; it starts by itself (the CART line) at the next
 * reset. Cassette: .CAS, played when the motor runs and the CoCo reads it,
 * recorded at the tape position when it writes (CSAVE). Floppies: .DSK
 * (JVC: 256-byte sectors, 18 a track, an optional header), read and
 * written through to the file. Hard disk: a VHD image, 256-byte sectors.
 */
bool coco_cart_insert(const char *path);
void coco_cart_eject(void);
const char *coco_cart_path(void);       /* NULL when empty */
bool coco_disk_controller(void);        /* the disk controller is in the slot */

bool coco_cas_insert(const char *path);
void coco_cas_eject(void);
const char *coco_cas_path(void);
bool coco_cas_write_protected(void);
void coco_cas_rewind(void);
bool coco_cas_create(const char *path);

#define COCO_DRIVES             2
bool coco_disk_insert(int drive, const char *path);
void coco_disk_eject(int drive);
const char *coco_disk_path(int drive);
bool coco_disk_write_protected(int drive);
/* A blank (formatted, empty) disk for Disk BASIC: 35 tracks single sided
 * (156 KB), 40 tracks or 80 tracks double sided (360 or 720 KB)
 */
#define COCO_DISK_35_BYTES      (35 * 18 * 256)
#define COCO_DISK_40DS_BYTES    (40 * 2 * 18 * 256)
#define COCO_DISK_80DS_BYTES    (80 * 2 * 18 * 256)
bool coco_disk_create(const char *path, uint32_t bytes);

bool coco_hd_insert(const char *path);
void coco_hd_eject(void);
const char *coco_hd_path(void);
bool coco_hd_write_protected(void);
bool coco_hd_create(const char *path, uint32_t bytes);
/* A disk's free space and size in bytes, as its file system counts them
 * (OS-9's bitmap, Disk BASIC's granule table); worked out at insert and
 * flush (any task may ask). False if not known
 */
bool coco_disk_space(int drive, uint64_t *free_bytes, uint64_t *size);
bool coco_hd_space(uint64_t *free_bytes, uint64_t *size);

typedef struct {
    int light;                  /* floppy drive busy, -1 none */
    uint32_t reads[COCO_DRIVES], writes[COCO_DRIVES];   /* sectors */
    uint8_t track[COCO_DRIVES];
    bool motor;                 /* the floppy motor */
    bool cas_motor, cas_recording;
    uint32_t cas_position, cas_size;    /* bytes */
    bool hd_busy;
    uint32_t hd_reads, hd_writes;
    uint32_t hd_sector;         /* the last one accessed */
} coco_media_status_t;

void coco_media_status(coco_media_status_t *status);

/* Floppy and cassette writes are kept in memory, the hard disk's in the
 * file's buffers: write them out (not while coco_run_frame() runs)
 */
bool coco_media_dirty(void);
void coco_media_flush(void);

/* While a tape plays, run this many frames for each (1: real time); the
 * extra frames are silent
 */
void coco_set_tape_speed(int factor);
