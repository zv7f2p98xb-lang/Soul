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
# 3D (Direct3D 9 through Soul's d3d9 layer; shader bytecode in kup3b_d3d9_shaders.h)
$CC -O2 -s -mwindows -o "$OUT/Kup3BD3D9.exe" kup3b_d3d9.c -ld3d9 -lgdi32 -luser32 -lm
# 32-bit (x86) builds: same programs through the 32-bit mode of the JIT
CC32=i686-w64-mingw32-gcc
$CC32 -O2 -s -o "$OUT/Benchmark32.exe" benchmark.c
$CC32 -O2 -s -mwindows -o "$OUT/Kup3B32.exe" kup3b.c -lopengl32 -lglu32 -lm
cp samples.json "$OUT/samples.json"
ls -la "$OUT"
