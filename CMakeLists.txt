# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-socket-react)

set(ACTIVE_ROM_IMAGE "${CMAKE_SOURCE_DIR}/roms/active.gb")
if(EXISTS "${ACTIVE_ROM_IMAGE}")
    esptool_py_flash_to_partition(flash "rom" "${ACTIVE_ROM_IMAGE}")
endif()
