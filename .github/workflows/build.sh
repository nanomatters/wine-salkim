#!/usr/bin/env bash

set -euo pipefail

Info() {
    echo -e '\033[1;34m'"WineBuild:\033[0m $*"
}

Error() {
    echo -e '\033[1;31m'"WineBuild:\033[0m $*"
    exit 1
}

Info "Starting Wine build process..."

WINE_VERSION="$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^cachyos-//; s/-[^-]*$//' || echo unknown)"
Info "Building Wine version: ${WINE_VERSION}"
BUILD_DIR="/tmp/wine-build"
mkdir -p "${BUILD_DIR}"/{build64,build32,wine-install}

# initialize git for build tools
git config --global --add safe.directory "${PWD}"
git config commit.gpgsign false
git config user.email "wine@build.dev"
git config user.name "winebuild"

Info "Generating build files..."
[ -e dlls/winevulkan/make_vulkan ] && {
    chmod +x dlls/winevulkan/make_vulkan
    dlls/winevulkan/make_vulkan
}

chmod +x tools/make_requests
tools/make_requests

[ -e tools/make_specfiles ] && {
    chmod +x tools/make_specfiles
    tools/make_specfiles
}

autoreconf -fiv

# setup reproducible build
export SOURCE_DATE_EPOCH=0
export PKG_CONFIG="pkg-config"

# gcc-mingw setup
export GCC_MINGW_PATH="/usr/local/gcc-mingw"
export PATH="${GCC_MINGW_PATH}/bin:${PATH}"
export LIBRARY_PATH="/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14:/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14/32:/usr/lib/gcc-14/lib:/usr/lib/gcc-14/lib32:/usr/lib:/usr/lib/x86_64-linux-gnu:/usr/local/lib:/usr/local/lib/x86_64-linux-gnu:/usr/local/i386/lib/i386-linux-gnu:/usr/local/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu"
export LD_LIBRARY_PATH="/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14:/usr/lib/gcc-14/lib/gcc/x86_64-linux-gnu/14/32:/usr/lib/gcc-14/lib:/usr/lib/gcc-14/lib32:/usr/lib:/usr/lib/x86_64-linux-gnu:/usr/local/lib:/usr/local/lib/x86_64-linux-gnu:/usr/local/i386/lib/i386-linux-gnu:/usr/local/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu"

# compiler settings
export CC="ccache gcc"
export CXX="ccache g++"
export CROSSCC_X32="ccache i686-w64-mingw32-gcc"
export CROSSCXX_X32="ccache i686-w64-mingw32-g++"
export CROSSCC_X64="ccache x86_64-w64-mingw32-gcc"
export CROSSCXX_X64="ccache x86_64-w64-mingw32-g++"
export x86_64_CC="${CROSSCC_X64}"
export i386_CC="${CROSSCC_X32}"

# compiler flags
_GCC_FLAGS="-march=x86-64 -msse3 -mfpmath=sse -O2 -ftree-vectorize -static-libgcc -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion"
_LD_FLAGS="-Wl,-O1,--sort-common,--as-needed"

export CFLAGS="${_GCC_FLAGS}"
export CXXFLAGS="${_GCC_FLAGS}"
export LDFLAGS="${_LD_FLAGS}"
export CROSSCFLAGS="${_GCC_FLAGS}"
export CROSSCXXFLAGS="${_GCC_FLAGS}"
export CROSSLDFLAGS="${_LD_FLAGS}"
export i386_CFLAGS="${CROSSCFLAGS}"
export x86_64_CFLAGS="${CROSSCFLAGS}"

# configure options
WINE_CONFIGURE_OPTS=(
    --prefix="${BUILD_DIR}/wine-install"
    --disable-tests
    --disable-winemenubuilder
    --disable-win16
    --with-x
    --with-gstreamer
    --with-ffmpeg
    --with-wayland
    --without-oss
    --without-coreaudio
    --without-cups
    --without-sane
    --without-gphoto
    --without-pcsclite
    --without-pcap
    --without-capi
    --without-v4l2
    --without-netapi
    --disable-msv1_0
)

## 64-bit
Info "Configuring 64-bit build..."
cd "${BUILD_DIR}/build64"

export PKG_CONFIG_LIBDIR="/usr/local/x86_64/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_PATH="${PKG_CONFIG_LIBDIR}"
export CROSSCC="${CROSSCC_X64}"

# libunwind support
if [ -f "/usr/local/lib/libunwind.a" ] && [ -f "/usr/local/lib/liblzma.a" ]; then
    export UNWIND_CFLAGS=""
    export UNWIND_LIBS="-L/usr/local/lib/ -static-libgcc -l:libunwind.a -l:liblzma.a"
fi

"${GITHUB_WORKSPACE}/configure" \
    "${WINE_CONFIGURE_OPTS[@]}" \
    --libdir="${BUILD_DIR}/wine-install/lib" \
    --enable-win64 \
    --with-mingw="${x86_64_CC}"

Info "Building 64-bit..."
make -j$(($(nproc) + 1))

unset UNWIND_CFLAGS UNWIND_LIBS

## build 32-bit
Info "Configuring 32-bit build..."
cd "${BUILD_DIR}/build32"

export PKG_CONFIG_LIBDIR="/usr/local/i386/lib/i386-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_PATH="${PKG_CONFIG_LIBDIR}"
export CROSSCC="${CROSSCC_X32}"

"${GITHUB_WORKSPACE}/configure" \
    "${WINE_CONFIGURE_OPTS[@]}" \
    --libdir="${BUILD_DIR}/wine-install/lib" \
    --with-wine64="${BUILD_DIR}/build64" \
    --with-mingw="${i386_CC}"

Info "Building 32-bit..."
make -j$(($(nproc) + 1))

Info "Installing Wine..."

cd "${BUILD_DIR}/build32"
make -j$(($(nproc) + 1)) \
    prefix="${BUILD_DIR}/wine-install" \
    libdir="${BUILD_DIR}/wine-install/lib" \
    dlldir="${BUILD_DIR}/wine-install/lib/wine" \
    install-lib

cd "${BUILD_DIR}/build64"
make -j$(($(nproc) + 1)) \
    prefix="${BUILD_DIR}/wine-install" \
    libdir="${BUILD_DIR}/wine-install/lib" \
    dlldir="${BUILD_DIR}/wine-install/lib/wine" \
    install-lib

# symlinks
cd "${BUILD_DIR}/wine-install"
ln -srf lib lib64
ln -srf lib lib32

[ ! -f bin/wine64 ] && [ -f bin/wine ] && ln -srf bin/wine{,64}

# strip binaries
Info "Stripping debug symbols..."
find "${BUILD_DIR}/wine-install/lib/" \
    -type f '(' -iname '*.a' -o -iname '*.dll' -o -iname '*.so' -o -iname '*.sys' -o -iname '*.drv' -o -iname '*.exe' ')' \
    -print0 | xargs -0 strip -s 2>/dev/null || true

# cleanup
rm -rf "${BUILD_DIR}/wine-install"/{include,share/{applications,man}}

Info "Creating archive..."
cd "${BUILD_DIR}"

BUILD_NAME="wine-cachy-${WINE_VERSION}"
mv wine-install "${BUILD_NAME}"

ARCHIVE_NAME="${BUILD_NAME}-1-x86_64.tar.xz"
tar -cJf \
    "${ARCHIVE_NAME}" \
    --xattrs --numeric-owner --owner=0 --group=0 "${BUILD_NAME}"

# move to workspace
mv "${ARCHIVE_NAME}" "${GITHUB_WORKSPACE}/"
Info "Build completed successfully! Archive: ${ARCHIVE_NAME}"
