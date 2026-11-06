#!/bin/sh
# cross-build minplayer with the gres-builder docker toolchain.
# usage: ./build.sh
set -e
SYS=/out/workstation/host/i586-buildroot-linux-gnu/sysroot
CC=/out/workstation/host/bin/i586-buildroot-linux-gnu-gcc

docker run --rm -v gres-out:/out -v "$(pwd)/../..:/host" gres-builder sh -c "
  set -e
  S=$SYS
  CC=$CC
  \$CC --sysroot=\$S -O2 -D_GNU_SOURCE \\
      -I\$S/usr/include/libdrm -I\$S/usr/include -I\$S/usr/include/va \\
      -I\$S/usr/include/libavcodec -I\$S/usr/include/libavformat -I\$S/usr/include/libavutil \\
      -c /host/board/intel/galileo/minplayer/minplayer.c \\
      -o /host/board/intel/galileo/minplayer/minplayer.o
  \$CC --sysroot=\$S /host/board/intel/galileo/minplayer/minplayer.o \\
      -o /host/board/intel/galileo/minplayer/minplayer \\
      -L\$S/usr/lib -lavcodec -lavformat -lavutil -lva -lva-drm \\
      -lEGL -lGL -lgbm -ldrm -lpthread -ldl -lm
"
file board/intel/galileo/minplayer/minplayer
echo "build ok"