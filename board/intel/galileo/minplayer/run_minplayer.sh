#!/bin/sh
# run_minplayer.sh: stop X, run minplayer for SECS seconds on the video,
# write /root/minplayer-result.txt with CPU% and frames presented.
# usage: run_minplayer.sh [seconds] [video]
SECS="${1:-20}"
VIDEO="${2:-/root/media/bbb_hw.mp4}"

export LIBVA_DRIVER_NAME=radeonsi
export LIBVA_DRIVERS_PATH=/usr/lib/dri

# stop X (drmModeSetCrtc returns EACCES while Xorg owns the CRTC)
killall Xorg 2>/dev/null
sleep 1
killall X 2>/dev/null
sleep 1

/usr/bin/minplayer "$VIDEO" /root/minplayer-result.txt "$SECS" > /tmp/minplayer.log 2>&1
echo "minplayer exit=$?"
echo "=== log ==="
grep -E 'VA-API|video|connector|done:|result file|drmMode|failed' /tmp/minplayer.log
echo "=== result ==="
cat /root/minplayer-result.txt 2>/dev/null
exit 0