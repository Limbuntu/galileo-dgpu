# minplayer

Minimal VA-API + EGL/GLES + KMS page-flip video player for Galileo Gen 2 + RX 580.
Measures the CPU cost of mpv's zero-copy VAAPI path: FFmpeg VAAPI hw-decode →
`vaExportSurfaceHandle` dmabuf → EGL `EGL_LINUX_DMA_BUF_EXT` → GLES2 YUV→RGB
shader → GBM scanout BO → `drmModeAddFB2` → `drmModePageFlip`.

## Build (gres-builder docker image)

    ./build.sh          # runs the cross compile, emits minplayer binary

## Deploy + run on the board (root@<board-ip>)

    # copy the binary:
    #   cat board/intel/galileo/minplayer/minplayer |
    #     sshpass -p <board-password> ssh ... root@<board-ip> \
    #       'cat > /usr/bin/minplayer && chmod +x /usr/bin/minplayer'
    # then:
    sh run_minplayer.sh 20 /root/media/bbb_hw.mp4

The script kills Xorg (required: `drmModeSetCrtc` fails with EACCES while X owns
the CRTC), sets `LIBVA_DRIVER_NAME=radeonsi LIBVA_DRIVERS_PATH=/usr/lib/dri`,
runs minplayer for the requested seconds, and prints the result file.

## Result (measured on the board)

Run: 640x360 H.264 30 fps (`/root/media/bbb_hw.mp4`), 20 s, scanout 720x400@70
(no 640x360 mode on the EDID; the video is letterboxed into 720x400).

    # /root/minplayer-result.txt
    cpu 27.5% frames 90
    cpu 25.0% frames 150
    ... (per-second samples)
    frames_presented=600
    elapsed_s=20.0
    cpu_percent=24.2

vs mpv `--vo=gpu --gpu-context=drm --hwdec=vaapi`: ~98-101% CPU at 640x360.
The shell `time` ground truth for minplayer: user 6.91 s + sys 2.56 s over
24.2 s real (≈39% of one core incl. driver/kernel time).

**Conclusion: the decode/display pipeline itself costs ~25% (process utime+stime)
of one core at 30 fps 640x360 — the rest of mpv's ~100% is its own overhead
(playback core, VO/GPU machinery, audio, script/lua).**

## Notes / gotchas

- The sysroot has no libGLESv2 (Mesa built with `gles2` disabled). All GLES2
  entry points are resolved at runtime via `eglGetProcAddress` (Mesa's shared
  glapi dispatch) into function pointers.
- `vaSyncSurface` hangs with this radeonsi 20.2.2 driver; FFmpeg's VAAPI hwaccel
  already syncs, so minplayer skips it.
- FFmpeg stores the decoded `VASurfaceID` directly in `frame->data[3]` (as
  intptr_t), not as a pointer.
- The VA hw_frames_ctx is created lazily on the first decoded frame (FFmpeg 4.3);
  pre-creating it via `avcodec_get_hw_frames_parameters` trips an assert in
  `vaapi_decode_find_best_format` (`source_format == AV_PIX_FMT_NONE`).
- `gettimeofday` on this board is unreliable across a run (frozen-looking wall
  clock, no RTC/NTP), so timing uses the kernel's monotonic `/proc/uptime`.