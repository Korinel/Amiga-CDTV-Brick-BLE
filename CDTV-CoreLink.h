// =============================================================================
/*
 * CDTV Core Link — Core 0 (input) to Core 1 (IR output) hand-off
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Single owner of the data that crosses the core boundary.
 *
 * Division of labour (agreed design):
 *   Core 0 = INPUT only, RAW, no adjustments.
 *     - Runs BLE / BTstack / CYW43.
 *     - Accumulates raw mouse dx/dy/wheel and button state as reports arrive.
 *     - Publishes raw joystick GPIO bits.
 *     - Performs NO scaling, negation, clamping or direction logic.
 *
 *   Core 1 = OUTPUT and ALL adjustments. Sole owner of the IR LED.
 *     - Owns the frame cadence (its own timing loop, like the proven square test).
 *     - Consumes the raw accumulator once per mouse frame.
 *     - Does scale / direction-preservation / clamp / CDTV negation / burst
 *       discard, encodes, and transmits.
 *     - Alternates mouse and joystick frames; nothing else ever drives the LED.
 *
 * Concurrency model:
 *   A single hardware spinlock (via critical_section_t) guards the shared
 *   accumulator. Both cores hold it only for the few instructions needed to
 *   add (core 0) or snapshot-and-zero (core 1). It is NEVER held during IR
 *   transmission — core 1 copies out under the lock, releases, then transmits
 *   with interrupts fully enabled and no lock held. This is what lets core 1
 *   run an uninterrupted, metronomic frame cadence.
 *
 * Accumulate semantics:
 *   Motion is summed on core 0 and drained (read-and-zero) by core 1 each frame,
 *   so every report is counted exactly once: nothing double-counted, nothing
 *   dropped. dx/dy are int16 to hold multiple reports plus burst spikes without
 *   overflow; core 1 is responsible for clamping to the CDTV int8 range.
 */
// =============================================================================

#ifndef CDTV_CORELINK_H
#define CDTV_CORELINK_H

#include <stdint.h>
#include <stdbool.h>

// Snapshot of raw input drained by core 1 once per mouse frame.
typedef struct {
    int16_t dx;        // raw accumulated X (not scaled, not negated)
    int16_t dy;        // raw accumulated Y
    int16_t wheel;     // raw accumulated wheel
    uint8_t buttons;   // latched | current button bits since last drain
    uint8_t n_reports; // BLE reports coalesced into this snapshot (diagnostic)
} corelink_mouse_t;

// Initialise the link. Call once on core 0 before launching core 1.
void corelink_init(void);

// ---- Core 0 (producer) ----------------------------------------------------

// Add one BLE mouse report's raw deltas + buttons. Safe from the BLE callback.
void corelink_mouse_push(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons);

// Publish current joystick GPIO bitmask (raw, active-high already applied by
// the joystick reader). Latest value wins; joystick state is level, not summed.
void corelink_joystick_set(uint16_t joy_bits);

// Mark whether a BLE mouse is currently connected. Core 1 reads this to decide
// whether to emit mouse frames.
void corelink_set_mouse_connected(bool connected);

// ---- Core 1 (consumer) ----------------------------------------------------

// Atomically read and zero the mouse accumulator. Returns the snapshot.
void corelink_mouse_drain(corelink_mouse_t *out);

// Read the latest joystick bitmask.
uint16_t corelink_joystick_get(void);

// Is a BLE mouse connected?
bool corelink_mouse_connected(void);

#endif // CDTV_CORELINK_H