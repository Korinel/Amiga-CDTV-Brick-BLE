// =============================================================================
/*
 * BLE Mouse Receiver Header
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 */
// =============================================================================

#ifndef CDTV_BLE_MOUSE_H
#define CDTV_BLE_MOUSE_H

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Pairing Timeout
// =============================================================================

#ifndef PAIRING_TIMEOUT_MS
#define PAIRING_TIMEOUT_MS 60000  // 60 seconds
#endif

// =============================================================================
// Callback Type
// =============================================================================

/**
 * Called when mouse movement or button press is received via BLE.
 * @param dx      X-axis movement delta (12-bit range, not pre-clamped)
 * @param dy      Y-axis movement delta (12-bit range, not pre-clamped)
 * @param wheel   Scroll wheel delta
 * @param buttons Button state bitmask
 */
typedef void (*mouse_input_callback_t)(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons);

// =============================================================================
// Public Functions
// =============================================================================

/**
 * Initialise BLE mouse in pairing mode.
 * Starts a BLE scan filtered to HID devices (UUID 0x1812).
 * A PAIRING_TIMEOUT_MS one-shot timer is armed; on expiry scanning stops
 * and the module enters BT_Disabled state for the session.
 *
 * @param cb  Callback invoked on each HID input report
 * @return true on success
 */
bool ble_mouse_init_pairing(mouse_input_callback_t cb);

/**
 * Process pending BLE events (non-blocking).
 * Call regularly from the main loop.
 */
void ble_mouse_process_events(void);

/**
 * Check if a HID device is connected and ready.
 * @return true if connected
 */
bool ble_mouse_is_connected(void);

#endif // CDTV_BLE_MOUSE_H
