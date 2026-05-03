set(GB_CORE_SRCS
    "core/apu.cpp"
    "core/cpu.cpp"
    "core/lcd.cpp"
    "core/mbc.cpp"
    "core/rom.cpp"
    "core/sdl.cpp"
    # interrupt.cpp / mem.cpp / timer.cpp are #include'd from cpu.cpp via the
    # INTER_MODULE_OPT optimization. We still list them here so CMake sees them
    # and re-runs the build on edits — they compile to empty object files when
    # INTER_MODULE_OPT is defined globally.
    "core/interrupt.cpp"
    "core/mem.cpp"
    "core/timer.cpp"
)

set(GB_HAL_SRCS
    "main.cpp"
    "emulator_runtime.cpp"
    "display.cpp"
    "cst816d.cpp"
    "touch_input.cpp"
    "web_portal.cpp"
)

idf_component_register(
    SRCS ${GB_HAL_SRCS} ${GB_CORE_SRCS}
    INCLUDE_DIRS "." "core"
    REQUIRES driver esp_lcd esp_wifi esp_event esp_netif esp_http_server
             esp_timer spiffs nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    "INTER_MODULE_OPT=1"
)

if(DEFINED GB_ENABLE_AUDIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "GB_ENABLE_AUDIO=${GB_ENABLE_AUDIO}"
    )
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    "-O3"
    "-Wno-unused-variable"
    "-Wno-unused-but-set-variable"
    "-Wno-unused-function"
    "-Wno-narrowing"
    "-Wno-write-strings"
)

set(WEB_UI_DIST_DIR "${CMAKE_SOURCE_DIR}/data")
if(EXISTS "${WEB_UI_DIST_DIR}")
    spiffs_create_partition_image(storage "${WEB_UI_DIST_DIR}" FLASH_IN_PROJECT)
endif()
