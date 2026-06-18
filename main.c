// =============================================================================
/*
 * CDTV BLE Mouse + Joystick to IR Transmitter — main / core 0
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Turns a modern Bluetooth mouse or trackball into a Commodore CDTV pointing
 * device by translating its input into the CDTV's infra-red mouse protocol.
 * Built and tuned with a Logitech M575 Bluetooth trackball on a Raspberry Pi
 * Pico W / Pico 2 W.
 *
 * Two-core design
 * ---------------
 *   Core 0 (this file) — INPUT. Runs Bluetooth (BTstack + CYW43), receives mouse
 *     reports, and hands the raw movement and button data to core 1 through the
 *     core link. It also reads the DB9 joystick ports. It does no signal timing.
 *
 *   Core 1 (CDTV-IR-Core1.c) — OUTPUT. Owns the IR LED and the frame timing.
 *     It applies sensitivity and the CDTV axis convention, then transmits the IR
 *     frames. Running output on its own core, free of Bluetooth/Wi-Fi
 *     interrupts, is what keeps the transmitted timing clean and the cursor
 *     smooth.
 *
 * Boot behaviour (JOY2_FIRE1 held at power-on)
 * --------------------------------------------
 *   Fire held     -> BLE pairing mode: scan for the first HID device, pair and
 *                    connect. 60 s timeout; if nothing pairs, fall back to
 *                    joystick-only. LED: slow blink (boot) → fast blink (scanning)
 *                    → solid (connected) → slow blink (timeout, joystick active).
 *   Fire not held -> Joystick-only mode immediately. No Bluetooth. LED solid.
 *
 * Both DB9 joystick ports are always active regardless of Bluetooth state.
 */
// =============================================================================

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"

#include "hardware/gpio.h"

#include "btstack.h"

#include "CDTV-BLE-Mouse.h"
#include "CDTV-IR-Mouse.h"
#include "CDTV-Joystick.h"
#include "CDTV-CoreLink.h"
#include "CDTV-IR-Core1.h"

// =============================================================================
// Hardware Configuration
// =============================================================================
#define LED_GPIO 25 // Status LED GPIO (non-W variants only)

// =============================================================================
// Board identification (compile-time)
// =============================================================================
#ifdef CYW43_ENABLE_BLUETOOTH
static const char * const board_name_str = "Pico W / Pico 2 W";
#else
static const char * const board_name_str = "Pico / Pico 2 (no wireless)";
#endif

// =============================================================================
// LED Controller
// Non-blocking blink via btstack_run_loop timer (BLE-enabled mode).
// Joystick-only mode uses a frame-count tick instead (BTstack not running).
// =============================================================================
typedef enum
{
    LED_PATTERN_OFF,
    LED_PATTERN_SLOW_BLINK,    // 1 Hz (500 ms) — booting / initialising
    LED_PATTERN_FAST_BLINK,    // 4 Hz (125 ms) — scanning / pairing
    LED_PATTERN_SOLID,         // Connected and active
} led_pattern_t;

static led_pattern_t g_led_pattern = LED_PATTERN_OFF;
static bool g_led_state = false;
static btstack_timer_source_t g_led_timer;

static void set_led_hw(bool on)
{
#ifdef CYW43_ENABLE_BLUETOOTH
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
#else
    gpio_put(LED_GPIO, on ? 1 : 0);
#endif
}

static void led_timer_callback(btstack_timer_source_t *ts)
{
    switch (g_led_pattern)
    {
    case LED_PATTERN_SOLID:
        set_led_hw(true);
        return;
    case LED_PATTERN_OFF:
        set_led_hw(false);
        return;
    case LED_PATTERN_SLOW_BLINK:
        g_led_state = !g_led_state;
        set_led_hw(g_led_state);
        btstack_run_loop_set_timer(ts, 500);
        btstack_run_loop_add_timer(ts);
        return;
    case LED_PATTERN_FAST_BLINK:
        g_led_state = !g_led_state;
        set_led_hw(g_led_state);
        btstack_run_loop_set_timer(ts, 125);
        btstack_run_loop_add_timer(ts);
        return;
    default:
        return;
    }
}

static void led_set_pattern(led_pattern_t pattern)
{
    g_led_pattern = pattern;
    btstack_run_loop_remove_timer(&g_led_timer);
    btstack_run_loop_set_timer_handler(&g_led_timer, led_timer_callback);
    btstack_run_loop_set_timer(&g_led_timer, 1);
    btstack_run_loop_add_timer(&g_led_timer);
}

static void init_led_hw(void)
{
#ifdef CYW43_ENABLE_BLUETOOTH
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
#else
    gpio_init(LED_GPIO);
    gpio_set_dir(LED_GPIO, GPIO_OUT);
    gpio_put(LED_GPIO, 0);
#endif
}

// =============================================================================
// Boot-Time Button Sample
// =============================================================================
static bool boot_sample_fire_button(void)
{
    // JOY2_FIRE1 triggers BLE pairing mode when held at boot.
    // JOY1 is reserved for normal joystick use.
    // Active-low: GPIO low = button pressed.
    return !gpio_get(JOY2_FIRE1);
}

// =============================================================================
// Mouse Input — core 0 is INPUT ONLY. The Bluetooth callback forwards each
// report's raw deltas and buttons straight to the core link. No accumulation,
// scaling, negation or clamping happens here; that is all core 1's job. (Mouse
// sensitivity lives in CDTV-IR-Core1.c.)
// =============================================================================
void mouse_input_callback(int16_t dx, int16_t dy, int8_t wheel, uint8_t buttons)
{
    corelink_mouse_push(dx, dy, wheel, buttons);
}


// =============================================================================
// Main
// =============================================================================
int main(void)
{
    stdio_init_all();

    // Step 1: Joystick GPIO init (all modes)
    joystick_init();

    // Step 2: Settle pull-up lines
    sleep_ms(5);

    // Step 3: Sample fire button before any wireless init
    bool fire_pressed = boot_sample_fire_button();

    sleep_ms(2000); // allow UART to settle
    printf("\n");
    printf("##############################################################\n");
    printf("##  CDTV BLE Mouse + Joystick Adapter                      ##\n");
    printf("##############################################################\n");
    printf("  Build   : " __DATE__ " " __TIME__ "\n");
    printf("  Board   : %s\n", board_name_str);
    printf("  Fire btn: %s\n", fire_pressed ? "PRESSED (pairing mode)" : "not pressed (joystick only)");
    printf("##############################################################\n\n");

    // Step 4: Init CYW43 once for LED control (needed in both modes on Pico W/2W).
    // Doing this before the mode branch ensures it is called exactly once,
    // preventing double-init problems on soft reset.
#ifdef CYW43_ENABLE_BLUETOOTH
    printf("Initializing CYW43...\n");
    if (cyw43_arch_init() != 0)
        printf("WARNING: CYW43 init failed — LED will not work\n");
    else
        init_led_hw();
#else
    init_led_hw();
#endif

    // Step 5: Branch on fire button
    if (!fire_pressed)
    {
        printf("Mode: Joystick-only (hold fire button at boot to pair BLE mouse)\n\n");
        led_set_pattern(LED_PATTERN_SOLID); // joystick active
        goto joystick_only_mode;
    }

#ifndef CYW43_ENABLE_BLUETOOTH
    printf("No wireless capability — joystick-only mode\n");
    goto joystick_only_mode;
#endif

    led_set_pattern(LED_PATTERN_SOLID); // booting — solid on

    // Step 6: Init IR transmitter and self-test
    ir_transmitter_init();
    ir_transmitter_selftest();

    // Step 7: Init BLE in pairing mode
    led_set_pattern(LED_PATTERN_FAST_BLINK); // scanning — fast flash
    printf("Boot: Pairing mode (timeout %d s) — put mouse in pairing mode\n",
           PAIRING_TIMEOUT_MS / 1000);
    if (!ble_mouse_init_pairing(mouse_input_callback))
    {
        printf("ERROR: BLE init failed — joystick-only mode\n");
        led_set_pattern(LED_PATTERN_SLOW_BLINK); // pair error
        goto joystick_only_mode;
    }

    // Step 8: Initialise the core link, then launch the IR output engine on
    // core 1. Core 1 owns the IR LED and all frame cadence from here on; core 0
    // does input only. This isolates IR transmission from CYW43/BLE background
    // interrupts, which on single-core stretched marks and jittered frame
    // spacing. (The IR PWM was initialised above; ir_core1_main re-inits
    // defensively, which is safe.)
    corelink_init();
    corelink_set_mouse_connected(false);
    multicore_launch_core1(ir_core1_main);

    printf("System ready — core 1 driving IR, core 0 scanning for mouse...\n\n");

    // -------------------------------------------------------------------------
    // BLE-enabled main loop
    // -------------------------------------------------------------------------
    while (true)
    {
        ble_mouse_process_events();

        // Update LED on connection state change
        static bool was_connected = false;
        bool is_connected = ble_mouse_is_connected();
        if (is_connected != was_connected)
        {
            was_connected = is_connected;
            if (is_connected)
            {
                led_set_pattern(LED_PATTERN_SOLID);
                printf("BLE: Mouse connected\n");
            }
            else
            {
                led_set_pattern(LED_PATTERN_FAST_BLINK);
                printf("BLE: Mouse disconnected — rescanning\n");
            }
            // Publish connection state to core 1 so it knows whether to emit
            // mouse frames.
            corelink_set_mouse_connected(is_connected);
        }

        // Detect pairing timeout — turn LED off and keep running for joystick.
        static bool timed_out = false;
        if (!timed_out && ble_mouse_is_timed_out()) {
            timed_out = true;
            led_set_pattern(LED_PATTERN_SLOW_BLINK); // joystick still active
            printf("BLE: Pairing timed out — joystick still active (reboot to retry)\n");
        }

        // Mouse input is delivered to the link asynchronously by
        // mouse_input_callback. Joystick state is read directly by core 1
        // at frame time so no publish step is needed here.
        // ALL IR transmission and timing happens on core 1 — core 0 never
        // touches the IR LED.
        __wfi();
    }

    return 0;

    // -------------------------------------------------------------------------
    // Joystick-only mode — no BLE
    // Core 1 still owns the IR LED and frame cadence; core 0 publishes joystick
    // state and blinks the status LED. mouse_connected stays false, so core 1
    // emits only joystick frames.
    // -------------------------------------------------------------------------
joystick_only_mode:
    printf("Mode: Joystick-only\n\n");

    ir_transmitter_init();
    corelink_init();
    corelink_set_mouse_connected(false);
    multicore_launch_core1(ir_core1_main);

    // LED is already set to SOLID by the mode branch above (or to the last
    // pattern set before a BLE error fallthrough). Nothing more needed here.

    while (true)
    {
        corelink_joystick_set((uint16_t)(joystick_read_all() & 0x0FFF));
        sleep_ms(5);
    }
}