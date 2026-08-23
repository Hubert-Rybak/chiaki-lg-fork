#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${1:?usage: build-webos-armv7-module.sh WORK_DIR OUTPUT_DIR}"
OUTPUT_DIR="${2:?usage: build-webos-armv7-module.sh WORK_DIR OUTPUT_DIR}"

LG_RELEASE_API="https://opensource.lge.com/download/releaseFileDownloadUrl"
LG_RELEASE_OS_SEQ="510437"
LG_RELEASE_MODEL="43UR640S9ZD"
LG_RELEASE_FILE_INDEX="1"
LG_BSP_MEMBER="GPL-2.0/Linux Kernel/bsp_open3.tar.gz"
LG_BSP_SIZE="730073336"
LG_BSP_CRC32="69717dd7"
LG_BSP_SHA256="4029e666c3c31bb4ff8f4fe351dcf546ac8321a0e42fb76016879f150df74fde"
LG_BSP_FILE="$WORK_DIR/bsp_open3.tar.gz"
LG_BSP_DIR="$WORK_DIR/bsp_open"
KERNEL_DIR="$LG_BSP_DIR/kernel"
TOOLCHAIN_INSTALLER_NAME="starfish-sdk-x86_64-ca9v1-toolchain-5.0.0-20190307.sh"
TOOLCHAIN_INSTALLER="$LG_BSP_DIR/$TOOLCHAIN_INSTALLER_NAME"
TOOLCHAIN_DIR="$WORK_DIR/starfish-sdk-5.0.0-20190307"
TOOLCHAIN_ENV="$TOOLCHAIN_DIR/environment-setup-ca9v1-starfishmllib32-linux-gnueabi"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"

python3 "$SCRIPT_DIR/download-lg-zip-member.py" \
    --api "$LG_RELEASE_API" \
    --os-seq "$LG_RELEASE_OS_SEQ" \
    --model "$LG_RELEASE_MODEL" \
    --file-index "$LG_RELEASE_FILE_INDEX" \
    --member "$LG_BSP_MEMBER" \
    --size "$LG_BSP_SIZE" \
    --crc32 "$LG_BSP_CRC32" \
    --sha256 "$LG_BSP_SHA256" \
    "$LG_BSP_FILE"

if [[ ! -f "$KERNEL_DIR/Makefile" || ! -f "$TOOLCHAIN_INSTALLER" ]]; then
    tar xzf "$LG_BSP_FILE" -C "$WORK_DIR" \
        bsp_open/kernel \
        "bsp_open/$TOOLCHAIN_INSTALLER_NAME"
fi

if [[ ! -f "$TOOLCHAIN_ENV" ]]; then
    chmod 0755 "$TOOLCHAIN_INSTALLER"
    "$TOOLCHAIN_INSTALLER" -y -d "$TOOLCHAIN_DIR"
fi

# The official SDK environment supplies the relocated native tools, sysroot,
# and exact GCC 8.2.0 cross compiler used by LG's published LM21U modules.
set +u
source "$TOOLCHAIN_ENV"
set -u

test "$(arm-starfish-linux-gnueabi-gcc -dumpfullversion)" = "8.2.0"

make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm \
    CROSS_COMPILE=arm-starfish-linux-gnueabi- \
    lm21u_dtb_mma_seeTV_defconfig

grep -qx 'CONFIG_MSTAR_MT5889=y' "$KERNEL_DIR/.config"
grep -qx 'CONFIG_MODULES=y' "$KERNEL_DIR/.config"
grep -qx 'CONFIG_MODULE_UNLOAD=y' "$KERNEL_DIR/.config"
grep -qx 'CONFIG_SMP=y' "$KERNEL_DIR/.config"
grep -qx 'CONFIG_PREEMPT=y' "$KERNEL_DIR/.config"
grep -qx 'CONFIG_AEABI=y' "$KERNEL_DIR/.config"
if grep -qx 'CONFIG_MODVERSIONS=y' "$KERNEL_DIR/.config"; then
    echo "LM21U source unexpectedly enables CONFIG_MODVERSIONS" >&2
    exit 1
fi

make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm \
    CROSS_COMPILE=arm-starfish-linux-gnueabi- \
    modules_prepare
make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm \
    CROSS_COMPILE=arm-starfish-linux-gnueabi- \
    KBUILD_MODPOST_WARN=1 \
    M="$SCRIPT_DIR/hid-playstation-compat" \
    clean modules

module="$SCRIPT_DIR/hid-playstation-compat/hid-playstation.ko"
test -s "$module"
cp "$module" "$OUTPUT_DIR/hid-playstation.ko"

arm-starfish-linux-gnueabi-readelf -h "$OUTPUT_DIR/hid-playstation.ko" \
    | grep -F 'Class:' \
    | grep -F 'ELF32'
arm-starfish-linux-gnueabi-readelf -h "$OUTPUT_DIR/hid-playstation.ko" \
    | grep -F 'Machine:' \
    | grep -F 'ARM'
test "$(modinfo -F vermagic "$OUTPUT_DIR/hid-playstation.ko")" = \
    '4.4.84 SMP preempt mod_unload ARMv7 '
