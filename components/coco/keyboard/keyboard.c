/*
 * The keyboard: a matrix of 7 rows (PIA0 PA0-6, read) by 8 columns (PB0-7,
 * driven low one at a time by the ROM's scan):
 *
 *        PB0   PB1   PB2   PB3   PB4   PB5   PB6   PB7
 *   PA0  @     A     B     C     D     E     F     G
 *   PA1  H     I     J     K     L     M     N     O
 *   PA2  P     Q     R     S     T     U     V     W
 *   PA3  X     Y     Z     UP    DOWN  LEFT  RIGHT SPACE
 *   PA4  0     1     2     3     4     5     6     7
 *   PA5  8     9     :     ;     ,     -     .     /
 *   PA6  ENTER CLEAR BREAK ALT   CTRL  F1    F2    SHIFT    (ALT-F2: CoCo 3)
 *
 * Keys come from other tasks (Bluetooth, USB, the console): one byte per
 * row, read by the emulation. Typed text goes through a queue, a key every
 * few frames (Color BASIC debounces its scan).
 */
#include <string.h>
#include "coco_internal.h"

static volatile uint8_t s_rows[8];      /* pressed keys: bit = column */

#define TYPE_QUEUE      1024
static char s_queue[TYPE_QUEUE];
static volatile int s_queue_head, s_queue_tail;
static int s_type_phase;                /* frames into the current key */
static int s_type_key = -1;
static bool s_type_shift;
static bool s_type_seen;                /* the ROM scanned its column this frame */
static bool s_scanned;                  /* the ROM scanned the keyboard this frame */
static int s_scan_streak;               /* frames in a row it did */
static bool s_after_enter;              /* the last key typed was ENTER */

void keyboard_reset(void)
{
    memset((void *)s_rows, 0, sizeof(s_rows));
    s_queue_head = s_queue_tail = 0;
    s_type_key = -1;
    s_type_phase = 0;
}

void coco_key(int key, bool down)
{
    if (key < 0 || key >= COCO_KEYS) {
        return;
    }
    int row = key >> 3;
    uint8_t bit = (uint8_t)(1u << (key & 7));
    if (down) {
        s_rows[row] |= bit;
    } else {
        s_rows[row] &= (uint8_t)~bit;
    }
}

void coco_keys_up(void)
{
    memset((void *)s_rows, 0, sizeof(s_rows));
}

uint8_t COCO_HOT keyboard_rows(uint8_t columns_low)
{
    if (s_type_key >= 0 && (columns_low & (1u << (s_type_key & 7)))) {
        s_type_seen = true;
    }
    if (columns_low) {
        s_scanned = true;
    }
    uint8_t rows = 0;
    for (int r = 0; r < 7; r++) {
        if (s_rows[r] & columns_low) {
            rows |= (uint8_t)(1u << r);
        }
    }
    return rows;
}

uint8_t keyboard_columns(uint8_t rows_low)
{
    uint8_t cols = 0;
    for (int r = 0; r < 7; r++) {
        if (rows_low & (1u << r)) {
            cols |= s_rows[r];
        }
    }
    return cols;
}

bool keyboard_any(uint8_t columns_low)
{
    return keyboard_rows(columns_low) != 0;
}

/* ---- ASCII ---- */

int coco_ascii_key(char c, bool *shift)
{
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        return COCO_KEY_A + (c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return COCO_KEY_A + (c - 'a');  /* the same key (lower case is SHIFT-0's mode) */
    }
    if (c >= '0' && c <= '9') {
        return COCO_KEY_0 + (c - '0');
    }
    switch (c) {
    case '@': return COCO_KEY_AT;
    case ' ': return COCO_KEY_SPACE;
    case '\n': case '\r': return COCO_KEY_ENTER;
    case ':': return COCO_KEY_COLON;
    case ';': return COCO_KEY_SEMICOLON;
    case ',': return COCO_KEY_COMMA;
    case '-': return COCO_KEY_MINUS;
    case '.': return COCO_KEY_PERIOD;
    case '/': return COCO_KEY_SLASH;
    case '\b': return COCO_KEY_LEFT;
    default: break;
    }
    /* SHIFT + 1-9 : ; , - . / */
    static const char shifted[] = "!\"#$%&'()*+<=>?";
    static const int keys[] = {
        COCO_KEY_0 + 1, COCO_KEY_0 + 2, COCO_KEY_0 + 3, COCO_KEY_0 + 4, COCO_KEY_0 + 5,
        COCO_KEY_0 + 6, COCO_KEY_0 + 7, COCO_KEY_0 + 8, COCO_KEY_0 + 9, COCO_KEY_COLON,
        COCO_KEY_SEMICOLON, COCO_KEY_COMMA, COCO_KEY_MINUS, COCO_KEY_PERIOD, COCO_KEY_SLASH,
    };
    for (int i = 0; shifted[i]; i++) {
        if (c == shifted[i]) {
            *shift = true;
            return keys[i];
        }
    }
    switch (c) {
    case '[': *shift = true; return COCO_KEY_DOWN;      /* the CoCo 3's SHIFT-arrows */
    case ']': *shift = true; return COCO_KEY_RIGHT;
    case '\\': *shift = true; return COCO_KEY_SLASH;
    case '^': return COCO_KEY_UP;
    case '_': *shift = true; return COCO_KEY_UP;
    default: return -1;
    }
}

void coco_type(const char *text)
{
    int head = s_queue_head;
    for (; *text; text++) {
        int next = (head + 1) % TYPE_QUEUE;
        if (next == s_queue_tail) {
            break;
        }
        s_queue[head] = *text;
        head = next;
    }
    s_queue_head = head;
}

bool coco_typing(void)
{
    return s_queue_head != s_queue_tail || s_type_key >= 0;
}

/* Down for 2 frames in which the ROM scanned the key's column, up for 2
 * more (BASIC has no keyboard buffer: while it's busy, as after ENTER, it
 * doesn't scan, and the typing waits). After ENTER, the next key waits for
 * 10 frames in a row with scans: a command like SAVE checks for BREAK now
 * and then, which would take (and lose) a key typed meanwhile.
 */
void keyboard_frame(void)
{
    s_scan_streak = s_scanned ? s_scan_streak + 1 : 0;
    s_scanned = false;
    if (s_type_key >= 0) {
        bool seen = s_type_seen;
        s_type_seen = false;
        if (!seen) {
            return;
        }
        if (++s_type_phase == 2) {
            coco_key(s_type_key, false);
            if (s_type_shift) {
                coco_key(COCO_KEY_SHIFT, false);
            }
        } else if (s_type_phase >= 4) {
            s_type_key = -1;
        }
        return;
    }
    if (s_after_enter && s_scan_streak < 10) {
        return;
    }
    while (s_queue_tail != s_queue_head) {
        char c = s_queue[s_queue_tail];
        s_queue_tail = (s_queue_tail + 1) % TYPE_QUEUE;
        bool shift;
        int key = coco_ascii_key(c, &shift);
        if (key < 0) {
            continue;
        }
        s_type_key = key;
        s_type_shift = shift;
        s_type_phase = 0;
        s_type_seen = false;
        s_after_enter = key == COCO_KEY_ENTER;
        if (shift) {
            coco_key(COCO_KEY_SHIFT, true);
        }
        coco_key(key, true);
        return;
    }
}
