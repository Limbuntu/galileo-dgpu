#!/bin/sh -e

cp board/intel/galileo/grub.cfg ${BINARIES_DIR}/efi-part/EFI/BOOT/grub.cfg

# Trim amdgpu firmware to Polaris10 only (RX 580). The full linux-firmware
# amdgpu/ directory is >100 MB; we only ever load polaris10_* on this board.
# The radeon/ directory is kept whole (~15 MB) so all HD 6450 (Caicos)
# firmware variants are available.
if [ -d "${TARGET_DIR}/lib/firmware/amdgpu" ]; then
	find "${TARGET_DIR}/lib/firmware/amdgpu" \
		\( -type f -o -type l \) ! -name 'polaris10*' -delete
fi

# Remove buildroot's default X startup script: it starts Xorg on vt01 where
# it fights the respawning tty1 getty (display signal cycling) and grabs
# display 0 so our S50desktop cannot start its own server.
rm -f ${TARGET_DIR}/etc/init.d/S40xorg

# glibc gconv modules (iconv needs them; buildroot doesn't install to target)
if [ -d "${STAGING_DIR}/usr/lib/gconv" ] && [ ! -d "${TARGET_DIR}/usr/lib/gconv" ]; then
    cp -a "${STAGING_DIR}/usr/lib/gconv" "${TARGET_DIR}/usr/lib/"
fi
