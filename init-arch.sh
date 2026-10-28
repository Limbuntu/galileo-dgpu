#!/bin/bash -e
groupadd users
useradd -m galileo
echo "galileo:galileo" | chpasswd
echo "root:root" | chpasswd
echo "nameserver 8.8.8.8" > /etc/resolv.conf
echo "galileo" > /etc/hostname
echo "127.0.0.1       localhost localhost.localdomain galileo" > /etc/hosts
echo "::1             localhost localhost.localdomain galileo" >> /etc/hosts
mkdir /etc/systemd/system/multi-user.target.wants
ln -s /usr/lib/systemd/system/sshd.service /etc/systemd/system/multi-user.target.wants/sshd.service
sed -i 's/auto/i486/g' /etc/pacman.conf
cat <<EOT >> /etc/systemd/network/20-wired.network
[Match]
Name=en*

[Network]
DHCP=yes
EOT
