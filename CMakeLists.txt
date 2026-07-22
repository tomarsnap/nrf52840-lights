cmake_minimum_required(VERSION 3.20.0)

# Add this project root as a board root so boards/nicekeyboards/nice_nano/ is found
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(bl_led VERSION 0.1.0)

target_sources(app PRIVATE
    src/main.c
    src/led_effects.c
    src/ble_service.c
)
