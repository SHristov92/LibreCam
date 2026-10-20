# Host (Linux) build. Selects a board, which selects its HAL:
#   cmake -S . -B build -DA9_BOARD=host
# MCU targets are built with their vendor SDK; see hal/<mcu>/README.md.
cmake_minimum_required(VERSION 3.13)
project(a9_firmware C)

set(CMAKE_C_STANDARD 99)
set(A9_BOARD host CACHE STRING "Board directory under boards/")

set(A9_BOARD_DIR ${CMAKE_SOURCE_DIR}/boards/${A9_BOARD})
if(NOT EXISTS ${A9_BOARD_DIR}/board.cmake)
    message(FATAL_ERROR "Unknown board '${A9_BOARD}'")
endif()
include(${A9_BOARD_DIR}/board.cmake)
set(A9_HAL_DIR ${CMAKE_SOURCE_DIR}/hal/${A9_HAL})
if(NOT EXISTS ${A9_HAL_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "HAL '${A9_HAL}' is built with its vendor SDK, see hal/${A9_HAL}/README.md")
endif()

enable_testing()
add_subdirectory(lib)
add_subdirectory(${A9_HAL_DIR} hal)
