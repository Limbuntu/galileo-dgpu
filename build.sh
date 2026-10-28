#!/bin/sh
# Galileo Gen 2 (Quark X1000) 完整系统构建脚本
# 前置: Docker Desktop 运行中, 已构建镜像 gres-builder (Debian bookworm + tuna 源)
# 产出: docker 卷 gres-out 中的 /out/galileo-rx580/images/sdcard.img
set -e
cd "$(dirname "$0")"
docker rm -f gres-build >/dev/null 2>&1 || true
docker run -d --name gres-build \
  -e FORCE_UNSAFE_CONFIGURE=1 \
  -v "$PWD":/build -v gres-out:/out -w /build \
  gres-builder sh -c '
    git config --global --add safe.directory /build
    make O=/out/galileo-rx580 BR2_DL_DIR=/out/dl galileo_rx580_defconfig &&
    make O=/out/galileo-rx580 BR2_DL_DIR=/out/dl -j"$(nproc)" &&
    echo BUILD_OK || echo BUILD_FAILED'
docker wait gres-build
docker logs gres-build 2>&1 | grep -E "BUILD_OK|BUILD_FAILED|Error" | tail -5
echo "镜像位置: docker run --rm -v gres-out:/out gres-builder ls /out/galileo-rx580/images/"
