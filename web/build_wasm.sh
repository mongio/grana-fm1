#!/bin/sh
# Build GRANA for the browser: web/grana.wasm (the same grana.c as the FM-1, on web/web_host.c)
set -e
cd "$(dirname "$0")/.."
clang --target=wasm32 -O2 -ffreestanding -nostdlib -fno-builtin -Wall -Wno-unused-function \
    -DGRANA_WEB -DAPP_VER="\"${APP_VER:-0.1}\"" -Ifirmware/src -Ifirmware/gen \
    -Wl,--no-entry -Wl,--export-memory -Wl,-z,stack-size=262144 -Wl,--initial-memory=8388608 \
    -o web/grana.wasm firmware/src/grana.c
ls -l web/grana.wasm
