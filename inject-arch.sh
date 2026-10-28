#!/bin/bash -e
function cleanup {
  pkill gpg-agent || true
  sleep 5
  umount /tmp/mnt || true
  kpartx -d /dev/loop0 || true
  losetup -d /dev/loop0 || true
  rm -rf /tmp/mnt_backup || true
  rmdir /tmp/mnt || true
}

trap cleanup EXIT
cp output/images/sdcard.img output/images/sdcard-arch.img
dd if=/dev/zero bs=1G count=1 >> output/images/sdcard-arch.img
kpartx -a output/images/sdcard-arch.img
mkdir /tmp/mnt
mount /dev/mapper/loop0p2 /tmp/mnt
mkdir /tmp/mnt_backup
cp -var /tmp/mnt/lib/modules /tmp/mnt_backup/lib_modules
cp -var /tmp/mnt/boot /tmp/mnt_backup/boot
rm -rf /tmp/mnt/*
cd archbashstrap
./archbashstrap /tmp/mnt
cd ..
rm -rf /tmp/mnt/boot
cp -var /tmp/mnt_backup/boot /tmp/mnt/boot
cp -var /tmp/mnt_backup/lib_modules /tmp/mnt/lib/modules
cat init-arch.sh | chroot /tmp/mnt /bin/bash -

