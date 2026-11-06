# Included by OpenTXW81X's project/Makefile in place of OpenBeken's
# platforms/TXW81X/OpenBeken.mk (build.sh rewrites that include line).
# Paths are relative to <sdk>/project; build.sh copies this firmware tree to
# <sdk>/lc and generates <sdk>/lc_gen.
LC       := ../lc
LC_BOARD ?= txw817-cam

INCLUDES += -I$(LC)/hal/txw81x/compat -I$(LC)/lib/include -I$(LC)/hal/txw81x -I$(LC)/boards/$(LC_BOARD) -I../lc_gen
CFLAGS   += -DLC_PORT_TXW81X

SRC_C += $(LC)/lib/src/app_main.c
SRC_C += $(LC)/lib/src/audio.c
SRC_C += $(LC)/lib/src/avi.c
SRC_C += $(LC)/lib/src/clock.c
SRC_C += $(LC)/lib/src/recorder.c
SRC_C += $(LC)/lib/src/config.c
SRC_C += $(LC)/lib/src/http_server.c
SRC_C += $(LC)/lib/src/image.c
SRC_C += $(LC)/lib/src/kv_flash.c
SRC_C += $(LC)/lib/src/led.c
SRC_C += $(LC)/lib/src/mdns.c
SRC_C += $(LC)/lib/src/mdns_msg.c
SRC_C += $(LC)/lib/src/net.c
SRC_C += $(LC)/lib/src/rtp_jpeg.c
SRC_C += $(LC)/lib/src/rtsp_server.c
SRC_C += $(LC)/lib/src/sd_config.c
SRC_C += $(LC)/hal/txw81x/hal_txw81x.c
SRC_C += $(LC)/hal/txw81x/camera_txw81x.c
SRC_C += $(LC)/hal/txw81x/stubs_txw81x.c
SRC_C += ../lc_gen/web_ui.c
