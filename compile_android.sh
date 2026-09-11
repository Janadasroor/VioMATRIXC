#!/bin/bash
# Smart Android NDK Cross-Compilation Script for VioMATRIXC / ngspice
# Usage: ./compile_android.sh [x86_64|arm64|arm|x86] [api_level]

set -e

# Dynamically resolve script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ABI="${1:-x86_64}"
API="${2:-24}"

# Color output helpers
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${CYAN}=== VioMATRIXC Android NDK Cross-Compiler ===${NC}"
echo -e "Target ABI: ${GREEN}${ABI}${NC}"
echo -e "Target API Level: ${GREEN}${API}${NC}"

# Auto-detect Android NDK Location
if [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
    NDK="$ANDROID_NDK_HOME"
elif [ -n "$NDK_HOME" ] && [ -d "$NDK_HOME" ]; then
    NDK="$NDK_HOME"
elif [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk" ]; then
    NDK_LATEST=$(ls -1 "$ANDROID_HOME/ndk" 2>/dev/null | tail -n 1)
    NDK="$ANDROID_HOME/ndk/$NDK_LATEST"
else
    # Search common user & system SDK/NDK paths
    POSSIBLE_PATHS=(
        "$HOME/Android/Sdk/ndk"
        "$HOME/android-sdk/ndk"
        "$HOME/Library/Android/sdk/ndk"
        "/opt/android-sdk/ndk"
        "/usr/local/android-sdk/ndk"
    )
    for path in "${POSSIBLE_PATHS[@]}"; do
        if [ -d "$path" ]; then
            NDK_LATEST=$(ls -1 "$path" 2>/dev/null | tail -n 1)
            if [ -n "$NDK_LATEST" ]; then
                NDK="$path/$NDK_LATEST"
                break
            fi
        fi
    done
fi

if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo -e "${RED}Error: Android NDK not found! Please set ANDROID_NDK_HOME or NDK_HOME environment variable.${NC}"
    exit 1
fi

echo -e "Using NDK: ${YELLOW}${NDK}${NC}"

# Detect Host OS for Toolchain path
HOST_TAG="linux-x86_64"
if [[ "$OSTYPE" == "darwin"* ]]; then
    HOST_TAG="darwin-x86_64"
fi

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
if [ ! -d "$TOOLCHAIN" ]; then
    echo -e "${RED}Error: Toolchain directory $TOOLCHAIN does not exist.${NC}"
    exit 1
fi

# Map ABI to Target Triple
case "$ABI" in
    x86_64)
        TARGET_TRIPLE="x86_64-linux-android"
        CLANG_PREFIX="x86_64-linux-android${API}"
        ;;
    arm64|arm64-v8a|aarch64)
        ABI="arm64-v8a"
        TARGET_TRIPLE="aarch64-linux-android"
        CLANG_PREFIX="aarch64-linux-android${API}"
        ;;
    arm|armeabi-v7a)
        ABI="armeabi-v7a"
        TARGET_TRIPLE="armv7a-linux-androideabi"
        CLANG_PREFIX="armv7a-linux-androideabi${API}"
        ;;
    x86)
        TARGET_TRIPLE="i686-linux-android"
        CLANG_PREFIX="i686-linux-android${API}"
        ;;
    *)
        echo -e "${RED}Unknown ABI: $ABI. Supported: x86_64, arm64, arm, x86${NC}"
        exit 1
        ;;
esac

export AR="$TOOLCHAIN/bin/llvm-ar"
export CC="$TOOLCHAIN/bin/${CLANG_PREFIX}-clang"
export CXX="$TOOLCHAIN/bin/${CLANG_PREFIX}-clang++"
export AS="$CC"
export LD="$TOOLCHAIN/bin/ld"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

if [ ! -f "$CC" ]; then
    echo -e "${RED}Error: Compiler binary $CC not found!${NC}"
    exit 1
fi

# Ensure autotools files exist
if [ ! -f "configure" ]; then
    echo -e "${CYAN}Running autogen.sh...${NC}"
    ./autogen.sh
fi

BUILD_DIR="build_android_${ABI}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo -e "${CYAN}Configuring build for $ABI...${NC}"

../configure --host="$TARGET_TRIPLE" \
             --with-ngshared \
             --enable-cider \
             --enable-vicompat \
             --disable-debug \
             --disable-openmp \
             CFLAGS="-O2 -fPIC -DHAVE_LIBPTHREAD=1" \
             LDFLAGS="-s -Wl,-soname,libngspice.so" \
             LIBS="-lm -lc"

CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo -e "${CYAN}Building shared library with $CORES jobs...${NC}"
make -j"$CORES"

OUTPUT_SO="src/.libs/libngspice.so"
if [ -f "$OUTPUT_SO" ]; then
    echo -e "${GREEN}=== Build Successful! ===${NC}"
    echo -e "Shared Library: ${YELLOW}${SCRIPT_DIR}/${BUILD_DIR}/${OUTPUT_SO}${NC}"
else
    echo -e "${RED}Build failed: $OUTPUT_SO was not produced.${NC}"
    exit 1
fi
