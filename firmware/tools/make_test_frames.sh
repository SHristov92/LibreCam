#!/bin/sh
# Generates a looping set of baseline JPEG frames for the host build,
# similar to what the BK7252 JPEG encoder produces.
set -e
out=${1:-test_frames}
mkdir -p "$out"
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=640x480:rate=15 -t 4 \
    -pix_fmt yuvj420p -huffman default -q:v 5 "$out/frame_%04d.jpg"
echo "wrote $(ls "$out" | wc -l) frames to $out"
