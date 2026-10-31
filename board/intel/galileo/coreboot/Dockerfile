# Toolchain image for building the Galileo Gen 2 coreboot ROM.
#
# Why coreboot at all: the board ships UEFI-only firmware with no CSM, and DOS
# needs real-mode BIOS services, so running FreeDOS/MS-DOS natively means
# replacing the firmware. coreboot 4.4-4.7 are the releases that still carry
# src/soc/intel/quark + src/mainboard/intel/galileo.
#
# This image only provides the toolchain; build-rom.sh extracts the coreboot
# 4.7 release tarball (provisioned on the CI host at /opt/git-assets, or fetched
# from coreboot.org as fallback) and produces /out/coreboot.rom.
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

# coreboot 4.7 is incompatible with GNU make >= 4.3: its Makefile.inc defines a
# variable whose name is a single space, which make 4.3 turns into a segfault.
# Build make 4.2.1 instead; its bundled gnulib glob still calls __alloca/__stat,
# removed from modern glibc, so rewrite those two symbols.
RUN curl -sSL https://mirrors.tuna.tsinghua.edu.cn/gnu/make/make-4.2.1.tar.gz -o /tmp/m.tgz \
    && tar xf /tmp/m.tgz -C /tmp \
    && cd /tmp/make-4.2.1 \
    && sed -i 's/__alloca/alloca/g; s/__stat/stat/g' glob/glob.c \
    && ./configure --prefix=/opt/make42 >/dev/null \
    && make -j"$(nproc)" >/dev/null \
    && make install >/dev/null \
    && /opt/make42/bin/make --version | head -1
ENV PATH=/opt/make42/bin:$PATH

COPY build-rom.sh /usr/local/bin/build-rom.sh
RUN chmod +x /usr/local/bin/build-rom.sh

CMD ["/usr/local/bin/build-rom.sh"]
