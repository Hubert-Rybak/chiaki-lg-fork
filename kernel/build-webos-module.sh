#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${1:?usage: build-webos-module.sh WORK_DIR OUTPUT_DIR}"
OUTPUT_DIR="${2:?usage: build-webos-module.sh WORK_DIR OUTPUT_DIR}"
KERNEL_VERSION=4.4.84
LG_RELEASE_API="https://opensource.lge.com/download/releaseFileDownloadUrl"
LG_RELEASE_ARCHIVE="webOS-5.0-JO-2.0-2.tar.gz"
LG_RELEASE_SHA256="e01f2fadcc5a65b8e6f62380def123895902da5f2604dabbdad16430ed934493"
LG_BSP_PATH="JO-r3357_SoC/SoC_BSP_webOS_5.0.tar"
LG_BSP_SHA256="809879b6fb866196f6cb8fd571dc33c395f4baa487bf3ea0491a82c01b9a122e"
LG_KDRIVER_PATH="webOS5.0_SIC_Opensource/GPL_2.0/kdriver.tar.gz"
LG_KDRIVER_SHA256="6dec0514633415c1bb266de5fc8dca0d7c89973546a91c4a3e8e8b248e94722e"
LG_KERNEL_PATH="kdriver/kernel/linux-4.4-lg115x.tgz"
LG_KERNEL_SHA256="8ae082e9c24b1f3320c18e2ac390499fac7855c452707901c8e49ce034901626"
LG_RELEASE_FILE="$WORK_DIR/$LG_RELEASE_ARCHIVE"
LG_BSP_FILE="$WORK_DIR/SoC_BSP_webOS_5.0.tar"
LG_KDRIVER_FILE="$WORK_DIR/kdriver.tar.gz"
LG_KERNEL_FILE="$WORK_DIR/linux-4.4-lg115x.tgz"
KERNEL_DIR="$WORK_DIR/linux-4.4-lg115x"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"

if [[ ! -d "$KERNEL_DIR" ]]; then
    if [[ ! -f "$LG_RELEASE_FILE" ]]; then
        release_response="$(curl --fail --silent --show-error \
            --request POST \
            --data-urlencode 'osSeq=511222' \
            --data-urlencode 'modelName=OLED55GXRLA' \
            --data-urlencode 'fileType=Op' \
            --data-urlencode 'fileIdx=2' \
            "$LG_RELEASE_API")"
        release_url="$(python3 -c '
import json
import sys

response = json.load(sys.stdin)
if response.get("success") is not True or not response.get("data"):
    raise SystemExit(f"LG source URL request failed: {response!r}")
print(response["data"])
' <<<"$release_response")"
        curl --fail --location --retry 5 \
            --output "$LG_RELEASE_FILE.new" "$release_url"
        echo "$LG_RELEASE_SHA256  $LG_RELEASE_FILE.new" \
            | sha256sum --check --strict
        mv "$LG_RELEASE_FILE.new" "$LG_RELEASE_FILE"
    fi
    echo "$LG_RELEASE_SHA256  $LG_RELEASE_FILE" | sha256sum --check --strict

    tar xOzf "$LG_RELEASE_FILE" "$LG_BSP_PATH" > "$LG_BSP_FILE"
    echo "$LG_BSP_SHA256  $LG_BSP_FILE" | sha256sum --check --strict
    tar xOf "$LG_BSP_FILE" "$LG_KDRIVER_PATH" > "$LG_KDRIVER_FILE"
    echo "$LG_KDRIVER_SHA256  $LG_KDRIVER_FILE" | sha256sum --check --strict
    tar xOzf "$LG_KDRIVER_FILE" "$LG_KERNEL_PATH" > "$LG_KERNEL_FILE"
    echo "$LG_KERNEL_SHA256  $LG_KERNEL_FILE" | sha256sum --check --strict
    tar xzf "$LG_KERNEL_FILE" -C "$WORK_DIR"
fi

make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    lg1k_defconfig
"$KERNEL_DIR/scripts/config" --file "$KERNEL_DIR/.config" \
    --enable SMP \
    --enable PREEMPT \
    --enable MODULES \
    --enable MODULE_UNLOAD \
    --enable LG_BUILTIN_KDRIVER \
    --disable SONY_DUALSHOCK_4_USER_DRIVER \
    --disable MODVERSIONS \
    --disable MODULE_SIG
make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    olddefconfig
make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    modules_prepare
make -C "$KERNEL_DIR" \
    HOSTCFLAGS=-fcommon \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    KBUILD_MODPOST_WARN=1 \
    M="$SCRIPT_DIR/hid-playstation-compat" \
    clean modules

module="$SCRIPT_DIR/hid-playstation-compat/hid-playstation.ko"
test -s "$module"
cp "$module" "$OUTPUT_DIR/hid-playstation.ko"

aarch64-linux-gnu-readelf -h "$OUTPUT_DIR/hid-playstation.ko" \
    | grep -F 'Machine:' \
    | grep -F 'AArch64'
modinfo "$OUTPUT_DIR/hid-playstation.ko" | grep -F 'vermagic:       4.4.84 '
