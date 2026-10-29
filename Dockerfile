# Buildroot builder image for Galileo Gen 2 images
# Used by CI (.gitea/workflows/build.yml) and local builds
FROM debian:bookworm


RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make bc bison flex \
        libncurses-dev libssl-dev \
        bash patch gzip bzip2 xz-utils tar cpio unzip rsync file wget \
        python3 python-is-python3 perl libxml-parser-perl \
        git curl sudo locales gtk-doc-tools \
    && rm -rf /var/lib/apt/lists/*

RUN sed -i "s/# en_US.UTF-8/en_US.UTF-8/" /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

ENV FORCE_UNSAFE_CONFIGURE=1

WORKDIR /build
