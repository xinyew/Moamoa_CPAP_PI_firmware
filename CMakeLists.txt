cmake_minimum_required(VERSION 3.20.0)

# Adding board defined in this project to the board searching list
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kmm_pmask)
target_sources(app PRIVATE
    src/main.c
    src/drivers/bus_diag.c
    src/drivers/driver_batt.c
    src/drivers/driver_led.c
    src/drivers/driver_ms5611.c
    src/sensors/sensor_manager.c
    src/sensors/ppg_reader.c
    src/sensors/sht40_reader.c
    src/sensors/tmp117_reader.c
    src/comm/ble_manager.c
    src/comm/comm_manager.c
    src/comm/rtt_stream.c
)
# target_sources(app PRIVATE src/myfunc.c)
target_sources_ifdef(CONFIG_MYFUNC app PRIVATE src/myfunc.c)
