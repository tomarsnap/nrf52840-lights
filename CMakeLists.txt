cmake_minimum_required(VERSION 3.20.0)

# Add this project root as a board root so boards/nicekeyboards/nice_nano/ is found
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Add this project root as a DTS root so the in-tree binding under
# dts/bindings/ (blled,ws2812-i2s-runtime) is found by the devicetree compiler.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(bl_led VERSION 0.1.0)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
    src/main.c
    src/led_effects.c
    src/led_animations.c
    src/ble_service.c
    src/battery.c
    # In-tree fork of the WS2812 I2S driver with a runtime-switchable colour
    # order (GRB / RGB / GRBW / RGBW). See drivers/ws2812_i2s_runtime/.
    drivers/ws2812_i2s_runtime/ws2812_i2s_runtime.c
)
