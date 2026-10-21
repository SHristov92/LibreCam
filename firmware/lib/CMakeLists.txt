# Hardware-independent core: config, Wi-Fi logic, HTTP, RTSP, RTP/JPEG, web UI.
# Needs LC_HAL_DIR (for hal_net.h) and LC_BOARD_DIR (for board_config.h).
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WEB_UI_C ${CMAKE_CURRENT_BINARY_DIR}/web_ui.c)
add_custom_command(
    OUTPUT ${WEB_UI_C}
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_web.py
            ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${WEB_UI_C}
    DEPENDS web/index.html tools/embed_web.py)

add_library(lc_core STATIC
    src/app_main.c
    src/audio.c
    src/avi.c
    src/clock.c
    src/recorder.c
    src/config.c
    src/http_server.c
    src/image.c
    src/led.c
    src/mdns.c
    src/mdns_msg.c
    src/net.c
    src/rtp_jpeg.c
    src/rtsp_server.c
    src/sd_config.c
    ${WEB_UI_C})
target_include_directories(lc_core
    PUBLIC include ${LC_BOARD_DIR}
    PRIVATE ${LC_HAL_DIR})
target_compile_definitions(lc_core PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(lc_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(test_rtp_jpeg tests/test_rtp_jpeg.c src/rtp_jpeg.c)
target_include_directories(test_rtp_jpeg PRIVATE include)
add_test(NAME rtp_jpeg COMMAND test_rtp_jpeg ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/frame.jpg)

add_executable(test_led tests/test_led.c src/led.c)
target_include_directories(test_led PRIVATE include)
add_test(NAME led COMMAND test_led)

add_executable(test_alaw tests/test_alaw.c)
target_include_directories(test_alaw PRIVATE include)
add_test(NAME alaw COMMAND test_alaw)

add_executable(test_sd_config tests/test_sd_config.c src/sd_config.c src/config.c)
target_include_directories(test_sd_config PRIVATE include ${LC_BOARD_DIR})
add_test(NAME sd_config COMMAND test_sd_config)

add_executable(test_mdns tests/test_mdns.c src/mdns_msg.c)
target_include_directories(test_mdns PRIVATE include)
add_test(NAME mdns COMMAND test_mdns)
