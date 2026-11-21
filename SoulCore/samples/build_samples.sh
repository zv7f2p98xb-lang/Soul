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
# windowed (GUI) samples
$CC -O2 -s -mwindows -o "$OUT/Cizim.exe" cizim.c
$CC -O2 -s -mwindows -o "$OUT/Saat.exe" saat.c -lm
$CC -O2 -s -mwindows -o "$OUT/HesapMakinesiGUI.exe" hesapgui.c
$CC -O2 -s -mwindows -o "$OUT/NotDefteri.exe" notdefteri.c -lcomdlg32
$CC -O2 -s -mwindows -o "$OUT/Yilan.exe" yilan.c
$CC -O2 -s -mwindows -o "$OUT/MayinTarlasi.exe" mayin.c
# 3D (OpenGL through Soul's graphics layer)
$CC -O2 -s -mwindows -o "$OUT/Kup3B.exe" kup3b.c -lopengl32 -lglu32 -lm
cp samples.json "$OUT/samples.json"
ls -la "$OUT"
