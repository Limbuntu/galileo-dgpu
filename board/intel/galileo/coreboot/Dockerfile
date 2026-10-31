# Builds an 8 MB coreboot ROM for the Intel Galileo Gen 2 with a SeaBIOS payload.
#
# Why: the board ships UEFI-only firmware (no CSM), and DOS needs real-mode BIOS
# services, so the only way to run FreeDOS/MS-DOS natively is a BIOS implementation
# on the SPI flash. coreboot 4.4-4.7 are the releases that still carry
# src/soc/intel/quark + src/mainboard/intel/galileo.
#
# The tree is the official 4.7 release tarball, not a git checkout: coreboot's
# cbfstool needs 3rdparty/vboot headers, and github.com is unreachable from the
# CI runner (its gitee mirror does not carry the submodules).
FROM debian:12

ENV DEBIAN_FRONTEND=noninteractive

# Domestic mirror: deb.debian.org is slow/unreliable from the CI runner.
RUN sed -i 's|deb.debian.org|mirrors.tuna.tsinghua.edu.cn|g' \
           /etc/apt/sources.list.d/debian.sources \
    && apt-get -o Acquire::Retries=5 update \
    && apt-get -o Acquire::Retries=5 install -y --no-install-recommends \
        git build-essential gcc-multilib libc6-dev-i386 \
        python3 nasm iasl libssl-dev pkg-config zlib1g-dev \
        m4 bison flex bc libelf-dev curl ca-certificates \
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

WORKDIR /src
ADD coreboot-4.7.tar.xz /src/

COPY build-rom.sh /usr/local/bin/build-rom.sh
RUN chmod +x /usr/local/bin/build-rom.sh && /usr/local/bin/build-rom.sh
