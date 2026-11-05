# flipplay / keywait

Pre-baked video playback for the Galileo Gen 2 + RX 580.

Every frame is copied **once** into its own KMS framebuffer (VRAM); playback
then only issues `drmModePageFlip`, so the per-frame cost is an ioctl instead
of a YUV→RGB conversion plus a memcpy.

Measured on the board (640×480 RGBA, 30 fps, 240 frames):

| path | user CPU | sys CPU |
|---|---|---|
| mpv `--vo=drm` (converts each frame) | ~85–98% | — |
| flipplay (page flips only) | **0%** | **~9%** |

## Build

    SYS=<buildroot>/host/i586-buildroot-linux-gnu/sysroot
    HOST=<buildroot>/host/bin/i586-buildroot-linux-gnu-gcc
    $HOST --sysroot=$SYS -O2 -I$SYS/usr/include/libdrm flipplay.c -o flipplay -L$SYS/usr/lib -ldrm
    $HOST --sysroot=$SYS -O2 keywait.c -o keywait

## Bake a clip from a video

    ffmpeg -i in.mp4 -vf scale=640:480 -pix_fmt bgra -f rawvideo out.rgb

240 frames = 281 MB = 8 s of video. On the board that bake took 158 s
(~20× slower than real time); do it on a workstation instead.

Playback needs `seconds = 0` (loop) plus any-key exit, which `flip-demo`
wires up: it switches to VT1, starts flipplay, waits on `keywait`, then
restarts the desktop.
