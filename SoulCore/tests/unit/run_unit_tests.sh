#!/bin/bash
# Compiles and runs the standalone unit tests (no emulator needed).
cd "$(dirname "$0")" || exit 1
B=../../build/unit
mkdir -p "$B"
python3 inflate_vectors.py "$B/inflate" || { echo "FAIL vector generation"; echo "passed=0 failed=1"; exit 1; }
gcc -std=gnu11 -O2 -Wall -Wextra -I../../src -o "$B/inflate_test" inflate_test.c ../../src/inflate.c || { echo "FAIL build"; echo "passed=0 failed=1"; exit 1; }
"$B/inflate_test" "$B/inflate" | tee "$B/inflate_test.log"
grep -q 'failed=0$' "$B/inflate_test.log"
