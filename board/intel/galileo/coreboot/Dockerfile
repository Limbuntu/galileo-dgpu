# Builds an 8 MB coreboot ROM for the Intel Galileo Gen 2 with a SeaBIOS payload.
#
# Why: the board ships UEFI-only firmware (no CSM), and DOS needs real-mode BIOS
# services, so the only way to run FreeDOS/MS-DOS natively is a BIOS implementation
# on the SPI flash. coreboot 4.4-4.7 are the releases that still carry
# src/soc/intel/quark + src/mainboard/intel/galileo.
FROM debian:11

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        git build-essential gcc-multilib libc6-dev-i386 \
        python3 nasm iasl libssl-dev pkg-config zlib1g-dev \
        m4 bison flex bc libelf-dev curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch 4.7 https://github.com/coreboot/coreboot.git

COPY build-rom.sh /usr/local/bin/build-rom.sh
RUN chmod +x /usr/local/bin/build-rom.sh && /usr/local/bin/build-rom.sh
