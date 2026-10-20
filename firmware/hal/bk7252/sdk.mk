# Included by the Beken FreeRTOS SDK's application.mk (build.sh patches the
# include in). Paths are relative to the SDK root, where build.sh links this
# firmware tree as ./lc and generates ./lc_gen.
LC       := ./lc
LC_BOARD ?= a13-mini-bk7252

INCLUDES += -I$(LC)/lib/include -I$(LC)/hal/bk7252 -I$(LC)/boards/$(LC_BOARD)

SRC_C += $(LC)/lib/src/app_main.c
SRC_C += $(LC)/lib/src/audio.c
SRC_C += $(LC)/lib/src/avi.c
SRC_C += $(LC)/lib/src/clock.c
SRC_C += $(LC)/lib/src/recorder.c
SRC_C += $(LC)/lib/src/config.c
SRC_C += $(LC)/lib/src/http_server.c
SRC_C += $(LC)/lib/src/led.c
SRC_C += $(LC)/lib/src/net.c
SRC_C += $(LC)/lib/src/rtp_jpeg.c
SRC_C += $(LC)/lib/src/rtsp_server.c
SRC_C += $(LC)/hal/bk7252/hal_bk7252.c
SRC_C += $(LC)/hal/bk7252/camera_bk7252.c
SRC_C += $(LC)/hal/bk7252/audio_bk7252.c
SRC_C += $(LC)/hal/bk7252/storage_bk7252.c
SRC_C += $(LC)/third_party/fatfs/ff.c
SRC_C += $(LC)/third_party/fatfs/ffunicode.c
SRC_C += ./lc_gen/web_ui.c

# user_main() in hal_bk7252.c overrides the SDK's weak one; force its object
# out of the archive.
LFLAGS += -Wl,-u,lc_bk_entry
