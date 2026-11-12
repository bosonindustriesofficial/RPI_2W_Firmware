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

# ON  = micro USB OTG HID scanner (printf on UART0 @ 115200, GP0 TX / GND).
# OFF = VS Code USB serial on micro USB (no scanner host).
option(BOSON_USB_HID_HOST "USB OTG HID barcode scanner on micro USB" ON)

include(pico_sdk_import.cmake)

project(node_firmware C CXX ASM)

pico_sdk_init()

add_executable(node_firmware node_firmware.cpp)

pico_set_program_name(node_firmware "node_firmware")
pico_set_program_version(node_firmware "1.0.0")

set(NODE_LIBS pico_stdlib pico_cyw43_arch_lwip_threadsafe_background)

if(BOSON_USB_HID_HOST)
    target_compile_definitions(node_firmware PRIVATE BOSON_USB_HID_HOST=1)
    pico_enable_stdio_uart(node_firmware 1)
    pico_enable_stdio_usb(node_firmware 0)
    list(APPEND NODE_LIBS tinyusb_host)
else()
    target_compile_definitions(node_firmware PRIVATE BOSON_USB_HID_HOST=0)
    pico_enable_stdio_uart(node_firmware 0)
    pico_enable_stdio_usb(node_firmware 1)
endif()

target_link_libraries(node_firmware ${NODE_LIBS})

target_include_directories(node_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(node_firmware)
