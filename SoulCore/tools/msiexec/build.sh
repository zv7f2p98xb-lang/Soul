#!/usr/bin/env bash
# Builds msiexec.exe (32-bit GUI program) into build/msiexec/msiexec.exe.
# With --update-stub it also regenerates src/msiexec_stub.h from it (commit that header when msiexec.c changed).
set -e
cd "$(dirname "$0")"
OUT=../../build/msiexec
mkdir -p "$OUT"
i686-w64-mingw32-gcc -Os -s -mwindows -ffunction-sections -fdata-sections -Wl,--gc-sections -o "$OUT/msiexec.exe" msiexec.c -lmsi -lcomctl32
if [ "${1:-}" == "--update-stub" ]; then python3 ../gen_stub.py "$OUT/msiexec.exe" ../../src/msiexec_stub.h; fi
ls -l "$OUT/msiexec.exe"
