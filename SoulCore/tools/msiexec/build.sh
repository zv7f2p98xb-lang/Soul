#!/usr/bin/env bash
# Builds msiexec.exe (32-bit GUI program) and regenerates src/msiexec_stub.h from it.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -Os -s -mwindows -ffunction-sections -fdata-sections -Wl,--gc-sections -o msiexec.exe msiexec.c -lmsi -lcomctl32
python3 ../gen_stub.py msiexec.exe ../../src/msiexec_stub.h
ls -l msiexec.exe
