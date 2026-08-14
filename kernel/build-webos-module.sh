#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${1:?usage: build-webos-module.sh WORK_DIR OUTPUT_DIR}"
OUTPUT_DIR="${2:?usage: build-webos-module.sh WORK_DIR OUTPUT_DIR}"
KERNEL_VERSION=4.4.84
KERNEL_ARCHIVE="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v4.x/${KERNEL_ARCHIVE}"
KERNEL_SHA256="360f89d62d4cdf1c3c978ded1666951e4d61d6b12f1b4caa5adc788d5a484904"
KERNEL_DIR="$WORK_DIR/linux-${KERNEL_VERSION}"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"

if [[ ! -f "$WORK_DIR/$KERNEL_ARCHIVE" ]]; then
    curl --fail --location --retry 5 --output "$WORK_DIR/$KERNEL_ARCHIVE" "$KERNEL_URL"
fi
echo "$KERNEL_SHA256  $WORK_DIR/$KERNEL_ARCHIVE" | sha256sum --check --strict

if [[ ! -d "$KERNEL_DIR" ]]; then
    tar xJf "$WORK_DIR/$KERNEL_ARCHIVE" -C "$WORK_DIR"
fi

make -C "$KERNEL_DIR" \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    defconfig
"$KERNEL_DIR/scripts/config" --file "$KERNEL_DIR/.config" \
    --enable SMP \
    --enable PREEMPT \
    --enable MODULES \
    --enable MODULE_UNLOAD \
    --disable MODVERSIONS \
    --disable MODULE_SIG
make -C "$KERNEL_DIR" \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    olddefconfig
make -C "$KERNEL_DIR" \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    modules_prepare
make -C "$KERNEL_DIR" \
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
