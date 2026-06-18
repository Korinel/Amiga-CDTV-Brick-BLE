// =============================================================================
/*
 * CDTV IR Mouse Transmitter Implementation
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Encodes and transmits CDTV CD-1252 IR mouse frames at 40 kHz.
 * Uses the shared CDTV-IR-PWM driver for carrier generation.
 *
 * IR encoding (hardware-verified against CD1200 captures, U75 firmware)
 * -----------------------------------------------------------------------
 *   Carrier:   40 kHz, 33% duty
 *   Header:    1100 µs mark + 375 µs space
 *   Bit '1':    500 µs mark + 375 µs space  (long mark)
 *   Bit '0':    150 µs mark + 725 µs space  (short mark)
 *   Stop bit:   100 µs mark
 *   Inter-frame gap: 13844 µs (hardware-verified)
 *   19 data bits, MSB-first:
 *     [18]    Mouse flag (always 1)
 *     [17]    RIGHT button, active-low (0 = pressed)
 *     [16]    LEFT  button, active-low (0 = pressed)
 *     [15:8]  X delta, signed, positive = left
 *     [7:0]   Y delta, signed, positive = up
 *
 * CDTV axis convention:
 *   The CDTV is inverted on both axes relative to a modern USB/Bluetooth mouse:
 *   on the CDTV, positive X means LEFT and positive Y means UP. The caller is
 *   responsible for that conversion — the core 1 engine negates both axes
 *   before calling here (see emit_mouse_frame in CDTV-IR-Core1.c). If your
 *   device tracks the wrong way, that negation is the place to adjust.
 */
// =============================================================================

#include "CDTV-IR-Mouse.h"
#include "CDTV-IR-PWM.h"
#include <stdio.h>
#include "pico/stdlib.h"

// =============================================================================
// IR Protocol Timing (µs) — hardware-verified against CD1200 and CD1253
// =============================================================================
#define T_HDR_MARK_US    1100u
#define T_HDR_SPACE_US    375u
#define T_ZERO_MARK_US    150u
#define T_ZERO_SPACE_US   725u
#define T_ONE_MARK_US     500u
#define T_ONE_SPACE_US    375u
#define T_STOP_MARK_US    100u
#define T_GAP_US        13844u

// Frame period: header + 19 worst-case (all-zero) bits + stop + gap.
const uint64_t IR_FRAME_PERIOD_US =
    (uint64_t)T_HDR_MARK_US  + T_HDR_SPACE_US +
    (19ULL * (T_ZERO_MARK_US + T_ZERO_SPACE_US)) +
    T_STOP_MARK_US + T_GAP_US;

// =============================================================================
// Public Functions
// =============================================================================

bool ir_transmitter_init(void) {
    // Initialise the shared PWM hardware (idempotent)
    ir_pwm_init();
    printf("IR: init OK  frame_period=%llu us\n",
           (unsigned long long)IR_FRAME_PERIOD_US);
    return true;
}

void ir_transmitter_send_mouse_frame(int8_t dx, int8_t dy, uint8_t buttons) {
    bool left  = (buttons & 0x01) != 0;
    bool right = (buttons & 0x02) != 0;

    uint32_t payload =
        (1u                    << 18) |
        ((right ? 0u : 1u)     << 17) |
        ((left  ? 0u : 1u)     << 16) |
        ((uint32_t)(uint8_t)dx <<  8) |
        ((uint32_t)(uint8_t)dy <<  0);

    // Runs on core 1, which has no CYW43/BLE background interrupts, so no
    // interrupt protection is needed around the busy-wait timing — matching the
    // proven PS/2 trackball reference, which disables nothing. Core 1 is the
    // sole owner of the IR LED and the only thing executing on this core during
    // a frame, so marks cannot be stretched by competing work.
    ir_pwm_emit(T_HDR_MARK_US, T_HDR_SPACE_US);

    for (int b = 18; b >= 0; b--) {
        if ((payload >> b) & 1u)
            ir_pwm_emit(T_ONE_MARK_US,  T_ONE_SPACE_US);
        else
            ir_pwm_emit(T_ZERO_MARK_US, T_ZERO_SPACE_US);
    }

    ir_pwm_mark_only(T_STOP_MARK_US);

    busy_wait_us_32(T_GAP_US);
}

void ir_transmitter_selftest(void) {
    printf("IR: self-test — transmitting 5 idle frames...\n");
    for (int i = 0; i < 5; i++) {
        ir_transmitter_send_mouse_frame(0, 0, 0);
        printf("  IR frame %d sent\n", i + 1);
    }
    printf("IR: self-test done\n");
}

// -----------------------------------------------------------------------------
// Optional hardware bring-up diagnostics. Neither is called in normal operation;
// they are provided for verifying a new build against a real CDTV without a
// Bluetooth device attached. Call one from main() before launching core 1.
//
//   ir_transmitter_test_square_motion() — drives the pointer in a continuous
//   square. The single most useful sanity check: if the square is smooth on the
//   CDTV, the IR timing, encoding, LED and receiver are all good.
//
//   ir_transmitter_test_mouse_limits() — steps through increasing deltas to
//   explore how the CDTV responds to small vs large movements.
// -----------------------------------------------------------------------------
void ir_transmitter_test_square_motion(void) {
    const int side_pixels = 240;
    const int step_pixels = 20;
    const int steps = side_pixels / step_pixels;
    printf("IR: square motion test\n");
    while (true) {
        for (int i = 0; i < steps; i++) ir_transmitter_send_mouse_frame( step_pixels,           0, 0);
        for (int i = 0; i < steps; i++) ir_transmitter_send_mouse_frame(           0,  step_pixels, 0);
        for (int i = 0; i < steps; i++) ir_transmitter_send_mouse_frame(-step_pixels,           0, 0);
        for (int i = 0; i < steps; i++) ir_transmitter_send_mouse_frame(           0, -step_pixels, 0);
        printf("IR: square lap complete\n");
    }
}

void ir_transmitter_test_mouse_limits(void) {
    printf("\n=== CDTV Mouse Limits Test ===\n");
    sleep_ms(5000);
    const int8_t test_deltas[] = {1, 2, 5, 10, 20, 50, 100, 127};
    for (int i = 0; i < 8; i++) {
        int8_t d = test_deltas[i];
        for (int f = 0; f < 10; f++) ir_transmitter_send_mouse_frame(d, 0, 0);
        sleep_ms(500);
        for (int f = 0; f < 10; f++) ir_transmitter_send_mouse_frame(-d, 0, 0);
        sleep_ms(5000);
    }
    ir_transmitter_send_mouse_frame(127, 127, 0);  sleep_ms(5000);
    ir_transmitter_send_mouse_frame(-127, -127, 0); sleep_ms(5000);
    for (int f = 0; f < 32; f++) ir_transmitter_send_mouse_frame(127, 0, 0);
    sleep_ms(5000);
    for (int f = 0; f < 32; f++) ir_transmitter_send_mouse_frame(-127, 0, 0);
    sleep_ms(5000);
    printf("\n=== Test Complete ===\n");
}