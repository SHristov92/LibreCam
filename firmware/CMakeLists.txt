# Host (Linux) build of the A9 firmware application, for testing the HTTP and
# RTSP servers without hardware. The BK7252 build lives in platform/bk7252.
cmake_minimum_required(VERSION 3.13)
project(a9_firmware C)

set(CMAKE_C_STANDARD 99)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/web_ui.c
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/embed_web.py
            ${CMAKE_SOURCE_DIR}/web/index.html ${CMAKE_BINARY_DIR}/web_ui.c
    DEPENDS ${CMAKE_SOURCE_DIR}/web/index.html ${CMAKE_SOURCE_DIR}/tools/embed_web.py)

set(APP_SOURCES
    src/app_main.c
    src/config.c
    src/http_server.c
    src/net_compat.c
    src/rtp_jpeg.c
    src/rtsp_server.c
    ${CMAKE_BINARY_DIR}/web_ui.c)

add_executable(a9_host ${APP_SOURCES} platform/host/hal_host.c platform/host/main.c)
target_include_directories(a9_host PRIVATE include)
target_compile_definitions(a9_host PRIVATE _GNU_SOURCE)
target_compile_options(a9_host PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(a9_host Threads::Threads)

add_executable(test_rtp_jpeg tests/test_rtp_jpeg.c src/rtp_jpeg.c)
target_include_directories(test_rtp_jpeg PRIVATE include)
enable_testing()
add_test(NAME rtp_jpeg COMMAND test_rtp_jpeg ${CMAKE_SOURCE_DIR}/tests/data/frame.jpg)
