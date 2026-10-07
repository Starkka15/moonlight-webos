#!/bin/bash
# Fetches and cross-builds everything the Makefile expects:
#   deps/moonlight-embedded   recursive checkout
#   sysroot/                  static OpenSSL, expat, opus, curl and FFmpeg
#
# Usage: ./build-deps.sh        (PDK=/opt/PalmPDK by default)
#
# Run it from a Linux filesystem: the PDK's 2011 toolchain is a 32-bit build
# without large-file support and fails on network or Windows-mounted drives.
set -e

PDK=${PDK:-/opt/PalmPDK}
HOST=arm-none-linux-gnueabi
CROSS=$PDK/arm-toolchain/bin/$HOST-
ROOT=$(cd "$(dirname "$0")" && pwd)
DEPS=$ROOT/deps
SYSROOT=$ROOT/sysroot
JOBS=${JOBS:-$(nproc)}

# moonlight-embedded as of the 0.1.0 release (override to try a newer one)
MOONLIGHT_DATE=${MOONLIGHT_DATE:-2026-01-26}

OPENSSL=openssl-1.1.1w
EXPAT=expat-2.5.0
OPUS=opus-1.4
CURL=curl-7.88.1
FFMPEG=ffmpeg-4.4.4

export CC=${CROSS}gcc AR=${CROSS}ar RANLIB=${CROSS}ranlib STRIP=${CROSS}strip
export CFLAGS="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=softfp"

test -x "$CC" || { echo "Cross-compiler not found at $CC; set PDK=..."; exit 1; }
mkdir -p "$DEPS" "$SYSROOT"
cd "$DEPS"

fetch() {  # fetch <url> <archive>
    [ -f "$2" ] || curl -fL --retry 3 -o "$2" "$1"
}

if [ ! -d moonlight-embedded ]; then
    git clone https://github.com/moonlight-stream/moonlight-embedded
    cd moonlight-embedded
    git checkout "$(git rev-list -1 --before="$MOONLIGHT_DATE 23:59" HEAD)"
    git submodule update --init --recursive
    cd ..
fi

if [ ! -f "$SYSROOT/lib/libssl.a" ]; then
    fetch https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/$OPENSSL.tar.gz $OPENSSL.tar.gz
    rm -rf $OPENSSL && tar xf $OPENSSL.tar.gz && cd $OPENSSL
    # Configure adds the cross prefix itself
    CC=gcc AR=ar RANLIB=ranlib ./Configure linux-armv4 no-shared no-tests no-async no-engine \
        --prefix="$SYSROOT" --openssldir="$SYSROOT/ssl" --cross-compile-prefix="$CROSS" $CFLAGS
    make -j"$JOBS" build_libs
    make install_dev
    cd ..
fi

if [ ! -f "$SYSROOT/lib/libexpat.a" ]; then
    fetch https://github.com/libexpat/libexpat/releases/download/R_2_5_0/$EXPAT.tar.gz $EXPAT.tar.gz
    rm -rf $EXPAT && tar xf $EXPAT.tar.gz && cd $EXPAT
    # GCC 4.5.2 hits an internal compiler error in xmltok_impl.c at -O2
    CFLAGS="${CFLAGS/-O2/-O1}" ./configure --host=$HOST --prefix="$SYSROOT" --disable-shared \
        --without-docbook --without-examples --without-tests --without-xmlwf
    make -j"$JOBS"
    make install
    cd ..
fi

if [ ! -f "$SYSROOT/lib/libopus.a" ]; then
    fetch https://downloads.xiph.org/releases/opus/$OPUS.tar.gz $OPUS.tar.gz
    rm -rf $OPUS && tar xf $OPUS.tar.gz && cd $OPUS
    ./configure --host=$HOST --prefix="$SYSROOT" --disable-shared \
        --disable-doc --disable-extra-programs
    make -j"$JOBS"
    make install
    cd ..
fi

if [ ! -f "$SYSROOT/lib/libcurl.a" ]; then
    fetch https://curl.se/download/$CURL.tar.gz $CURL.tar.gz
    rm -rf $CURL && tar xf $CURL.tar.gz && cd $CURL
    ./configure --host=$HOST --prefix="$SYSROOT" --disable-shared \
        --with-openssl="$SYSROOT" --without-zlib --without-brotli --without-zstd \
        --without-libpsl --without-libidn2 --without-nghttp2 --without-librtmp \
        --disable-ldap --disable-ldaps --disable-manual --disable-docs \
        --disable-ftp --disable-file --disable-dict --disable-telnet --disable-tftp \
        --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher \
        --disable-mqtt --disable-rtsp
    make -j"$JOBS"
    make install
    cd ..
fi

if [ ! -f "$SYSROOT/lib/libavcodec.a" ]; then
    fetch https://ffmpeg.org/releases/$FFMPEG.tar.xz $FFMPEG.tar.xz
    rm -rf $FFMPEG && tar xf $FFMPEG.tar.xz && cd $FFMPEG
    # Only the H.264 software decoder is used
    ./configure --prefix="$SYSROOT" --enable-cross-compile --cross-prefix="$CROSS" \
        --arch=arm --cpu=cortex-a8 --target-os=linux --extra-cflags="$CFLAGS" \
        --enable-static --disable-shared --disable-programs --disable-doc \
        --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
        --disable-postproc --disable-network --disable-everything \
        --enable-decoder=h264 --enable-parser=h264
    make -j"$JOBS"
    make install
    cd ..
fi

echo "Done: $SYSROOT and $DEPS/moonlight-embedded are ready; run make."
