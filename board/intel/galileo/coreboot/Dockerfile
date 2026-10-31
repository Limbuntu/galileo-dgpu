# Builds an 8 MB coreboot ROM for the Intel Galileo Gen 2 with a SeaBIOS payload.
#
# Why: the board ships UEFI-only firmware (no CSM), and DOS needs real-mode BIOS
# services, so the only way to run FreeDOS/MS-DOS natively is a BIOS implementation
# on the SPI flash. coreboot 4.4-4.7 are the releases that still carry
# src/soc/intel/quark + src/mainboard/intel/galileo.
#
# Debian 11 is archived (its image points at snapshot.debian.org, which is very
# slow from here), so build on bookworm with a domestic mirror instead.
FROM debian:12

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i -e 's|deb.debian.org|mirrors.tuna.tsinghua.edu.cn|g' \
           /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        git build-essential gcc-multilib libc6-dev-i386 \
        python3 nasm iasl libssl-dev pkg-config zlib1g-dev \
        m4 bison flex bc libelf-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# github.com is unreachable from the CI runner (dial tcp: i/o timeout).
# Use the Gitee mirror first; coreboot's own Gerrit as fallback. SeaBIOS is
# fetched by coreboot's payload Makefile from review.coreboot.org (reachable).
RUN git clone --depth 1 --branch 4.7 https://gitee.com/mirrors/coreboot.git coreboot || \
    git clone --depth 1 --branch 4.7 https://review.coreboot.org/coreboot.git coreboot
# Drop git metadata: coreboot's build would otherwise run `git submodule update`
# for vboot/chromeec/cbootimage, and the mirror does not carry those submodules.
RUN rm -rf coreboot/.git

COPY build-rom.sh /usr/local/bin/build-rom.sh
RUN chmod +x /usr/local/bin/build-rom.sh && /usr/local/bin/build-rom.sh
