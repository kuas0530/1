cmake_minimum_required(VERSION 3.13)

# 引入 SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(host_hid C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# 💥 關鍵：自動下載 pico-pio-usb 函式庫
include(FetchContent)
FetchContent_Declare(
  pico_pio_usb
  GIT_REPOSITORY https://github.com/sekigon-gonnoc/pico-pio-usb
  GIT_TAG        main
)
FetchContent_MakeAvailable(pico_pio_usb)

# 建立執行檔
add_executable(host_hid
    src/main.c
    src/usb_descriptors.c
    src/hid_reports.c
)

# 包含路徑
target_include_directories(host_hid PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 連結庫 (包含 PIO USB)
target_link_libraries(host_hid
    pico_stdlib
    pico_multicore
    tinyusb_host
    tinyusb_device
    tinyusb_board
    pico_pio_usb  # <--- 連結這個庫
)

# 禁用 stdio USB 避免衝突
pico_enable_stdio_usb(host_hid 0)
pico_enable_stdio_uart(host_hid 1)

# 產生 uf2
pico_add_extra_outputs(host_hid)