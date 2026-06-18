// =============================================================================
/*
 * CDTV IR Output Engine — runs on core 1
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Core 1 owns the IR LED and the frame cadence. It takes the raw input that
 * core 0 collected over Bluetooth, applies every adjustment (sensitivity,
 * direction, clamping, the CDTV axis convention) and transmits CDTV mouse and
 * joystick frames. Keeping all of this on a second core — with no Bluetooth or
 * Wi-Fi interrupts to disturb it — is what gives a smooth, glitch-free cursor.
 *
 * -------------------------------------------------------------------------
 *  CUSTOMISING FOR YOUR OWN MOUSE / TRACKBALL
 * -------------------------------------------------------------------------
 *  This project was tuned with a Logitech M575 Bluetooth trackball at its
 *  default 1000 DPI. The only setting most people need to change for a
 *  different pointing device is the sensitivity, MOUSE_SCALE_NUM / _DEN below.
 *
 *    - Pointer too fast / too sensitive  -> lower the ratio (e.g. 1/2).
 *    - Pointer too slow / sluggish       -> raise the ratio (e.g. 1/1).
 *
 *  Higher-DPI mice send larger deltas and so need a smaller ratio; lower-DPI
 *  mice need a larger one. The descriptor parser in CDTV-BLE-Mouse.c adapts to
 *  whatever report format your device advertises, so no other change is usually
 *  required. Axis direction is handled in send_mouse_frame() (see "CDTV axis
 *  convention" below) if your device tracks inverted.
 * -------------------------------------------------------------------------
 */
// =============================================================================

#include "CDTV-IR-Core1.h"
#include "CDTV-CoreLink.h"
#include "CDTV-IR-Mouse.h"
#include "CDTV-Joystick.h"

#include "pico/time.h"

// =============================================================================
// Mouse sensitivity (see "CUSTOMISING FOR YOUR OWN MOUSE" above).
//
// Each frame's accumulated movement is multiplied by NUM/DEN. It is a rational
// multiplier — applied as (delta * NUM) / DEN — so that small movements are not
// lost to integer division. 2/3 matches the feel of a real wired CDTV trackball
// for a 1000 DPI Logitech M575; pick the ratio that suits your device.
// =============================================================================
#ifndef MOUSE_SCALE_NUM
#define MOUSE_SCALE_NUM 2
#endif
#ifndef MOUSE_SCALE_DEN
#define MOUSE_SCALE_DEN 3
#endif

// =============================================================================
// Active-device lock
//
// The CDTV was designed for one pointing device at a time.  Interleaving mouse
// and joystick frames on the IR line risks violating the inter-frame silence
// the receiver needs to reset between protocols, permanently confusing its
// decoder state.
//
// The lock enforces strict mutual exclusion:
//   ACTIVE_NONE  — neither device active; whichever transmits first wins.
//   ACTIVE_MOUSE — only mouse frames sent; joystick blocked.
//   ACTIVE_JOY   — only joystick frames sent; mouse blocked.
//
// After IDLE_LOCK_US of silence from the active device the lock is released and
// both cadence timers are reset to now, so whichever device moves first wins
// cleanly with no stale catch-up frames.
// =============================================================================
typedef enum {
    ACTIVE_NONE,
    ACTIVE_MOUSE,
    ACTIVE_JOY,
} active_device_t;

// 1 second of idle releases the lock and allows the other device to take over.
#define IDLE_LOCK_US 1000000ULL

// Ignore a frame's motion if an implausible number of reports landed in it.
// Genuine movement is only 1-4 reports per frame; a much larger count is an
// artifact of a momentary stall, carrying stale zero-motion keepalives.
#define MOUSE_BURST_DISCARD_N 8

// Stop sending mouse frames after this many idle frames in a row. The CDTV does
// not need a continuous stream — it is happy for the mouse to fall silent when
// there is nothing to report.
#define MOUSE_MAX_IDLE_FRAMES 16

static int8_t clamp8(int32_t v)
{
    if (v >  127) return  127;
    if (v < -128) return -128;
    return (int8_t)v;
}

// =============================================================================
// Build and transmit one mouse frame from a raw core-link snapshot.
// Returns true if a frame was actually transmitted.
// =============================================================================
static bool emit_mouse_frame(void)
{
    corelink_mouse_t snap;
    corelink_mouse_drain(&snap);

    int16_t acc_dx  = snap.dx;
    int16_t acc_dy  = snap.dy;
    uint8_t buttons = snap.buttons;

    // Burst discard: drop motion (but keep buttons) when the report count is
    // implausibly high — those deltas are keepalive/stall artifacts, not input.
    if (snap.n_reports > MOUSE_BURST_DISCARD_N) {
        acc_dx = 0;
        acc_dy = 0;
    }

    // Cap raw accumulation before scaling so a one-off spike cannot slam the
    // cursor to the screen edge. ±127 → ±84 after the 2/3 scale.
    if (acc_dx >  127) acc_dx =  127;
    if (acc_dx < -127) acc_dx = -127;
    if (acc_dy >  127) acc_dy =  127;
    if (acc_dy < -127) acc_dy = -127;

    // Scale with int32 intermediate to avoid overflow.
    int32_t pre_dx = (int32_t)acc_dx * MOUSE_SCALE_NUM / MOUSE_SCALE_DEN;
    int32_t pre_dy = (int32_t)acc_dy * MOUSE_SCALE_NUM / MOUSE_SCALE_DEN;

    // Direction preservation: never let integer truncation drop a real movement
    // to zero — the cursor must always follow intent on small motions.
    if (pre_dx == 0 && acc_dx != 0) pre_dx = (acc_dx > 0) ? 1 : -1;
    if (pre_dy == 0 && acc_dy != 0) pre_dy = (acc_dy > 0) ? 1 : -1;

    int8_t dx = clamp8(pre_dx);
    int8_t dy = clamp8(pre_dy);

    static uint32_t idle = 0;
    bool active = (dx != 0 || dy != 0 || buttons != 0);
    if (active) idle = 0; else idle++;

    if (idle >= MOUSE_MAX_IDLE_FRAMES)
        return false;   // suppress: nothing to say

    // CDTV convention is inverted on both axes vs modern HID — negate here.
    ir_transmitter_send_mouse_frame((int8_t)-dx, (int8_t)-dy, buttons);
    return true;
}

// =============================================================================
// Core 1 entry point.
//
// Runs the active-device lock: only one protocol is on the IR line at a time.
// Whichever device produces the first frame wins the lock; the other is blocked
// until the winner has been idle for IDLE_LOCK_US (1 second), at which point
// both cadence timers are reset to now and the race begins again.
//
// Core 1 is the sole owner of the IR LED — nothing else drives it.
// =============================================================================
void ir_core1_main(void)
{
    // The IR PWM hardware is initialised by core 0 before launch (idempotent),
    // but call again here defensively — ir_transmitter_init() is safe to repeat.
    ir_transmitter_init();

    absolute_time_t next_mouse  = get_absolute_time();
    absolute_time_t next_joy    = get_absolute_time();
    absolute_time_t last_active = get_absolute_time();
    active_device_t active      = ACTIVE_NONE;

    while (true) {
        absolute_time_t now = get_absolute_time();

        // ---- Idle-lock release -------------------------------------------
        // If the active device has been silent for IDLE_LOCK_US, release the
        // lock and reset both cadence timers so the first new activity wins.
        if (active != ACTIVE_NONE &&
            absolute_time_diff_us(last_active, now) >= (int64_t)IDLE_LOCK_US) {
            active     = ACTIVE_NONE;
            next_mouse = now;
            next_joy   = now;
        }

        // ---- Mouse frame ------------------------------------------------
        // Blocked while joystick holds the lock.
        if (active != ACTIVE_JOY &&
            absolute_time_diff_us(next_mouse, now) >= 0) {

            next_mouse = delayed_by_us(next_mouse, (uint64_t)IR_FRAME_PERIOD_US);

            if (corelink_mouse_connected()) {
                bool sent = emit_mouse_frame();
                if (sent) {
                    active      = ACTIVE_MOUSE;
                    last_active = get_absolute_time();
                }
                continue;
            }
        }

        // ---- Joystick frame ---------------------------------------------
        // Blocked while mouse holds the lock.
        if (active != ACTIVE_MOUSE &&
            absolute_time_diff_us(next_joy, now) >= 0) {

            next_joy = delayed_by_us(next_joy, (uint64_t)JOYSTICK_IR_FRAME_PERIOD_US);

            uint16_t joy_bits = (uint16_t)(joystick_read_all() & 0x0FFF);
            static uint32_t joy_idle = 0;
            if (joy_bits == 0) joy_idle++; else joy_idle = 0;
            if (joy_idle < IDLE_SUPPRESS_THRESHOLD) {
                joystick_ir_send_frame(joy_bits);
                active      = ACTIVE_JOY;
                last_active = get_absolute_time();
            }
            continue;
        }

        // Nothing due yet — yield briefly.
        sleep_us(100);
    }
}