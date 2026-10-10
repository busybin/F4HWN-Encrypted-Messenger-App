#!/usr/bin/env bash
# Build "SECMSG V24" — an encrypted messenger overlay App for the
# F4HWN UV-K1/K5v3 firmware with the 8 KiB overlay window.
#
# Requirements: arm-none-eabi-gcc 13.x (tested with 13.3.rel1), python3.
#   TC=/path/to/toolchain-bin ./build.sh      # or just have it on PATH
# Output: SECMSGV24.app (FAP1 image, ready for UVStudio install).
set -euo pipefail

APP="secmsg"
SRC="secmsg_v24.c"
APP_NAME="SECMSG"
APP_VER="25"
APP_API_MIN=2                            # needs API level 2 (uid_get) from the 8K firmware
APP_MAX=8192                             # 8 KiB overlay window (code+bss)
APP_VMA="${APP_VMA:-0x20000280}"         # firmware __mb_workspace_start
OUT="${APP_NAME// /}"

# --- locate the toolchain -------------------------------------------------
if [ -n "${TC:-}" ]; then
  :
elif command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  TC="$(dirname "$(command -v arm-none-eabi-gcc)")"
else
  TC="/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin"
fi
CC="$TC/arm-none-eabi-gcc"
OBJCOPY="$TC/arm-none-eabi-objcopy"
[ -x "$CC" ] || { printf 'ARM GCC not found: %s (set TC=...)\n' "$CC" >&2; exit 1; }

CFLAGS="-mcpu=cortex-m0plus -mthumb -Oz -std=gnu11 -ffreestanding -fno-builtin -fno-common \
  -fomit-frame-pointer -ffunction-sections -fdata-sections -fno-tree-tail-merge \
  -fno-merge-constants -fno-ipa-reference -fno-ipa-pure-const -Wall -Wextra"
LDFLAGS="-nostdlib -nostartfiles -T app.ld -Wl,--defsym,APP_VMA=${APP_VMA} \
  -Wl,--gc-sections -Wl,--sort-section=alignment -Wl,-Map=${APP}.map \
  -Wl,--build-id=none -Wl,--no-warn-rwx-segments"

rm -f "${APP}.elf" "${APP}.bin" "${APP}.map" "${OUT}.app"

"$CC" $CFLAGS $LDFLAGS "$SRC" -lgcc -o "${APP}.elf"
"$OBJCOPY" -O binary "${APP}.elf" "${APP}.bin"
BYTES=$(wc -c < "${APP}.bin")
if [ "$BYTES" -gt "$APP_MAX" ]; then
  printf 'OVERFLOW: code image %d B > %d B\n' "$BYTES" "$APP_MAX" >&2; exit 1
fi
python3 pack_app.py "${APP}.bin" "${OUT}.app" \
    --name "$APP_NAME" --ver "$APP_VER" --api-min "$APP_API_MIN" --vma "${APP_VMA}"

printf 'OK  %-10s code %5d B (%d%% of 8 KiB code budget)  ->  %s\n' \
       "$APP_NAME" "$BYTES" "$(( BYTES * 100 / APP_MAX ))" "${OUT}.app"
