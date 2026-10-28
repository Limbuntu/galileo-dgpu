#!/bin/bash -e
function cleanup {
  umount /tmp/mnt || true
  kpartx -d /dev/loop0 || true
  losetup -d /dev/loop0 || true
  rm -rf /tmp/mnt_backup || true
  rmdir /tmp/mnt || true
}

trap cleanup EXIT
cp output/images/sdcard.img output/images/sdcard-alpine.img
dd if=/dev/zero bs=1G count=1 >> output/images/sdcard-alpine.img
kpartx -a output/images/sdcard-alpine.img
mkdir /tmp/mnt
mount /dev/mapper/loop0p2 /tmp/mnt
mkdir /tmp/mnt_backup
cp -var /tmp/mnt/lib/modules /tmp/mnt_backup/lib_modules
cp -var /tmp/mnt/boot /tmp/mnt_backup/boot
rm -rf /tmp/mnt/*
./alpine_build_tools/apk.static -X http://dl-cdn.alpinelinux.org/alpine/v3.13/main -U --allow-untrusted -p /tmp/mnt --initdb add alpine-base openssh bash chrony
cp -var /tmp/mnt_backup/boot /tmp/mnt/boot
cp -var /tmp/mnt_backup/lib_modules /tmp/mnt/lib/modules
cat init-alpine.sh | chroot /tmp/mnt /bin/bash -

