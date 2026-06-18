// =============================================================================
/*
 * CDTV IR Output Engine — runs on core 1
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Core 1's entry point. Owns the IR LED exclusively and is the sole driver of
 * frame cadence. Reads raw input from the core link, applies ALL adjustments
 * (scale, direction preservation, clamp, CDTV negation, burst discard), encodes
 * and transmits. Alternates mouse and joystick frames so the two never overlap
 * on the wire.
 *
 * This mirrors the structure of the proven square-motion test: a tight loop
 * that transmits a frame then waits out the remainder of the frame period, with
 * nothing else competing for the core. CYW43/BLE work stays on core 0, so no
 * background interrupt can stretch a mark or jitter the cadence.
 */
// =============================================================================

#ifndef CDTV_IR_CORE1_H
#define CDTV_IR_CORE1_H

// Core 1 entry point. Pass to multicore_launch_core1(). Never returns.
void ir_core1_main(void);

#endif // CDTV_IR_CORE1_H