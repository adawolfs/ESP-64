set(N64_SRCS
    "main.cpp"
    "n64_runtime.cpp"
    "n64_joybus.cpp"
    "n64_controller.cpp"
    "n64_accessory.cpp"
    "transfer_pak.cpp"
    "gb_cartridge.cpp"
    "mbc1_mapper.cpp"
    "pokemon_stadium_compat.cpp"
    "web_portal.cpp"
    "web_assets.S"
)

set(WEB_UI_DIST_DIR "${CMAKE_SOURCE_DIR}/data")

idf_component_register(
    SRCS ${N64_SRCS}
    INCLUDE_DIRS "." "core"
    REQUIRES driver esp_wifi esp_event esp_netif esp_http_server
             esp_timer spiffs nvs_flash
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

if(EXISTS "${WEB_UI_DIST_DIR}")
    spiffs_create_partition_image(storage "${WEB_UI_DIST_DIR}" FLASH_IN_PROJECT)
endif()
