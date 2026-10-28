#!/bin/bash -e
adduser -D galileo
echo "galileo:galileo" | chpasswd
echo "root:root" | chpasswd
sed -i '/^tty/d' /etc/inittab
echo "ttyS1::respawn:/sbin/getty -L ttyS1 115200 vt100" >> /etc/inittab
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "galileo" > /etc/hostname
echo "127.0.0.1       localhost localhost.localdomain galileo" > /etc/hosts
echo "::1             localhost localhost.localdomain galileo" >> /etc/hosts
echo "auto lo" > /etc/network/interfaces
echo "iface lo inet loopback" >> /etc/network/interfaces
echo "auto eth0" >> /etc/network/interfaces
echo "iface eth0 inet dhcp" >> /etc/network/interfaces
echo "http://dl-cdn.alpinelinux.org/alpine/v3.13/main" > /etc/apk/repositories
echo "http://dl-cdn.alpinelinux.org/alpine/v3.13/community" >> /etc/apk/repositories
rc-update add devfs sysinit
rc-update add dmesg sysinit
rc-update add mdev sysinit

rc-update add hwclock boot
rc-update add modules boot
rc-update add sysctl boot
rc-update add hostname boot
rc-update add bootmisc boot
rc-update add syslog boot
rc-update add networking boot
rc-update add sshd boot
rc-update add chronyd boot

rc-update add mount-ro shutdown
rc-update add killprocs shutdown
rc-update add savecache shutdown

