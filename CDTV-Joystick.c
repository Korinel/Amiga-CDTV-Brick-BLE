// =============================================================================
/*
 * CDTV Joystick Input and IR Transmitter Implementation
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Reads two DB9 joystick ports via GPIO and transmits CDTV CD-1252 joystick
 * IR frames. Uses the shared CDTV-IR-PWM driver for carrier generation.
 *
 * Sequencing: the core 1 output engine (CDTV-IR-Core1.c) is the only caller of
 * joystick_ir_send_frame() and interleaves it with mouse frames, so the two
 * never overlap on the wire. This file just reads the ports and encodes a frame.
 *
 * IR frame format (25 bits, MSB first):
 *   [24:13]  12 data bits  (joystick state)
 *   [12:1]   12 check bits (bitwise NOT of data bits)
 *   [0]      Identifier = 0 (joystick; mouse frames use 1)
 */
// =============================================================================

#include "CDTV-Joystick.h"
#include "CDTV-IR-PWM.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

// =============================================================================
// IR Protocol Timing (µs) — from Brick firmware, hardware-verified
// =============================================================================
#define JOY_HDR_MARK_US    1100u
#define JOY_HDR_SPACE_US    375u
#define JOY_ZERO_MARK_US    150u
#define JOY_ZERO_SPACE_US   725u
#define JOY_ONE_MARK_US     500u
#define JOY_ONE_SPACE_US    375u
#define JOY_GAP_US          800u

// Frame period: header + 13 zero bits (all-zero data) + 12 one bits (inverted check) + gap.
// Using average case (not worst case) so the timer period matches actual TX time.
// Worst-case (all zeros) would fire the timer before the frame finishes → double sends.
const uint64_t JOYSTICK_IR_FRAME_PERIOD_US =
    (uint64_t)JOY_HDR_MARK_US + JOY_HDR_SPACE_US +
    (13ULL * (JOY_ZERO_MARK_US + JOY_ZERO_SPACE_US)) +
    (12ULL * (JOY_ONE_MARK_US  + JOY_ONE_SPACE_US))  +
    JOY_GAP_US;

// =============================================================================
// GPIO Pin Table — bit ordering matches the CDTV CD-1252 joystick protocol.
// JOY2 in bits [11:6] (high), JOY1 in bits [5:0] (low), MSB first.
// =============================================================================
static const uint8_t joystick_gpio_pins[] = {
    JOY2_UP, JOY2_DOWN, JOY2_LEFT, JOY2_RIGHT, JOY2_FIRE1, JOY2_FIRE2,
    JOY1_UP, JOY1_DOWN, JOY1_LEFT, JOY1_RIGHT, JOY1_FIRE1, JOY1_FIRE2,
};
static const size_t NUM_JOYSTICK_PINS =
    sizeof(joystick_gpio_pins) / sizeof(joystick_gpio_pins[0]);

// =============================================================================
// Public Functions
// =============================================================================

void joystick_init(void) {
    for (size_t i = 0; i < NUM_JOYSTICK_PINS; i++) {
        uint8_t pin = joystick_gpio_pins[i];
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
    // PWM init is handled by ir_transmitter_init() in CDTV-IR-Mouse.c.
    // joystick_init() is called before ir_transmitter_init(), so we call
    // ir_pwm_init() here too — it is idempotent and safe to call multiple times.
    ir_pwm_init();
}

uint32_t joystick_read_all(void) {
    uint32_t raw    = ~gpio_get_all();  // invert: active-low inputs
    uint32_t result = 0;
    for (size_t i = 0; i < NUM_JOYSTICK_PINS; i++) {
        if (raw & (1u << joystick_gpio_pins[i]))
            result |= (1u << i);
    }
    return result;
}

void joystick_ir_send_frame(uint16_t joy_bits) {
    uint16_t data  = joy_bits & 0x0FFF;
    uint16_t check = (~data) & 0x0FFF;

    // Frame structure: header → identifier bit (0) → 12 data bits MSB-first → 12 check bits MSB-first
    ir_pwm_emit(JOY_HDR_MARK_US, JOY_HDR_SPACE_US);

    // Identifier bit: 0 = joystick (mouse uses 1)
    ir_pwm_emit(JOY_ZERO_MARK_US, JOY_ZERO_SPACE_US);

    // 12 data bits, MSB first (bit 11 down to bit 0)
    for (int b = 11; b >= 0; b--) {
        if ((data >> b) & 1u)
            ir_pwm_emit(JOY_ONE_MARK_US,  JOY_ONE_SPACE_US);
        else
            ir_pwm_emit(JOY_ZERO_MARK_US, JOY_ZERO_SPACE_US);
    }

    // 12 check bits (inverted data), MSB first
    for (int b = 11; b >= 0; b--) {
        if ((check >> b) & 1u)
            ir_pwm_emit(JOY_ONE_MARK_US,  JOY_ONE_SPACE_US);
        else
            ir_pwm_emit(JOY_ZERO_MARK_US, JOY_ZERO_SPACE_US);
    }

    // Inter-frame gap
    busy_wait_us_32(JOY_GAP_US);
}