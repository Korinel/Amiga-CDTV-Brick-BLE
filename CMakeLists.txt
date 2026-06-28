# CMakeLists.txt — CDTV BLE Mouse + Joystick Adapter
# Targets the Raspberry Pi Pico W / Pico 2 W (CYW43 wireless).
# See README for build prerequisites (Pico SDK, ARM toolchain, picotool).
#
# Produces two .uf2 binaries in one build:
#   CDTV-BLE-Mouse-v2.uf2 — Version 2 board: UART on GP0/GP1, joystick on GP2-GP7/GP10-GP15
#   CDTV-BLE-Mouse-v1.uf2 — Version 1 board: joystick on GP0-GP5/GP10-GP15, IR on GP20

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Pull in the Pico SDK before calling project()
include(pico_sdk_import.cmake)

project(CDTV-BLE-Mouse C CXX ASM)

pico_sdk_init()

# =============================================================================
# Helper function — avoids repeating identical settings for each board target
# =============================================================================
function(configure_cdtv_target TARGET_NAME BOARD_VER)

    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/..   # for btstack_config_common.h
    )

    pico_set_program_name(${TARGET_NAME} "CDTV-BLE-Mouse")
    pico_set_program_version(${TARGET_NAME} "0.1")

    # Serial output over UART (GP0/GP1). USB CDC disabled — it shares
    # the same stdio sink and is not needed for this embedded application.
    pico_enable_stdio_uart(${TARGET_NAME} 1)
    pico_enable_stdio_usb(${TARGET_NAME} 0)

    target_link_libraries(${TARGET_NAME}
        pico_stdlib
        pico_multicore
        pico_cyw43_arch_lwip_threadsafe_background
        pico_btstack_ble
        pico_btstack_cyw43
        hardware_clocks
        hardware_pwm
        hardware_gpio
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE
        RUNNING_AS_CLIENT=1               # btstack_config_common.h: enable GATT central
        CYW43_HOST_NAME="CDTV-Mouse"
        BOARD_VERSION=${BOARD_VER}        # GPIO layout: see CDTV-Joystick.h

        # Optional diagnostics (disabled by default — both add verbose UART output).
        # Uncomment to enable:
        #   DEBUG_BLE=1         per-report log of every decoded mouse input
        #   DEBUG_BLE_RATE=1    periodic summary of real BLE report delivery rate
    )

    pico_add_extra_outputs(${TARGET_NAME})

endfunction()

# =============================================================================
# Source file list (shared by both targets)
# =============================================================================
set(CDTV_SOURCES
    main.c
    CDTV-IR-Mouse.c
    CDTV-IR-PWM.c
    CDTV-BLE-Mouse.c
    CDTV-Joystick.c
    CDTV-CoreLink.c
    CDTV-IR-Core1.c
)

# =============================================================================
# Version 2 board — UART on GP0/GP1, joystick on GP2-GP7/GP10-GP15, IR on GP16
# =============================================================================
add_executable(CDTV-BLE-Mouse-v2 ${CDTV_SOURCES})
configure_cdtv_target(CDTV-BLE-Mouse-v2 2)

# =============================================================================
# Version 1 board — joystick on GP0-GP5/GP10-GP15, IR on GP20
# =============================================================================
add_executable(CDTV-BLE-Mouse-v1 ${CDTV_SOURCES})
configure_cdtv_target(CDTV-BLE-Mouse-v1 1)