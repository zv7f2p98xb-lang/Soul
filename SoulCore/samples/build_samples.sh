#!/usr/bin/env bash
# Builds the bundled sample Windows programs with MinGW-w64 into $1 (default: build/samples).
set -euo pipefail
cd "$(dirname "$0")"
OUT="${1:-../build/samples}"
mkdir -p "$OUT"
CC=x86_64-w64-mingw32-gcc
CXX=x86_64-w64-mingw32-g++
$CC -O2 -s -o "$OUT/Merhaba.exe" merhaba.c
$CC -O2 -s -o "$OUT/HesapMakinesi.exe" hesap.c -lm
$CC -O2 -s -o "$OUT/SayiTahmin.exe" tahmin.c
$CC -O2 -s -o "$OUT/Mandelbrot.exe" mandelbrot.c
$CC -O2 -s -o "$OUT/Benchmark.exe" benchmark.c
$CC -O2 -s -o "$OUT/SistemBilgisi.exe" sistem.c
$CC -O2 -s -o "$OUT/IsParcaciklari.exe" isparcaciklari.c
$CXX -O2 -s -static -o "$OUT/CppDemo.exe" cppdemo.cpp
cp samples.json "$OUT/samples.json"
ls -la "$OUT"
