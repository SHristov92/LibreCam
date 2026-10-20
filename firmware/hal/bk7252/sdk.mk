# Included by the Beken FreeRTOS SDK's application.mk (build.sh patches the
# include in). Paths are relative to the SDK root, where build.sh links this
# firmware tree as ./a9 and generates ./a9_gen.
A9       := ./a9
A9_BOARD ?= a9-mini-bk7252

INCLUDES += -I$(A9)/lib/include -I$(A9)/hal/bk7252 -I$(A9)/boards/$(A9_BOARD)

SRC_C += $(A9)/lib/src/app_main.c
SRC_C += $(A9)/lib/src/audio.c
SRC_C += $(A9)/lib/src/avi.c
SRC_C += $(A9)/lib/src/clock.c
SRC_C += $(A9)/lib/src/recorder.c
SRC_C += $(A9)/lib/src/config.c
SRC_C += $(A9)/lib/src/http_server.c
SRC_C += $(A9)/lib/src/net.c
SRC_C += $(A9)/lib/src/rtp_jpeg.c
SRC_C += $(A9)/lib/src/rtsp_server.c
SRC_C += $(A9)/hal/bk7252/hal_bk7252.c
SRC_C += $(A9)/hal/bk7252/camera_bk7252.c
SRC_C += $(A9)/hal/bk7252/audio_bk7252.c
SRC_C += $(A9)/hal/bk7252/storage_bk7252.c
SRC_C += $(A9)/third_party/fatfs/ff.c
SRC_C += $(A9)/third_party/fatfs/ffunicode.c
SRC_C += ./a9_gen/web_ui.c

# user_main() in hal_bk7252.c overrides the SDK's weak one; force its object
# out of the archive.
LFLAGS += -Wl,-u,a9_bk_entry
