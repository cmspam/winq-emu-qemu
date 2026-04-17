#!/bin/bash
# Alpha 6 VA-API acceptance test.
set -u
CLIP=/tmp/clip1080.mp4
[[ -f $CLIP ]] || curl -sLo "$CLIP" 'https://download.blender.org/durian/trailer/sintel_trailer-1080p.mp4' 2>/dev/null

echo "=== vainfo ==="
vainfo 2>&1 | grep -E 'VAProfile|Driver' | head -6

echo
echo "=== mpv HW vs SW frame time, 300 frames ==="
printf '%-18s' 'hwdec=no'
mpv --hwdec=no --vo=null --ao=null --no-config --frames=300 "$CLIP" 2>&1 | \
  awk '/AV:/ {last=$2} END {print " finished at", last}'

printf '%-18s' 'hwdec=vaapi-copy'
mpv --hwdec=vaapi-copy --vo=null --ao=null --no-config --frames=300 "$CLIP" 2>&1 | \
  awk '/AV:/ {last=$2} /Using hardware decoding/ {hw=$0} END {print " finished at", last; if (hw) print "   ", hw}'

echo
echo "=== confirm mpv actually picked vaapi-copy ==="
mpv --hwdec=vaapi-copy --vo=null --ao=null --no-config --frames=5 \
    --msg-level=vd=v "$CLIP" 2>&1 | grep -iE 'chose hwdec|using hwdec|hwdec api|selected' | head -5
