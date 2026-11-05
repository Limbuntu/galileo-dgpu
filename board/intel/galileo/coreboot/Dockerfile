# Toolchain image for building the Galileo Gen 2 coreboot ROM.
#
# Why coreboot at all: the board ships UEFI-only firmware with no CSM, and DOS
# needs real-mode BIOS services, so running FreeDOS/MS-DOS natively means
# replacing the firmware. coreboot 4.20.x is the last series that still carries
# src/soc/intel/quark + src/mainboard/intel/galileo (4.21 removed them).
#
# This image only provides the toolchain; build-rom.sh extracts the coreboot
# 4.20.1 release tarball (provisioned on the CI host at /opt/git-assets, or
# fetched from coreboot.org as fallback), injects the vendored FSP/RMU blobs and
# produces /out/coreboot.rom.
FROM debian:12

ENV DEBIAN_FRONTEND=noninteractive

# Domestic mirror: deb.debian.org is slow/unreliable from the CI runner.
RUN sed -i 's|deb.debian.org|mirrors.tuna.tsinghua.edu.cn|g' \
           /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        git build-essential gcc-multilib libc6-dev-i386 \
        python3 python-is-python3 nasm iasl libssl-dev pkg-config zlib1g-dev \
        m4 bison flex bc libelf-dev curl ca-certificates xz-utils \
    && rm -rf /var/lib/apt/lists/*

# coreboot 4.20.x builds fine with the distribution's GNU make (4.3); the
# make-4.2.1 workaround was only needed for the 4.7 tree.

COPY build-rom.sh compose-flash.sh /usr/local/bin/
COPY blobs /usr/local/bin/blobs
RUN chmod +x /usr/local/bin/build-rom.sh /usr/local/bin/compose-flash.sh

CMD ["/usr/local/bin/build-rom.sh"]
