cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(node_firmware C CXX ASM)

pico_sdk_init()

add_executable(node_firmware node_firmware.cpp)

pico_set_program_name(node_firmware "node_firmware")
pico_set_program_version(node_firmware "1.0.0")

pico_enable_stdio_uart(node_firmware 1)
pico_enable_stdio_usb(node_firmware 0)

target_link_libraries(node_firmware
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    tinyusb_host
)

target_include_directories(node_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(node_firmware)
