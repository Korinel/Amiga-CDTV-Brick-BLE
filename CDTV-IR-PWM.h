// =============================================================================
/*
 * Shared IR PWM Driver
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Owns the IR LED PWM hardware (LED_IR pin) and produces the modulated 40 kHz
 * carrier. Both the mouse transmitter (CDTV-IR-Mouse.c) and the joystick
 * transmitter (CDTV-Joystick.c) drive the same physical LED through this driver.
 *
 * Single-owner guarantee: all IR transmission happens on core 1 (see
 * CDTV-IR-Core1.c), which is the only code that ever calls into this driver and
 * serialises mouse and joystick frames so they never overlap on the wire. Marks
 * and spaces are produced with busy_wait_us_32(); because core 1 carries no
 * Bluetooth/Wi-Fi interrupts, that timing is clean and needs no locking.
 */
// =============================================================================

#ifndef CDTV_IR_PWM_H
#define CDTV_IR_PWM_H

#include <stdint.h>

/** Initialise the IR PWM hardware. Idempotent — safe to call multiple times. */
void ir_pwm_init(void);

/** Emit carrier for mark_us, then silence for space_us. */
void ir_pwm_emit(uint32_t mark_us, uint32_t space_us);

/** Emit carrier for mark_us then leave carrier off (stop bit). */
void ir_pwm_mark_only(uint32_t mark_us);

#endif // CDTV_IR_PWM_H