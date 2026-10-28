#!/bin/sh -e

# pick genimage layout by rootfs size (v4 glibc/openjdk is 2.4G, others 1.5G)
GENIMAGE_CFG="board/intel/galileo/genimage.cfg"
if [ "$(stat -c %s ${BINARIES_DIR}/rootfs.ext2)" -gt 1600000000 ]; then
    GENIMAGE_CFG="board/intel/galileo/genimage-v4.cfg"
fi
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${TARGET_DIR}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"
