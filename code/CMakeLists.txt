cmake_minimum_required(VERSION 3.12)

set(CMAKE_C_STANDARD 11)

set(PICO_PLATFORM rp2040)
set(BOARD pico_sdk)

include(pico_sdk_import.cmake)

set(PROJECT PM2040)

project(${PROJECT})

pico_sdk_init()

add_compile_options( -Ofast -Wall )

# For boards with crystals which take a bit longer to stablize.
add_compile_definitions(PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)

add_executable(${PROJECT})

# 16 MB flash, 20 slots, with GameKing (GK mini) support:
#   cmake .. -DPM2040_GK_MULTI20=ON
option(PM2040_GK_MULTI20 "20-slot multi-ROM firmware (16 MB flash) with GK mini support" OFF)

if(PM2040_GK_MULTI20)
  pico_set_linker_script(${PROJECT} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_16MBFlash.ld)
  target_compile_definitions(${PROJECT} PRIVATE PM2040_SLOTS20=1 PM2040_GKMINI=1 GK_PICO=1)
else()
pico_set_linker_script(${PROJECT} ${CMAKE_CURRENT_SOURCE_DIR}/memmap.ld)
#pico_set_linker_script(${PROJECT} ${CMAKE_CURRENT_SOURCE_DIR}/memmap_16MBFlash.ld)
endif()

pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/oe.pio ${CMAKE_CURRENT_LIST_DIR}/pushData.pio ${CMAKE_CURRENT_LIST_DIR}/hale.pio ${CMAKE_CURRENT_LIST_DIR}/lale.pio ${CMAKE_CURRENT_LIST_DIR}/lale_512k.pio ${CMAKE_CURRENT_LIST_DIR}/lale_menu.pio ${CMAKE_CURRENT_LIST_DIR}/writecheck.pio ${CMAKE_CURRENT_LIST_DIR}/writecheck_addr.pio)

pico_enable_stdio_usb(${PROJECT} 0)
pico_enable_stdio_uart(${PROJECT} 0)        

pico_add_extra_outputs(${PROJECT})

target_sources(${PROJECT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/main.c)

if(PM2040_GK_MULTI20)
  target_sources(${PROJECT} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gk_mode.c ${CMAKE_CURRENT_SOURCE_DIR}/gk_storage.c ${CMAKE_CURRENT_SOURCE_DIR}/gk.c)
  pico_generate_pio_header(${PROJECT} ${CMAKE_CURRENT_LIST_DIR}/lale_32k.pio)
  target_link_libraries(${PROJECT} pico_multicore)
endif()

target_link_libraries(${PROJECT} pico_stdlib hardware_pio hardware_dma)
