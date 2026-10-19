#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "coco.h"

/* Machine settings kept in NVS (namespace "coco") */
typedef struct {
    uint8_t model;          /* coco_model_t */
    uint8_t cpu;            /* 0 Motorola 6809, 1 Hitachi 6309 */
    uint8_t ram;            /* CoCo 3: 0 128 KB, 1 512 KB */
    uint8_t monitor;        /* CoCo 3: coco_monitor_t */
    uint8_t artifacts;      /* CoCo 2: 0 blue-red, 1 red-blue, 2 none */
    uint8_t joystick;       /* the joystick the mouse, touch and keypad move: 0 right, 1 left */
    uint8_t tape_speed;     /* 1-8: frames a frame while a tape plays */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);
