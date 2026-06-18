// =============================================================================
/*
 * CDTV IR Mouse Transmitter Header
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 */
// =============================================================================

#ifndef IR_TRANSMITTER_H
#define IR_TRANSMITTER_H

#include <stdint.h>
#include <stdbool.h>

// Frame period in microseconds, computed from the protocol timing constants.
// Core 1 uses this to pace mouse frames (see CDTV-IR-Core1.c).
extern const uint64_t IR_FRAME_PERIOD_US;

// =============================================================================
// Public Functions
// =============================================================================

/** Initialize IR transmitter hardware (PWM, GPIO). @return true on success. */
bool ir_transmitter_init(void);

/**
 * Transmit one CDTV mouse IR frame.
 * @param dx  X delta, signed. Positive = left on CDTV screen.
 * @param dy  Y delta, signed. Positive = up   on CDTV screen.
 * @param buttons  Bitmask: bit0=left, bit1=right (active-high, inverted internally).
 *
 * Includes the inter-frame gap (13844 µs) after the stop bit.
 * Total call duration matches IR_FRAME_PERIOD_US at worst case.
 */
void ir_transmitter_send_mouse_frame(int8_t dx, int8_t dy, uint8_t buttons);

/**
 * Transmit 5 idle frames to verify IR circuit before BLE init.
 * No motion, no buttons — just the mouse flag bit.
 */
void ir_transmitter_selftest(void);

/** Test: continuous square motion pattern. Does not return. */
void ir_transmitter_test_square_motion(void);

/** Test: evaluate movement limits and delta magnitudes. */
void ir_transmitter_test_mouse_limits(void);

#endif // IR_TRANSMITTER_H