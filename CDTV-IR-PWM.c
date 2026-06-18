// =============================================================================
/*
 * Shared IR PWM Driver
 * Copyright (c) 2025 Korinel
 * SPDX-License-Identifier: MIT
 *
 * Generates the 40 kHz IR carrier on the LED_IR pin. All callers run on core 1
 * (CDTV-IR-Core1.c), which is the sole owner of the LED and serialises mouse and
 * joystick frames, so no locking is needed.
 */
// =============================================================================

#include "CDTV-IR-PWM.h"
#include "CDTV-Joystick.h"   // for the LED_IR pin constant
#include "pico/stdlib.h"      // busy_wait_us_32
#include "hardware/clocks.h"  // clock_get_hz
#include "hardware/gpio.h"    // gpio_set_function, GPIO_FUNC_PWM
#include "hardware/pwm.h"     // pwm_*

#define IR_FREQUENCY_HZ  40000u
#define IR_DUTY_PERCENT  33u

static uint32_t g_slice = 0;
static uint32_t g_chan  = 0;
static uint16_t g_level = 0;
static bool     g_ready = false;

void ir_pwm_init(void) {
    if (g_ready) return;

    gpio_set_function(LED_IR, GPIO_FUNC_PWM);
    g_slice = pwm_gpio_to_slice_num(LED_IR);
    g_chan  = pwm_gpio_to_channel(LED_IR);

    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint16_t wrap   = (uint16_t)((sys_hz / IR_FREQUENCY_HZ) - 1u);
    g_level         = (uint16_t)((wrap * IR_DUTY_PERCENT) / 100u);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, wrap);
    pwm_init(g_slice, &cfg, true);
    pwm_set_chan_level(g_slice, g_chan, 0);  // carrier off

    g_ready = true;
}

void ir_pwm_emit(uint32_t mark_us, uint32_t space_us) {
    // Plain emit, matching the proven PS/2 trackball reference. This runs on
    // core 1, which carries no CYW43/BLE background interrupts, so there is
    // nothing to stretch the mark. No interrupt protection required.
    pwm_set_chan_level(g_slice, g_chan, g_level);
    busy_wait_us_32(mark_us);
    pwm_set_chan_level(g_slice, g_chan, 0);
    busy_wait_us_32(space_us);
}

void ir_pwm_mark_only(uint32_t mark_us) {
    pwm_set_chan_level(g_slice, g_chan, g_level);
    busy_wait_us_32(mark_us);
    pwm_set_chan_level(g_slice, g_chan, 0);
}