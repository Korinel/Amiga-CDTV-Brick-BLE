# CMakeLists.txt — CDTV BLE Mouse + Joystick Adapter
# Targets the Raspberry Pi Pico W / Pico 2 W (CYW43 wireless).
# See README for build prerequisites (Pico SDK, ARM toolchain, picotool).

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

add_executable(CDTV-BLE-Mouse
    main.c
    CDTV-IR-Mouse.c
    CDTV-IR-PWM.c
    CDTV-BLE-Mouse.c
    CDTV-Joystick.c
    CDTV-CoreLink.c
    CDTV-IR-Core1.c
)

target_include_directories(CDTV-BLE-Mouse PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..   # for btstack_config_common.h
)

pico_set_program_name(CDTV-BLE-Mouse "CDTV-BLE-Mouse")
pico_set_program_version(CDTV-BLE-Mouse "0.1")

# Serial output over UART (GP0/GP1). USB CDC disabled — it shares
# the same stdio sink and is not needed for this embedded application.
pico_enable_stdio_uart(CDTV-BLE-Mouse 1)
pico_enable_stdio_usb(CDTV-BLE-Mouse 0)

target_link_libraries(CDTV-BLE-Mouse
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    pico_btstack_ble
    pico_btstack_cyw43
    hardware_clocks
    hardware_pwm
    hardware_gpio
)

target_compile_definitions(CDTV-BLE-Mouse PRIVATE
    RUNNING_AS_CLIENT=1               # btstack_config_common.h: enable GATT central
    PICO_DEFAULT_UART=0               # UART0 on GP0/GP1
    PICO_DEFAULT_UART_TX_PIN=0
    PICO_DEFAULT_UART_RX_PIN=1
    CYW43_HOST_NAME="CDTV-Mouse"
    BOARD_VERSION=2                   # GPIO layout: see CDTV-Joystick.h

    # Optional diagnostics (disabled by default — both add verbose UART output).
    # Uncomment to enable:
    #   DEBUG_BLE=1         per-report log of every decoded mouse input
    #   DEBUG_BLE_RATE=1    periodic summary of real BLE report delivery rate
)

pico_add_extra_outputs(CDTV-BLE-Mouse)
