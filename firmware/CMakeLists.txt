# Host (Linux) build. Selects a board, which selects its HAL:
#   cmake -S . -B build -DA9_BOARD=host
# MCU targets are built with their vendor SDK; see hal/<mcu>/README.md.
cmake_minimum_required(VERSION 3.13)
project(lc_firmware C)

set(CMAKE_C_STANDARD 99)
set(LC_BOARD host CACHE STRING "Board directory under boards/")

set(LC_BOARD_DIR ${CMAKE_SOURCE_DIR}/boards/${LC_BOARD})
if(NOT EXISTS ${LC_BOARD_DIR}/board.cmake)
    message(FATAL_ERROR "Unknown board '${LC_BOARD}'")
endif()
include(${LC_BOARD_DIR}/board.cmake)
set(LC_HAL_DIR ${CMAKE_SOURCE_DIR}/hal/${LC_HAL})
if(NOT EXISTS ${LC_HAL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "HAL '${LC_HAL}' is built with its vendor SDK, see hal/${LC_HAL}/README.md")
endif()

enable_testing()
add_subdirectory(lib)
add_subdirectory(${LC_HAL_DIR} hal)
