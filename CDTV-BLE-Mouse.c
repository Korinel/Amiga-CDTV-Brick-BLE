// =============================================================================
/*
 * BLE Mouse / Trackball Receiver Implementation
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Implements a HOGP (HID over GATT Profile) central/host for mouse and
 * trackball devices, per Bluetooth SIG HOGP v1.0 and Core Spec 5.4.
 *
 * Discovery sequence:
 *   1. Scan for devices advertising HID service (0x1812)
 *   2. Connect and pair/bond (SM, Just Works, bonding)
 *   3. Read HID Information characteristic (0x2A4A)   [HOGP mandatory]
 *   4. Write Protocol Mode = Report Protocol (0x2A4E) [HOGP mandatory]
 *   5. Read Report Map (0x2A4B) via long read         [HOGP mandatory]
 *   6. Discover all Report characteristics (0x2A4D)
 *   7. Enable CCCD notifications on all notifiable reports
 *   8. Decode incoming HID reports using parsed descriptor
 *
 * Note on step 7: HOGP mandates reading the Report Reference descriptor
 * (0x2908) per characteristic to identify Input/Output/Feature type before
 * subscribing. This step is intentionally skipped here because the extended
 * GATT discovery time caused this mouse to stop sending notifications.
 * Instead we subscribe to all notifiable characteristics immediately and
 * rely on the Report Map parse (step 5) to correctly identify the mouse
 * report by Report ID during decoding.
 *
 * Intentional deviations from full HOGP spec:
 *   - IO capability is NO_INPUT_NO_OUTPUT (Just Works) — headless adapter.
 *   - Report Reference descriptor reads skipped — see note above.
 *   - Battery Service (0x180F) not read — not needed for this adapter.
 *   - Device Information Service (0x180A) not read — not needed.
 *   - Boot Protocol Mode not supported; Report Protocol only.
 */
// =============================================================================

#include "CDTV-BLE-Mouse.h"
#include <stdio.h>
#include <string.h>
#include "btstack.h"
#include "pico/cyw43_arch.h"

// =============================================================================
// Constants
// =============================================================================
#define MAX_REPORT_MAP_SIZE  512
#define MAX_USAGES           16
#define MAX_HID_REPORTS      4

// HOGP characteristic UUIDs (Bluetooth SIG assigned numbers)
#define UUID_HID_SERVICE         0x1812
#define UUID_HID_INFORMATION     0x2A4A  // bcdHID, bCountryCode, Flags
#define UUID_REPORT_MAP          0x2A4B  // HID Report Descriptor bytes
#define UUID_REPORT              0x2A4D  // HID Input/Output/Feature report
#define UUID_PROTOCOL_MODE       0x2A4E  // 0x00=Boot, 0x01=Report
#define UUID_REPORT_REFERENCE    0x2908  // Descriptor: [Report ID, Report Type]

// HID Report Types (HOGP Table 5.5)
#define HID_REPORT_TYPE_INPUT    0x01
#define HID_REPORT_TYPE_OUTPUT   0x02
#define HID_REPORT_TYPE_FEATURE  0x03

// Protocol Mode values (HOGP 5.4.3)
#define HID_PROTOCOL_MODE_BOOT   0x00
#define HID_PROTOCOL_MODE_REPORT 0x01

// =============================================================================
// State Machine
//
// Tracks which step of the HOGP discovery sequence we are in. The flow is
// driven entirely by GATT and HCI event callbacks — no polling, no RTOS.
// States are entered from within the event handlers as each step completes.
// =============================================================================
typedef enum {
    BLE_STATE_OFF,                       // BTstack not running / timed out
    BLE_STATE_SCANNING,                  // Scanning for HID devices (UUID 0x1812)
    BLE_STATE_CONNECTING,                // Connection request sent
    BLE_STATE_PAIRING,                   // Waiting for SM pairing to complete
    BLE_STATE_DISCOVERING_SERVICES,      // Discovering the HID GATT service
    BLE_STATE_READING_HID_INFO,          // Reading HID Information (0x2A4A)
    BLE_STATE_WRITING_PROTOCOL_MODE,     // Writing Protocol Mode = Report (0x2A4E)
    BLE_STATE_FINDING_REPORT_MAP,        // Discovering Report Map characteristic
    BLE_STATE_READING_REPORT_MAP,        // Long-reading the Report Map value
    BLE_STATE_DISCOVERING_REPORTS,       // Discovering all Report characteristics
    BLE_STATE_ENABLING_NOTIFICATIONS,    // Writing CCCD to enable notifications
    BLE_STATE_READY,                     // Connected and receiving HID input
} ble_state_t;

// =============================================================================
// Static State
// =============================================================================
static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;
static ble_state_t g_ble_state = BLE_STATE_OFF;
static bd_addr_t server_addr;
static bd_addr_type_t server_addr_type;
static hci_con_handle_t connection_handle;

// HID service
static gatt_client_service_t hid_service;
static bool hid_service_found = false;

// HID Information characteristic (0x2A4A)
static gatt_client_characteristic_t hid_info_char;
static bool hid_info_char_found = false;

// Protocol Mode characteristic (0x2A4E)
static gatt_client_characteristic_t protocol_mode_char;
static bool protocol_mode_char_found = false;

// Report Map characteristic (0x2A4B)
static gatt_client_characteristic_t report_map_char;
static bool report_map_char_found = false;

// All Report characteristics (0x2A4D) found during GATT discovery.
// We subscribe to every one that has the NOTIFY property; decode_mouse_report
// then filters by Report ID using the parsed Report Map.
static gatt_client_characteristic_t all_report_chars[MAX_HID_REPORTS];
static int num_all_reports = 0;

// Subset of all_report_chars that have the NOTIFY property — these are the
// characteristics we register notifications on and receive input from.
static gatt_client_characteristic_t hid_input_chars[MAX_HID_REPORTS];
static int num_input_reports = 0;
static int current_notification_index = 0;
static gatt_client_notification_t notification_listeners[MAX_HID_REPORTS];

// Pairing state
static bool pairing_in_progress = false;
static bool device_bonded = false;

// Pairing timeout one-shot timer
static btstack_timer_source_t g_pairing_timeout_timer;
static bool g_pairing_timed_out = false;

// Callback
static mouse_input_callback_t input_callback = NULL;

// =============================================================================
// HID Report Descriptor Parsing
// =============================================================================

typedef struct {
    uint8_t  report_id;
    uint16_t buttons_bit_offset;
    uint8_t  buttons_bit_size;
    uint8_t  button_count;
    uint16_t x_bit_offset;
    uint8_t  x_bit_size;
    uint16_t y_bit_offset;
    uint8_t  y_bit_size;
    uint16_t wheel_bit_offset;
    uint8_t  wheel_bit_size;
    uint16_t report_data_bytes;  // Total data payload size (without Report ID byte)
    bool     valid;
} hid_mouse_format_t;

static hid_mouse_format_t mouse_format = {0};

static uint8_t  report_map_data[MAX_REPORT_MAP_SIZE];
static uint16_t report_map_len = 0;

static void parse_report_descriptor(const uint8_t *desc, uint16_t len) {
    printf("\n=== HID Report Descriptor (%d bytes) ===\n", len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", desc[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");

    uint8_t  usage_page        = 0;
    uint8_t  report_size       = 0;
    uint8_t  report_count      = 0;
    uint8_t  current_report_id = 0;
    uint16_t bit_position      = 0;

    uint8_t  usages[MAX_USAGES];
    uint8_t  usage_count = 0;
    uint8_t  usage_min   = 0;
    uint8_t  usage_max   = 0;

    memset(&mouse_format, 0, sizeof(mouse_format));

    uint16_t i = 0;
    while (i < len) {
        uint8_t  prefix = desc[i];
        uint8_t  bSize  = prefix & 0x03;
        uint8_t  bType  = (prefix >> 2) & 0x03;
        uint8_t  bTag   = (prefix >> 4) & 0x0F;

        if (bSize == 3) bSize = 4;
        if (i + 1 + bSize > len) break;

        uint32_t data = 0;
        for (uint8_t j = 0; j < bSize; j++) {
            data |= (uint32_t)desc[i + 1 + j] << (8 * j);
        }

        if (bType == 0) {  // Main items
            if (bTag == 0x08) {  // INPUT
                printf("  INPUT: %d fields of %d bits at bit %d (usage_page=0x%02X, report_id=0x%02X)\n",
                       report_count, report_size, bit_position, usage_page, current_report_id);

                if (usage_page == 0x09) {  // Button page
                    mouse_format.buttons_bit_offset = bit_position;
                    mouse_format.buttons_bit_size   = report_size;
                    mouse_format.button_count       = report_count;
                    printf("    -> BUTTONS at bit %d\n", bit_position);
                } else if (usage_page == 0x01) {  // Generic Desktop page
                    for (int u = 0; u < (int)usage_count && u < (int)report_count; u++) {
                        uint16_t field_offset = bit_position + (u * report_size);
                        if (usages[u] == 0x30) {  // X axis
                            mouse_format.x_bit_offset = field_offset;
                            mouse_format.x_bit_size   = report_size;
                            mouse_format.report_id    = current_report_id;
                            printf("    -> X at bit %d, size %d\n", field_offset, report_size);
                        } else if (usages[u] == 0x31) {  // Y axis
                            mouse_format.y_bit_offset = field_offset;
                            mouse_format.y_bit_size   = report_size;
                            printf("    -> Y at bit %d, size %d\n", field_offset, report_size);
                        } else if (usages[u] == 0x38) {  // Wheel
                            mouse_format.wheel_bit_offset = field_offset;
                            mouse_format.wheel_bit_size   = report_size;
                            printf("    -> WHEEL at bit %d, size %d\n", field_offset, report_size);
                        }
                    }
                }
                bit_position += (uint16_t)report_count * (uint16_t)report_size;
                usage_count = 0;
            } else if (bTag == 0x0A) {
                printf("  COLLECTION (0x%02X)\n", (uint8_t)data);
            } else if (bTag == 0x0C) {
                printf("  END COLLECTION\n");
            }
        } else if (bType == 1) {  // Global items
            if      (bTag == 0x00) { usage_page        = (uint8_t)data; printf("  USAGE PAGE: 0x%02X\n", usage_page); }
            else if (bTag == 0x07) { report_size        = (uint8_t)data; }
            else if (bTag == 0x08) { current_report_id = (uint8_t)data; bit_position = 0; printf("  REPORT ID: 0x%02X\n", current_report_id); }
            else if (bTag == 0x09) { report_count       = (uint8_t)data; }
        } else if (bType == 2) {  // Local items
            if (bTag == 0x00) {
                if (usage_count < MAX_USAGES) usages[usage_count++] = (uint8_t)data;
            } else if (bTag == 0x01) {
                usage_min = (uint8_t)data;
            } else if (bTag == 0x02) {
                usage_max = (uint8_t)data;
                // Use uint16_t loop variable to avoid uint8_t wrap when usage_max == 0xFF
                for (uint16_t u = usage_min; u <= (uint16_t)usage_max && usage_count < MAX_USAGES; u++) {
                    usages[usage_count++] = (uint8_t)u;
                }
            }
        }

        i += 1 + bSize;
    }

    if (mouse_format.x_bit_size > 0 && mouse_format.y_bit_size > 0) {
        mouse_format.valid = true;

        // Second pass: count total bits in the mouse report to get its data size.
        // This lets decode_mouse_report distinguish mouse reports from vendor reports
        // without hardcoding a byte count.
        if (mouse_format.report_id != 0) {
            uint16_t bp = 0;
            uint8_t  cur_id = 0, rcount = 0, rsize = 0;
            for (uint16_t j = 0; j < len; ) {
                uint8_t  px    = desc[j];
                uint8_t  bsz   = px & 0x03; if (bsz == 3) bsz = 4;
                uint8_t  btype = (px >> 2) & 0x03;
                uint8_t  btag  = (px >> 4) & 0x0F;
                uint32_t val   = 0;
                if (j + 1u + bsz > len) break;
                for (uint8_t k = 0; k < bsz; k++) val |= (uint32_t)desc[j + 1 + k] << (8 * k);
                j += 1 + bsz;
                if (btype == 1) {
                    if      (btag == 0x07) rsize  = (uint8_t)val;
                    else if (btag == 0x08) { cur_id = (uint8_t)val; bp = 0; }
                    else if (btag == 0x09) rcount = (uint8_t)val;
                } else if (btype == 0 && btag == 0x08 && cur_id == mouse_format.report_id) {
                    bp += (uint16_t)rcount * (uint16_t)rsize;
                    // Save after every INPUT for our report: if a later REPORT_ID
                    // resets bp, the correct total is already stored here.
                    mouse_format.report_data_bytes = (bp + 7) / 8;
                }
            }
        }

        printf("\n=== Parsed Mouse Format ===\n");
        if (mouse_format.report_id != 0)
            printf("  Report ID: 0x%02X\n", mouse_format.report_id);
        printf("  Buttons: bit %d, %d bits, %d buttons\n",
               mouse_format.buttons_bit_offset, mouse_format.buttons_bit_size, mouse_format.button_count);
        printf("  X: bit %d, %d bits\n", mouse_format.x_bit_offset, mouse_format.x_bit_size);
        printf("  Y: bit %d, %d bits\n", mouse_format.y_bit_offset, mouse_format.y_bit_size);
        if (mouse_format.wheel_bit_size > 0)
            printf("  Wheel: bit %d, %d bits\n", mouse_format.wheel_bit_offset, mouse_format.wheel_bit_size);
        if (mouse_format.report_data_bytes > 0)
            printf("  Data bytes: %d\n", mouse_format.report_data_bytes);
        printf("===========================\n\n");
    } else {
        printf("\nWARNING: Could not parse mouse format from descriptor, using fallback\n\n");
    }
}

// Extract a signed integer of bit_size bits starting at bit_offset from data[]
static int32_t extract_signed_bits(const uint8_t *data, uint16_t data_len,
                                   uint16_t bit_offset, uint8_t bit_size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < bit_size; i++) {
        uint16_t bit_pos  = bit_offset + i;
        uint16_t byte_idx = bit_pos / 8;
        uint8_t  bit_idx  = bit_pos % 8;
        if (byte_idx >= data_len) break;  // bounds guard
        if (data[byte_idx] & (1u << bit_idx)) value |= (1u << i);
    }
    // Sign-extend
    if (bit_size < 32 && (value & (1u << (bit_size - 1))))
        value |= ~((1u << bit_size) - 1);
    return (int32_t)value;
}

static void decode_mouse_report(const uint8_t *report, uint16_t report_len) {
    if (!input_callback) return;
    if (report_len < 3) return;

    uint8_t  buttons = 0;
    int16_t  dx      = 0;
    int16_t  dy      = 0;
    int8_t   wheel   = 0;

    if (mouse_format.valid) {
        // Devices with multiple reports (report_id != 0) may or may not prepend the
        // Report ID byte to notification payloads. Detect which case we have by size:
        //   - report_len == report_data_bytes      → no Report ID in payload (HOGP-compliant)
        //   - report_len == report_data_bytes + 1  → Report ID prepended; strip it
        //   - anything else                        → not our mouse report, ignore
        const uint8_t *data = report;
        uint16_t data_len   = report_len;
        if (mouse_format.report_id != 0 && mouse_format.report_data_bytes > 0) {
            if (report_len == mouse_format.report_data_bytes) {
                // HOGP-compliant: no Report ID in payload
            } else if (report_len == mouse_format.report_data_bytes + 1 &&
                       report[0] == mouse_format.report_id) {
                // Report ID is prepended — skip it
                data     = report + 1;
                data_len = report_len - 1;
            } else {
                return;  // Wrong size — vendor report or unknown, ignore
            }
        }

        uint16_t total_bits = data_len * 8;

        uint16_t buttons_total_bits = (uint16_t)mouse_format.buttons_bit_size * (uint16_t)mouse_format.button_count;
        // Cap at 8 bits — buttons is passed as uint8_t; bits 0-7 cover all
        // standard mouse buttons (left, right, middle, forward, back).
        uint8_t buttons_to_extract = (buttons_total_bits > 8) ? 8 : (uint8_t)buttons_total_bits;
        if (buttons_to_extract > 0 &&
            mouse_format.buttons_bit_offset + buttons_to_extract <= total_bits) {
            buttons = (uint8_t)extract_signed_bits(data, data_len,
                          mouse_format.buttons_bit_offset, buttons_to_extract);
        }

        // Pass raw deltas — do NOT clamp here. The accumulator in CDTV-CoreLink.c
        // sums multiple reports per frame; clamping and scaling happen in CDTV-IR-Core1.c.
        if (mouse_format.x_bit_offset + mouse_format.x_bit_size <= total_bits) {
            dx = (int16_t)extract_signed_bits(data, data_len,
                     mouse_format.x_bit_offset, mouse_format.x_bit_size);
        }
        if (mouse_format.y_bit_offset + mouse_format.y_bit_size <= total_bits) {
            dy = (int16_t)extract_signed_bits(data, data_len,
                     mouse_format.y_bit_offset, mouse_format.y_bit_size);
        }
        if (mouse_format.wheel_bit_size > 0 &&
            mouse_format.wheel_bit_offset + mouse_format.wheel_bit_size <= total_bits) {
            int32_t w_raw = extract_signed_bits(data, data_len,
                                mouse_format.wheel_bit_offset, mouse_format.wheel_bit_size);
            wheel = (w_raw > 127) ? 127 : ((w_raw < -128) ? -128 : (int8_t)w_raw);
        }

#ifdef DEBUG_BLE
        if (dx != 0 || dy != 0 || buttons != 0 || wheel != 0) {
            printf("RAW[%d]:", report_len);
            for (int i = 0; i < (int)report_len && i < 8; i++) printf(" %02X", report[i]);
            printf(" | X=%d Y=%d W=%d btn=0x%02X\n", dx, dy, wheel, buttons);
        }
#endif
    } else {
        // Fallback: no valid parsed format — assume standard boot-protocol layout.
        if (report_len >= 4 && report[1] == 0x00) {
            buttons = report[0];
            dx      = (int8_t)report[2];
            dy      = (int8_t)report[3];
        } else {
            buttons = report[0];
            dx      = (int8_t)report[1];
            dy      = (int8_t)report[2];
        }
    }

#ifdef DEBUG_BLE
    if (buttons != 0 || dx != 0 || dy != 0 || wheel != 0)
        printf("BLE: btn=0x%02X dx=%+4d dy=%+4d wheel=%+4d\n", buttons, dx, dy, wheel);
#endif

    input_callback(dx, dy, wheel, buttons);
}

// =============================================================================
// Forward Declarations
// =============================================================================
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void start_scan(void);
static void start_enable_notifications(void);
static void discover_protocol_mode(void);
static void pairing_timeout_callback(btstack_timer_source_t *ts);
static bool ble_mouse_common_init(mouse_input_callback_t cb);

// =============================================================================
// Helper: transition to Protocol Mode discovery
// =============================================================================
static void discover_protocol_mode(void) {
    printf("GATT: Discovering Protocol Mode characteristic (0x2A4E)...\n");
    g_ble_state = BLE_STATE_WRITING_PROTOCOL_MODE;
    protocol_mode_char_found = false;
    gatt_client_discover_characteristics_for_service_by_uuid16(
        handle_gatt_client_event, connection_handle, &hid_service, UUID_PROTOCOL_MODE);
}

// =============================================================================
// Helper: after characteristic discovery, enable notifications on all notifiable
// reports. We subscribe to everything with NOTIFY property and let
// decode_mouse_report filter by Report ID using the parsed Report Map.
// =============================================================================
static void start_enable_notifications(void) {
    num_input_reports = 0;
    for (int i = 0; i < num_all_reports; i++) {
        if (all_report_chars[i].properties & 0x10) {  // NOTIFY
            if (num_input_reports < MAX_HID_REPORTS) {
                hid_input_chars[num_input_reports++] = all_report_chars[i];
                printf("GATT: Report #%d (handle=0x%04X) -> subscribing\n",
                       i + 1, all_report_chars[i].value_handle);
            }
        }
    }

    if (num_input_reports == 0) {
        printf("GATT: No notifiable reports found\n");
        gap_disconnect(connection_handle);
        return;
    }

    g_ble_state = BLE_STATE_ENABLING_NOTIFICATIONS;
    current_notification_index = 0;
    printf("GATT: Enabling notifications on %d report(s)...\n", num_input_reports);
    gatt_client_listen_for_characteristic_value_updates(
        &notification_listeners[0], handle_gatt_client_event,
        connection_handle, &hid_input_chars[0]);
    gatt_client_write_client_characteristic_configuration(
        handle_gatt_client_event, connection_handle, &hid_input_chars[0],
        GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
}

// =============================================================================
// BLE Event Handlers
// =============================================================================
static void start_scan(void) {
    printf("\n========================================\n");
    printf("SCANNING FOR BLE HID MOUSE / TRACKBALL\n");
    printf("Put your device in pairing mode now...\n");
    printf("========================================\n\n");
    g_ble_state = BLE_STATE_SCANNING;
    gap_set_scan_parameters(0, 0x0030, 0x0030);
    gap_start_scan();
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            // DEVIATION: Using IO_CAPABILITY_NO_INPUT_NO_OUTPUT (Just Works) rather than
            // DISPLAY_YES_NO. This sacrifices MITM protection but is necessary for a
            // headless adapter — there is no display or buttons to confirm a passkey.
            printf("SM: Just Works pairing requested (handle=0x%04X) - auto-confirming\n",
                   sm_event_just_works_request_get_handle(packet));
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("SM: Numeric comparison request - auto-confirming\n");
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            printf("SM: Display passkey %06u\n",
                   (unsigned int)sm_event_passkey_display_number_get_passkey(packet));
            break;

        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            // With IO_CAPABILITY_NO_INPUT_NO_OUTPUT this event should not occur.
            // If it does, a peripheral is forcing a pairing mode we can't satisfy —
            // disconnect rather than silently accepting passkey 0.
            printf("SM: Unexpected passkey input request - disconnecting\n");
            gap_disconnect(connection_handle);
            break;

        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)) {
                case ERROR_CODE_SUCCESS:
                    printf("SM: *** PAIRING SUCCESSFUL ***\n");
                    device_bonded       = true;
                    pairing_in_progress = false;
                    // Cancel the pairing timeout — we succeeded
                    btstack_run_loop_remove_timer(&g_pairing_timeout_timer);
                    if (g_ble_state == BLE_STATE_PAIRING) {
                        // Resume from wherever we were interrupted
                        printf("SM: Resuming HID service discovery...\n");
                        g_ble_state       = BLE_STATE_DISCOVERING_SERVICES;
                        hid_service_found = false;
                        gatt_client_discover_primary_services_by_uuid16(
                            handle_gatt_client_event, connection_handle, UUID_HID_SERVICE);
                    }
                    break;
                default:
                    printf("SM: Pairing failed (status=0x%02X)\n",
                           sm_event_pairing_complete_get_status(packet));
                    pairing_in_progress = false;
                    gap_disconnect(connection_handle);
                    break;
            }
            break;

        default:
            break;
    }
}

// Shared helper: trigger pairing if authentication is required and not already in progress
static bool maybe_start_pairing(uint8_t att_status) {
    if (att_status == ATT_ERROR_INSUFFICIENT_AUTHENTICATION && !device_bonded && !pairing_in_progress) {
        printf("GATT: Authentication required - starting pairing\n");
        pairing_in_progress = true;
        g_ble_state = BLE_STATE_PAIRING;
        sm_request_pairing(connection_handle);
        return true;
    }
    return false;
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);
    uint8_t att_status;

    switch (g_ble_state) {

        // -----------------------------------------------------------------
        // Step 3a: Discover HID Information characteristic (0x2A4A)
        // HOGP mandatory: host shall read HID Information before proceeding
        // -----------------------------------------------------------------
        case BLE_STATE_READING_HID_INFO:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &hid_info_char);
                    hid_info_char_found = true;
                    printf("GATT: Found HID Information characteristic (handle=0x%04X)\n",
                           hid_info_char.value_handle);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (maybe_start_pairing(att_status)) break;
                    if (hid_info_char_found) {
                        // Read it — we log the value but don't gate on it
                        gatt_client_read_value_of_characteristic(
                            handle_gatt_client_event, connection_handle, &hid_info_char);
                    } else {
                        // Not found — non-fatal, proceed to Protocol Mode
                        printf("GATT: HID Information not found (non-fatal), continuing\n");
                        discover_protocol_mode();
                    }
                    break;
                case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    // HID Information: bcdHID(2), bCountryCode(1), Flags(1)
                    uint16_t vlen = gatt_event_characteristic_value_query_result_get_value_length(packet);
                    const uint8_t *v = gatt_event_characteristic_value_query_result_get_value(packet);
                    if (vlen >= 4) {
                        uint16_t bcd = little_endian_read_16(v, 0);
                        printf("GATT: HID Information: bcdHID=0x%04X country=0x%02X flags=0x%02X\n",
                               bcd, v[2], v[3]);
                    }
                    discover_protocol_mode();
                    break;
                }
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 3b: Write Protocol Mode = Report Protocol (0x01)
        // HOGP mandatory: host shall set Report Protocol mode
        // -----------------------------------------------------------------
        case BLE_STATE_WRITING_PROTOCOL_MODE:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &protocol_mode_char);
                    protocol_mode_char_found = true;
                    printf("GATT: Found Protocol Mode characteristic (handle=0x%04X)\n",
                           protocol_mode_char.value_handle);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (maybe_start_pairing(att_status)) break;
                    if (protocol_mode_char_found) {
                        // Write Report Protocol (0x01) using Write Without Response
                        // per HOGP 5.4.3 — Protocol Mode uses WRITE_NO_RSP property
                        uint8_t report_mode = HID_PROTOCOL_MODE_REPORT;
                        printf("GATT: Writing Protocol Mode = Report Protocol\n");
                        gatt_client_write_value_of_characteristic_without_response(
                            connection_handle,
                            protocol_mode_char.value_handle,
                            1, &report_mode);
                    } else {
                        // DEVIATION: Protocol Mode characteristic absent on some devices.
                        // They default to Report Protocol, so we proceed without writing.
                        printf("GATT: Protocol Mode not found - device likely defaults to Report Protocol\n");
                    }
                    // Either way, proceed to Report Map
                    printf("GATT: Discovering Report Map characteristic (0x2A4B)...\n");
                    g_ble_state = BLE_STATE_FINDING_REPORT_MAP;
                    report_map_char_found = false;
                    gatt_client_discover_characteristics_for_service_by_uuid16(
                        handle_gatt_client_event, connection_handle, &hid_service, UUID_REPORT_MAP);
                    break;
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 4: Discover Report Map characteristic (0x2A4B)
        // -----------------------------------------------------------------
        case BLE_STATE_FINDING_REPORT_MAP:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    gatt_event_characteristic_query_result_get_characteristic(packet, &report_map_char);
                    report_map_char_found = true;
                    printf("GATT: Found Report Map characteristic (handle=0x%04X)\n",
                           report_map_char.value_handle);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (maybe_start_pairing(att_status)) break;
                    if (!report_map_char_found) {
                        // DEVIATION: No Report Map — fall back to descriptor-less decoding
                        printf("GATT: Report Map not found, will use fallback decoding\n");
                        g_ble_state = BLE_STATE_DISCOVERING_REPORTS;
                        num_all_reports = 0;
                        gatt_client_discover_characteristics_for_service_by_uuid16(
                            handle_gatt_client_event, connection_handle, &hid_service, UUID_REPORT);
                        break;
                    }
                    printf("GATT: Reading Report Map (long read)...\n");
                    g_ble_state = BLE_STATE_READING_REPORT_MAP;
                    report_map_len = 0;
                    gatt_client_read_long_value_of_characteristic_using_value_handle(
                        handle_gatt_client_event, connection_handle, report_map_char.value_handle);
                    break;
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 5: Long-read Report Map value
        // -----------------------------------------------------------------
        case BLE_STATE_READING_REPORT_MAP:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT: {
                    uint16_t chunk_len    = gatt_event_long_characteristic_value_query_result_get_value_length(packet);
                    uint16_t chunk_offset = gatt_event_long_characteristic_value_query_result_get_value_offset(packet);
                    const uint8_t *chunk  = gatt_event_long_characteristic_value_query_result_get_value(packet);
                    // Guard against integer overflow and buffer overrun before copying.
                    // chunk_offset and chunk_len are both uint16_t; check separately.
                    if (chunk_offset < MAX_REPORT_MAP_SIZE &&
                        chunk_len <= MAX_REPORT_MAP_SIZE - chunk_offset) {
                        memcpy(&report_map_data[chunk_offset], chunk, chunk_len);
                        if (chunk_offset + chunk_len > report_map_len)
                            report_map_len = chunk_offset + chunk_len;
                    } else {
                        printf("GATT: Report Map chunk out of bounds (offset=%d len=%d) - discarding\n",
                               chunk_offset, chunk_len);
                    }
                    printf("GATT: Report Map chunk: %d bytes at offset %d (total %d)\n",
                           chunk_len, chunk_offset, report_map_len);
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    printf("GATT: Report Map read complete (status=0x%02X, len=%d)\n",
                           att_status, report_map_len);
                    if (maybe_start_pairing(att_status)) break;
                    if (report_map_len > 0)
                        parse_report_descriptor(report_map_data, report_map_len);
                    printf("GATT: Discovering Report characteristics (0x2A4D)...\n");
                    g_ble_state = BLE_STATE_DISCOVERING_REPORTS;
                    num_all_reports = 0;
                    gatt_client_discover_characteristics_for_service_by_uuid16(
                        handle_gatt_client_event, connection_handle, &hid_service, UUID_REPORT);
                    break;
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 6: Discover all Report characteristics (0x2A4D)
        // -----------------------------------------------------------------
        case BLE_STATE_DISCOVERING_REPORTS:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
                    gatt_client_characteristic_t characteristic;
                    gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
                    printf("GATT: Found Report characteristic (handle=0x%04X, props=0x%02X)\n",
                           characteristic.value_handle, characteristic.properties);
                    if (num_all_reports < MAX_HID_REPORTS) {
                        all_report_chars[num_all_reports] = characteristic;
                        num_all_reports++;
                    } else {
                        printf("GATT: Max reports (%d) reached, skipping\n", MAX_HID_REPORTS);
                    }
                    break;
                }
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    printf("GATT: Report characteristic discovery complete (status=0x%02X), found %d\n",
                           att_status, num_all_reports);
                    if (maybe_start_pairing(att_status)) break;
                    if (num_all_reports == 0) {
                        printf("GATT: No Report characteristics found\n");
                        gap_disconnect(connection_handle);
                        break;
                    }
                    // Skip Report Reference reads — go straight to subscribing.
                    // Report Reference reads added latency that caused this mouse
                    // to stop sending notifications. The Report Map parse already
                    // gives us the correct report ID for decoding.
                    start_enable_notifications();
                    break;
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 7: Enable CCCD notifications on all notifiable reports
        // -----------------------------------------------------------------
        case BLE_STATE_ENABLING_NOTIFICATIONS:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    printf("GATT: Notifications enabled on report #%d (status=0x%02X)\n",
                           current_notification_index + 1, att_status);
                    if (maybe_start_pairing(att_status)) break;

                    current_notification_index++;
                    if (current_notification_index < num_input_reports) {
                        printf("GATT: Enabling notifications on report #%d...\n",
                               current_notification_index + 1);
                        gatt_client_listen_for_characteristic_value_updates(
                            &notification_listeners[current_notification_index],
                            handle_gatt_client_event, connection_handle,
                            &hid_input_chars[current_notification_index]);
                        gatt_client_write_client_characteristic_configuration(
                            handle_gatt_client_event, connection_handle,
                            &hid_input_chars[current_notification_index],
                            GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                    } else {
                        printf("\n========================================\n");
                        printf("*** MOUSE / TRACKBALL CONNECTED AND READY ***\n");
                        printf("========================================\n\n");
                        g_ble_state = BLE_STATE_READY;
                    }
                    break;
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // Step 9: Receive HID Input reports via notifications
        // -----------------------------------------------------------------
        case BLE_STATE_READY:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_NOTIFICATION: {
                    uint16_t value_length = gatt_event_notification_get_value_length(packet);
                    const uint8_t *value  = gatt_event_notification_get_value(packet);

                    // Measure ACTUAL report delivery rate. The negotiated interval
                    // is the requested cadence; real delivery can differ (batching,
                    // skipped intervals when idle). Report a rolling summary every
                    // 100 notifications so we see the true steady-state rate without
                    // flooding UART during motion.
#ifdef DEBUG_BLE_RATE
                    {
                        static uint32_t last_us = 0;
                        static uint32_t n = 0;
                        static uint32_t sum_dt = 0, min_dt = 0xFFFFFFFF, max_dt = 0;
                        uint32_t now = time_us_32();
                        if (last_us) {
                            uint32_t dt = now - last_us;
                            sum_dt += dt;
                            if (dt < min_dt) min_dt = dt;
                            if (dt > max_dt) max_dt = dt;
                            if (++n >= 100) {
                                printf("BLE rate: %lu reports, dt avg=%lu min=%lu max=%lu us  len=%u\n",
                                       (unsigned long)n, (unsigned long)(sum_dt / n),
                                       (unsigned long)min_dt, (unsigned long)max_dt, value_length);
                                n = 0; sum_dt = 0; min_dt = 0xFFFFFFFF; max_dt = 0;
                            }
                        }
                        last_us = now;
                    }
#endif
                    decode_mouse_report(value, value_length);
                    break;
                }
                default:
                    break;
            }
            break;

        // -----------------------------------------------------------------
        // HID service discovery
        // -----------------------------------------------------------------
        case BLE_STATE_DISCOVERING_SERVICES:
            switch (hci_event_packet_get_type(packet)) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    gatt_event_service_query_result_get_service(packet, &hid_service);
                    hid_service_found = true;
                    printf("GATT: Found HID service (handles 0x%04X–0x%04X)\n",
                           hid_service.start_group_handle, hid_service.end_group_handle);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    printf("GATT: Service discovery complete (status=0x%02X)\n", att_status);
                    if (att_status != ATT_ERROR_SUCCESS || !hid_service_found) {
                        if (maybe_start_pairing(att_status)) break;
                        gap_disconnect(connection_handle);
                        break;
                    }
                    // Proceed to HID Information (HOGP step 3)
                    printf("GATT: Discovering HID Information characteristic (0x2A4A)...\n");
                    g_ble_state = BLE_STATE_READING_HID_INFO;
                    hid_info_char_found = false;
                    gatt_client_discover_characteristics_for_service_by_uuid16(
                        handle_gatt_client_event, connection_handle, &hid_service, UUID_HID_INFORMATION);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
}

static void hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(size);
    UNUSED(channel);
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {

        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("HCI: Bluetooth stack WORKING\n");
                start_scan();
            } else {
                g_ble_state = BLE_STATE_OFF;
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (g_ble_state != BLE_STATE_SCANNING) return;

            const uint8_t *adv_data = gap_event_advertising_report_get_data(packet);
            uint8_t adv_len         = gap_event_advertising_report_get_data_length(packet);
            bool has_hid_service    = false;

            ad_context_t context;
            for (ad_iterator_init(&context, adv_len, adv_data);
                 ad_iterator_has_more(&context);
                 ad_iterator_next(&context)) {
                uint8_t data_type = ad_iterator_get_data_type(&context);
                uint8_t data_size = ad_iterator_get_data_len(&context);
                const uint8_t *data = ad_iterator_get_data(&context);
                // AD types 0x02 (incomplete) and 0x03 (complete) 16-bit UUIDs
                if (data_type == 0x02 || data_type == 0x03) {
                    for (int i = 0; i + 1 < (int)data_size; i += 2) {
                        if (little_endian_read_16(data, i) == UUID_HID_SERVICE) {
                            has_hid_service = true;
                            break;
                        }
                    }
                }
            }
            if (!has_hid_service) return;

            gap_event_advertising_report_get_address(packet, server_addr);
            server_addr_type = gap_event_advertising_report_get_address_type(packet);

            printf("\n========================================\n");
            printf("FOUND HID DEVICE: %s\n", bd_addr_to_str(server_addr));
            printf("Connecting...\n");
            printf("========================================\n\n");
            g_ble_state = BLE_STATE_CONNECTING;
            gap_stop_scan();
            gap_connect(server_addr, server_addr_type);
            break;
        }

        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
                case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
                    uint8_t conn_status = hci_subevent_le_connection_complete_get_status(packet);
                    if (conn_status != ERROR_CODE_SUCCESS) {
                        printf("HCI: Connection failed (status=0x%02X), resuming scan...\n",
                               conn_status);
                        start_scan();
                        break;
                    }
                    if (g_ble_state != BLE_STATE_CONNECTING) break;
                    connection_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    printf("HCI: Connected (handle=0x%04X)\n", connection_handle);

                    // Log negotiated connection parameters. These set how often the
                    // device may send input reports, hence how many accumulate per
                    // IR frame. interval/latency in 1.25ms / interval units;
                    // supervision timeout in 10ms units.
                    {
                        uint16_t itv = hci_subevent_le_connection_complete_get_conn_interval(packet);
                        uint16_t lat = hci_subevent_le_connection_complete_get_conn_latency(packet);
                        uint16_t sto = hci_subevent_le_connection_complete_get_supervision_timeout(packet);
                        printf("HCI: Conn params negotiated: interval=%u (%lu us)  latency=%u  timeout=%u (%u ms)\n",
                               itv, (unsigned long)itv * 1250UL, lat, sto, sto * 10);
                        if (itv) {
                            uint32_t itv_us = (uint32_t)itv * 1250UL;
                            printf("HCI:   ~%lu input reports per 32044 us IR frame at this interval\n",
                                   (unsigned long)(32044UL / itv_us));
                        }
                    }
                    pairing_in_progress = false;
                    device_bonded       = false;

                    printf("HCI: Starting HID service discovery...\n");
                    g_ble_state       = BLE_STATE_DISCOVERING_SERVICES;
                    hid_service_found = false;
                    gatt_client_discover_primary_services_by_uuid16(
                        handle_gatt_client_event, connection_handle, UUID_HID_SERVICE);
                    break;
                }
                case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE: {
                    // HID devices frequently renegotiate to a faster interval once
                    // active. Log the new parameters so we see the steady-state rate.
                    uint16_t itv = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                    uint16_t lat = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                    uint16_t sto = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                    printf("HCI: Conn params UPDATED: interval=%u (%lu us)  latency=%u  timeout=%u (%u ms)\n",
                           itv, (unsigned long)itv * 1250UL, lat, sto, sto * 10);
                    if (itv) {
                        uint32_t itv_us = (uint32_t)itv * 1250UL;
                        printf("HCI:   ~%lu input reports per 32044 us IR frame at this interval\n",
                               (unsigned long)(32044UL / itv_us));
                    }
                    break;
                }
                default:
                    break;
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            connection_handle = HCI_CON_HANDLE_INVALID;
            printf("HCI: Device disconnected\n");

            for (int i = 0; i < num_input_reports; i++) {
                gatt_client_stop_listening_for_characteristic_value_updates(&notification_listeners[i]);
            }

            // Reset all per-connection state so a new device starts clean
            num_all_reports            = 0;
            num_input_reports          = 0;
            current_notification_index = 0;
            hid_service_found          = false;
            hid_info_char_found        = false;
            protocol_mode_char_found   = false;
            report_map_char_found      = false;
            report_map_len             = 0;
            memset(&mouse_format, 0, sizeof(mouse_format));
            pairing_in_progress        = false;
            device_bonded              = false;

            if (g_ble_state == BLE_STATE_OFF) break;
            // Pairing timeout sets g_ble_state = BLE_STATE_OFF, so we only rescan if the
            // timeout has not yet fired.
            printf("HCI: Resuming scan...\n");
            start_scan();
            break;

        default:
            break;
    }
}

// =============================================================================
// Pairing Timeout Callback
// =============================================================================
static void pairing_timeout_callback(btstack_timer_source_t *ts) {
    (void)ts;
    printf("BLE: Pairing timeout — no device paired within %d ms\n", PAIRING_TIMEOUT_MS);
    gap_stop_scan();
    g_ble_state        = BLE_STATE_OFF;
    g_pairing_timed_out = true;
}

// =============================================================================
// Common BTstack Initialisation
// =============================================================================
static bool ble_mouse_common_init(mouse_input_callback_t cb) {
    if (!cb) return false;
    input_callback    = cb;
    connection_handle = HCI_CON_HANDLE_INVALID;

    l2cap_init();
    sm_init();
    att_server_init(NULL, NULL, NULL);
    gatt_client_init();

    // DEVIATION: Using NO_INPUT_NO_OUTPUT rather than DISPLAY_YES_NO.
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);

    hci_event_callback_registration.callback = &hci_event_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    return true;
}

// =============================================================================
// Public Functions
// =============================================================================

bool ble_mouse_init_pairing(mouse_input_callback_t cb) {
    printf("BLE: Initializing in Pairing_Mode...\n");
    if (!ble_mouse_common_init(cb)) return false;

    // Arm the pairing timeout one-shot timer
    btstack_run_loop_set_timer(&g_pairing_timeout_timer, PAIRING_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&g_pairing_timeout_timer, pairing_timeout_callback);
    btstack_run_loop_add_timer(&g_pairing_timeout_timer);

    printf("BLE: Pairing timeout set to %d ms\n", PAIRING_TIMEOUT_MS);
    printf("BLE: Powering on Bluetooth controller...\n");
    hci_power_control(HCI_POWER_ON);
    // start_scan() (scan) is called from hci_event_handler when HCI_STATE_WORKING
    return true;
}

void ble_mouse_process_events(void) {
    // Drives the BTstack run loop tick and CYW43 background processing.
    cyw43_arch_poll();
}

bool ble_mouse_is_connected(void) {
    return (g_ble_state == BLE_STATE_READY);
}

bool ble_mouse_is_timed_out(void) {
    return g_pairing_timed_out;
}