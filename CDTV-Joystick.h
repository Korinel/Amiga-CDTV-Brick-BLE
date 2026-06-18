// =============================================================================
/*
 * CDTV Joystick Input and IR Transmitter Header
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * GPIO pin map and public API for reading DB9 joystick ports and transmitting
 * CDTV CD-1252 joystick IR frames.
 *
 * BOARD_VERSION selects the GPIO layout:
 *   2 (default) — Version 2 board: UART on GP0/GP1, joystick on GP2-GP7/GP10-GP15
 *   1           — Version 1 board: joystick on GP0-GP5/GP10-GP15
 *
 * All joystick inputs are active-low with external 4k7 pull-ups (or internal
 * pull-ups enabled in software). JOY2_FIRE1 doubles as the BLE pairing trigger
 * button — sampled once at boot before BTstack initialises.
 */
// =============================================================================

#ifndef CDTV_JOYSTICK_H
#define CDTV_JOYSTICK_H

#include <stdint.h>

// =============================================================================
// Board Version GPIO Selection
// =============================================================================

#ifndef BOARD_VERSION
#define BOARD_VERSION 2
#endif

#if BOARD_VERSION == 2
// Version 2: UART on GP0/GP1; joystick port 1 on GP2-GP7
#define JOY1_UP     2
#define JOY1_DOWN   3
#define JOY1_LEFT   4
#define JOY1_RIGHT  5
#define JOY1_FIRE2  6
#define JOY1_FIRE1  7
#define LED_IR      16

#elif BOARD_VERSION == 1
// Version 1: joystick port 1 on GP0-GP5; IR LED on GP20
#define JOY1_UP     0
#define JOY1_DOWN   1
#define JOY1_LEFT   2
#define JOY1_RIGHT  3
#define JOY1_FIRE2  4
#define JOY1_FIRE1  5
#define LED_IR      20

#else
#error "Unknown BOARD_VERSION — must be 1 or 2"
#endif

// Joystick port 2 GPIO is the same for all board versions.
// JOY2_FIRE1 (GP15) doubles as the BLE pairing trigger — sampled once at boot.
#define JOY2_UP     10
#define JOY2_DOWN   11
#define JOY2_LEFT   12
#define JOY2_RIGHT  13
#define JOY2_FIRE2  14
#define JOY2_FIRE1  15

// =============================================================================
// Joystick IR Timing Constants
// =============================================================================

// Idle suppression: suppress IR after this many consecutive all-idle frames
#define IDLE_SUPPRESS_THRESHOLD 4

// Frame period exported for use by main.c timer setup
extern const uint64_t JOYSTICK_IR_FRAME_PERIOD_US;

// =============================================================================
// Public API
// =============================================================================

/**
 * Initialise all joystick GPIO pins (both ports) as inputs with pull-ups.
 * Must be called before boot_sample_fire_button() and before BTstack init.
 */
void joystick_init(void);

/**
 * Atomically read all joystick GPIO inputs.
 * Returns a bitmask with active-low inversion applied and non-joystick bits
 * masked to zero. Bit layout (matches the CDTV CD-1252 protocol bit ordering):
 *   bits [5:0]  = JOY2 (UP, DOWN, LEFT, RIGHT, FIRE1, FIRE2)
 *   bits [11:6] = JOY1 (UP, DOWN, LEFT, RIGHT, FIRE1, FIRE2)
 */
uint32_t joystick_read_all(void);

/**
 * Encode and transmit one 25-bit CDTV joystick IR frame.
 * @param joy_bits  12-bit joystick state (bits [5:0]=JOY2, bits [11:6]=JOY1)
 *
 * Frame layout (MSB first, bit 24 first on wire):
 *   [24]     Identifier = 0 (sent first; mouse frames start with 1)
 *   [23:12]  12 data bits (joy_bits, bit 11 first)
 *   [11:0]   12 inverted check bits (~joy_bits & 0xFFF, bit 11 first)
 */
void joystick_ir_send_frame(uint16_t joy_bits);

#endif // CDTV_JOYSTICK_H