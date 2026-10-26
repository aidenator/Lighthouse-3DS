# Nintendo 3DS build environment: devkitARM plus the libraries Lighthouse needs that devkitPro
# does not package for the 3DS (SDL2, libzip, spdlog, nlohmann_json), installed into the 3DS
# portlibs prefix.
#
#   docker build -t lighthouse-3ds -f tools/docker/3ds.Dockerfile tools/docker
FROM devkitpro/devkitarm:latest

ARG SDL2_VERSION=2.30.11
ARG LIBZIP_VERSION=1.11.2
ARG SPDLOG_VERSION=1.14.1
ARG JSON_VERSION=3.11.3
ARG TINYXML2_VERSION=10.0.0

ENV DEVKITPRO=/opt/devkitpro
ENV DEVKITARM=/opt/devkitpro/devkitARM
ENV PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH

# newlib on ARM makes int32_t a long; the decomp's s32 is an int. Make the 32-bit stdint types plain
# int/unsigned (same size and ABI) so the two agree. Lighthouse's CMake adds the same flags for itself;
# CMake picks these up when configuring each dependency below.
#
# Bare-metal ARM also defaults to variable-size ("short") enums and unsigned char, unlike the PC
# builds Lighthouse is written against: an enum of small values is one unsigned byte, so -1
# sentinels become 255 and struct layouts change. Everything is built with int-sized enums and
# signed char. newlib and libstdc++ keep their stock build; their enums are written to stay int
# sized either way.
ENV CFLAGS="-fno-short-enums -fsigned-char -U__INT32_TYPE__ -D__INT32_TYPE__=int -U__UINT32_TYPE__ -D__UINT32_TYPE__=unsigned -U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int -U__UINT_LEAST32_TYPE__ -D__UINT_LEAST32_TYPE__=unsigned"
ENV CXXFLAGS="$CFLAGS"

ARG LIBCTRU_VERSION=2.7.0
ARG CITRO3D_VERSION=1.7.1

# libctru and citro3d ship built with short enums, and their structs (C3D_RenderTarget, DVLE_s, ...)
# change layout with enum size, so rebuild them with int enums over the packaged copies. GCC 16
# warns where these releases' -Werror did not expect it, so that is dropped.
WORKDIR /deps
RUN curl -fsSL https://github.com/devkitPro/libctru/archive/refs/tags/v${LIBCTRU_VERSION}.tar.gz | tar xz \
    && sed -i -e 's/-Werror//' -e 's/^ARCH\s*:=.*/& -fno-short-enums/' libctru-${LIBCTRU_VERSION}/libctru/Makefile \
    && make -C libctru-${LIBCTRU_VERSION}/libctru -j$(nproc) install \
    && rm -rf libctru-${LIBCTRU_VERSION}
RUN curl -fsSL https://github.com/devkitPro/citro3d/archive/refs/tags/v${CITRO3D_VERSION}.tar.gz | tar xz \
    && sed -i -e 's/-Werror//' -e 's/^ARCH\s*:=.*/& -fno-short-enums/' citro3d-${CITRO3D_VERSION}/Makefile \
    && make -C citro3d-${CITRO3D_VERSION} -j$(nproc) install \
    && rm -rf citro3d-${CITRO3D_VERSION}

# Configure a dependency against the 3DS toolchain and install it into portlibs
RUN printf '#!/bin/sh\nset -e\nsrc="$1"; shift\ncmake -S "$src" -B "$src/build" -G Ninja \\\n    -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release \\\n    -DCMAKE_INSTALL_PREFIX=$DEVKITPRO/portlibs/3ds -DBUILD_SHARED_LIBS=OFF "$@"\ncmake --build "$src/build"\ncmake --install "$src/build"\n' > /usr/local/bin/dkp-3ds-install \
    && chmod +x /usr/local/bin/dkp-3ds-install

WORKDIR /deps

RUN curl -fsSL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz | tar xz \
    && dkp-3ds-install SDL2-${SDL2_VERSION} -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
    && rm -rf SDL2-${SDL2_VERSION}

RUN curl -fsSL https://github.com/nih-at/libzip/releases/download/v${LIBZIP_VERSION}/libzip-${LIBZIP_VERSION}.tar.gz | tar xz \
    && dkp-3ds-install libzip-${LIBZIP_VERSION} -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF \
        -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF \
        -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
    && rm -rf libzip-${LIBZIP_VERSION}

# spdlog builds with strict -std=c++11, where newlib hides POSIX calls such as fileno()
RUN curl -fsSL https://github.com/gabime/spdlog/archive/refs/tags/v${SPDLOG_VERSION}.tar.gz | tar xz \
    && CXXFLAGS="$CXXFLAGS -D_GNU_SOURCE" dkp-3ds-install spdlog-${SPDLOG_VERSION} -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF \
    && rm -rf spdlog-${SPDLOG_VERSION}

RUN curl -fsSL https://github.com/nlohmann/json/archive/refs/tags/v${JSON_VERSION}.tar.gz | tar xz \
    && dkp-3ds-install json-${JSON_VERSION} -DJSON_BuildTests=OFF \
    && rm -rf json-${JSON_VERSION}

# devkitPro's tinyxml2 is 6.0 and exports only tinyxml2_static; replace it with a current release
RUN curl -fsSL https://github.com/leethomason/tinyxml2/archive/refs/tags/${TINYXML2_VERSION}.tar.gz | tar xz \
    && rm -rf $DEVKITPRO/portlibs/3ds/lib/cmake/tinyxml2 \
    && dkp-3ds-install tinyxml2-${TINYXML2_VERSION} -Dtinyxml2_BUILD_TESTING=OFF \
    && rm -rf tinyxml2-${TINYXML2_VERSION}

# Tools to package a .cia: makerom builds it, bannertool makes the HOME Menu banner. Built from
# source, since their released Linux binaries need a newer glibc than this image has. Host tools,
# so built without the 3DS flags above.
ARG MAKEROM_VERSION=0.19.0
ARG BANNERTOOL_VERSION=1.2.3
RUN curl -fsSL https://github.com/3DSGuy/Project_CTR/archive/refs/tags/makerom-v${MAKEROM_VERSION}.tar.gz | tar xz \
    && cd Project_CTR-makerom-v${MAKEROM_VERSION}/makerom \
    && CFLAGS= CXXFLAGS= make deps && CFLAGS= CXXFLAGS= make -j$(nproc) \
    && install -m 755 bin/makerom /usr/local/bin/makerom \
    && cd /deps && rm -rf Project_CTR-makerom-v${MAKEROM_VERSION}
RUN curl -fsSL https://github.com/carstene1ns/3ds-bannertool/releases/download/${BANNERTOOL_VERSION}/bannertool-${BANNERTOOL_VERSION}.tar.gz | tar xz \
    && CFLAGS= CXXFLAGS= cmake -S bannertool-${BANNERTOOL_VERSION} -B bannertool-build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build bannertool-build -j$(nproc) \
    && install -m 755 bannertool-build/bannertool /usr/local/bin/bannertool \
    && rm -rf bannertool-${BANNERTOOL_VERSION} bannertool-build

WORKDIR /src
