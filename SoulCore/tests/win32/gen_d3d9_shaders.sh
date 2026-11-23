#!/usr/bin/env bash
# Regenerates d3d9_shaders.h (the shader bytecode used by d3d9.c) from d3d9_shaders/*.asm with the test assembler.
# Usage: tests/win32/gen_d3d9_shaders.sh [output.h]
cd "$(dirname "$0")" || exit 1
OUT=${1:-d3d9_shaders.h}
python3 ../unit/shaders/d3dasm.py "$OUT" d3d9_shaders/wvp.asm d3d9_shaders/sm3.asm d3d9_shaders/legacy.asm
