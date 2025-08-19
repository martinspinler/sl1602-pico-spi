set(PROJECT sl1602_spi_bridge)
cmake_minimum_required(VERSION 3.12)
set(PICO_BOARD pico_w)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(${PROJECT} C CXX ASM)

pico_sdk_init()
add_executable(${PROJECT} main.cpp)

pico_generate_pio_header(sl1602_spi_bridge ${CMAKE_CURRENT_LIST_DIR}/mux.pio)

target_link_libraries(${PROJECT} pico_stdlib hardware_spi hardware_pio)
pico_add_extra_outputs(${PROJECT})
pico_enable_stdio_usb(${PROJECT} 1)
pico_enable_stdio_uart(${PROJECT} 0)
