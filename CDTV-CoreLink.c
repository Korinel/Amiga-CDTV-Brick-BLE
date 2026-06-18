// =============================================================================
/*
 * CDTV Core Link — implementation
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * See CDTV-CoreLink.h for the design contract.
 */
// =============================================================================

#include "CDTV-CoreLink.h"
#include "pico/critical_section.h"
#include <stdatomic.h>

// critical_section_t wraps a hardware spinlock AND local-core interrupt disable.
// It is the correct primitive here because core 0 writes the accumulator from
// the BLE callback (which may run in IRQ context) while core 1 drains it from
// its frame loop. The section is held only for a handful of instructions.
static critical_section_t g_cs;

// ---- Shared state (guarded by g_cs except where noted) --------------------
static int16_t  s_acc_dx    = 0;
static int16_t  s_acc_dy    = 0;
static int16_t  s_acc_wheel = 0;
static uint8_t  s_cur_btn   = 0;   // most recent button state
static uint8_t  s_latch_btn = 0;   // OR of button bits since last drain
static uint8_t  s_n_reports = 0;

// Joystick bits and connection flag are single-word; written by core 0, read by
// core 1. Published with relaxed atomics — hardware guarantees single-copy
// atomicity for aligned accesses on RP2040/RP2350, and _Atomic prevents the
// compiler from caching values across the core boundary.
static _Atomic uint16_t s_joy_bits        = 0;
static _Atomic bool     s_mouse_connected = false;

void corelink_init(void)
{
    critical_section_init(&g_cs);
    s_acc_dx = s_acc_dy = s_acc_wheel = 0;
    s_cur_btn = s_latch_btn = 0;
    s_n_reports = 0;
    atomic_store_explicit(&s_joy_bits, 0, memory_order_relaxed);
    atomic_store_explicit(&s_mouse_connected, false, memory_order_relaxed);
}

// ---- Core 0 (producer) ----------------------------------------------------

void corelink_mouse_push(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    critical_section_enter_blocking(&g_cs);
    s_acc_dx    = (int16_t)(s_acc_dx + dx);
    s_acc_dy    = (int16_t)(s_acc_dy + dy);
    s_acc_wheel = (int16_t)(s_acc_wheel + wheel);
    s_cur_btn   = buttons;
    s_latch_btn = (uint8_t)(s_latch_btn | buttons);
    if (s_n_reports < 255) s_n_reports++;
    critical_section_exit(&g_cs);
}

void corelink_joystick_set(uint16_t joy_bits)
{
    atomic_store_explicit(&s_joy_bits, joy_bits, memory_order_relaxed);
}

void corelink_set_mouse_connected(bool connected)
{
    atomic_store_explicit(&s_mouse_connected, connected, memory_order_relaxed);
}

// ---- Core 1 (consumer) ----------------------------------------------------

void corelink_mouse_drain(corelink_mouse_t *out)
{
    critical_section_enter_blocking(&g_cs);
    out->dx        = s_acc_dx;
    out->dy        = s_acc_dy;
    out->wheel     = s_acc_wheel;
    out->buttons   = (uint8_t)(s_latch_btn | s_cur_btn);
    out->n_reports = s_n_reports;
    // Drain: zero motion accumulators and the report counter, clear the latch.
    // s_cur_btn is left as-is: it is the live button level and must persist
    // until core 0 updates it, so a held button keeps reporting pressed.
    s_acc_dx    = 0;
    s_acc_dy    = 0;
    s_acc_wheel = 0;
    s_latch_btn = 0;
    s_n_reports = 0;
    critical_section_exit(&g_cs);
}

uint16_t corelink_joystick_get(void)
{
    return atomic_load_explicit(&s_joy_bits, memory_order_relaxed);
}

bool corelink_mouse_connected(void)
{
    return atomic_load_explicit(&s_mouse_connected, memory_order_relaxed);
}