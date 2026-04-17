#!/bin/bash
# Guest-side VA-API decode test. Runs inside the VM via ssh.
set -u
CLIP=/tmp/bbb.mp4

if [[ ! -f "$CLIP" ]]; then
  echo "--- downloading test clip ---"
  curl -sLo "$CLIP" 'https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4' \
    || { echo "curl failed"; exit 1; }
fi

echo "=== 1. vainfo ==="
vainfo 2>&1 | grep -v '^error:' | tail -15

echo
echo "=== 2. ffmpeg VAAPI decode (download to yuv420p / CPU) ==="
# Forcing output to software surface — matches our CPU-readback backend.
ffmpeg -hide_banner -loglevel info \
  -hwaccel vaapi -hwaccel_output_format yuv420p \
  -i "$CLIP" -frames:v 50 -f null - 2>&1 | grep -iE "hwaccel|vaapi|error|frame=" | tail -10

echo
echo "=== 3. ffmpeg VAAPI decode (keep on GPU) ==="
ffmpeg -hide_banner -loglevel info \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -i "$CLIP" -frames:v 50 -f null - 2>&1 | grep -iE "hwaccel|vaapi|error|frame=" | tail -10

echo
echo "=== 4. mpv hwdec=vaapi-copy frames=30 ==="
mpv --hwdec=vaapi-copy --vo=null --ao=null --msg-level=vo=v:ffmpeg=v \
    --no-config --frames=30 "$CLIP" 2>&1 | grep -iE "hwdec|vaapi|vo " | tail -10

echo
echo "=== 5. mpv default hwdec ==="
mpv --hwdec=auto --vo=null --ao=null --msg-level=vo=v:ffmpeg=v \
    --no-config --frames=30 "$CLIP" 2>&1 | grep -iE "hwdec|vaapi|using " | tail -10
