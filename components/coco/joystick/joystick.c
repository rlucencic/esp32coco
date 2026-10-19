/*
 * Two analogue joysticks. The ROM reads a pot by setting the DAC and
 * reading the comparator on PIA0 PA7 (pot >= DAC), the analogue mux (PIA0
 * CA2, CB2) choosing right X, right Y, left X, left Y. The first buttons
 * pull PA0 (right) and PA1 (left) low, the CoCo 3's second buttons PA2 and
 * PA3.
 */
#include "coco_internal.h"

static volatile uint8_t s_axis[4] = { 32, 32, 32, 32 };    /* right x y, left x y */
static volatile uint8_t s_buttons[2];

void joystick_reset(void)
{
}

void coco_joystick(int port, int x, int y, uint8_t buttons)
{
    if (port < 0 || port > 1) {
        return;
    }
    s_axis[port * 2] = (uint8_t)(x < 0 ? 0 : x > 63 ? 63 : x);
    s_axis[port * 2 + 1] = (uint8_t)(y < 0 ? 0 : y > 63 ? 63 : y);
    s_buttons[port] = buttons & 3;
}

bool joystick_comparator(int mux, int dac)
{
    return s_axis[mux & 3] >= dac;
}

uint8_t joystick_buttons(void)
{
    uint8_t b = 0;
    if (s_buttons[0] & 1) b |= 0x01;
    if (s_buttons[1] & 1) b |= 0x02;
    if (s_buttons[0] & 2) b |= 0x04;
    if (s_buttons[1] & 2) b |= 0x08;
    return b;
}
