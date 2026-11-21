#!/usr/bin/env bash
# Differential tests: each portable C program is compiled natively (reference output)
# and as Windows x64 and x86 (32-bit) .exe files with MinGW, then executed through soulrun.
# Usage: tests/run_tests.sh [soulrun-command...]   (default: build/native/soulrun)
set -u
cd "$(dirname "$0")/.."
RUNNER=("$@")
[ ${#RUNNER[@]} -eq 0 ] && RUNNER=(build/native/soulrun)
OUT=build/tests
mkdir -p "$OUT"
pass=0; fail=0
for src in tests/programs/*.c; do
  name=$(basename "$src" .c)
  input=""
  [ -f "tests/programs/$name.in" ] && input="tests/programs/$name.in"
  cc -O2 -o "$OUT/$name.native" "$src" -lm 2>/dev/null || { echo "SKIP $name (native build failed)"; continue; }
  if [ -n "$input" ]; then (cd "$OUT" && ./"$name.native" < "../../$input" > "$name.expected"); else (cd "$OUT" && ./"$name.native" > "$name.expected"); fi
  for arch in x64 x86; do
  cc_win=x86_64-w64-mingw32-gcc
  [ "$arch" = x86 ] && cc_win=i686-w64-mingw32-gcc
  for opt in O0 O2; do
    for crt in msvcrt ucrt; do
      exe="$OUT/${name}_${arch}_${opt}_${crt}.exe"
      flags="-$opt"
      [ "$crt" = ucrt ] && flags="$flags -nodefaultlibs -lmingw32 -lgcc -lmingwex -lucrt -lkernel32 -lgcc"
      if ! $cc_win -o "$exe" "$src" $flags -lm 2>/dev/null; then echo "SKIP $name/$arch/$opt/$crt (mingw build failed)"; continue; fi
      if [ -n "$input" ]; then actual=$(cd "$OUT" && timeout 120 "${RUNNER[@]/#build/../../build}" --quiet "$(basename "$exe")" < "../../$input" 2>&1); else actual=$(cd "$OUT" && timeout 120 "${RUNNER[@]/#build/../../build}" --quiet "$(basename "$exe")" 2>&1 < /dev/null); fi
      if [ "$actual" == "$(cat "$OUT/$name.expected")" ]; then pass=$((pass+1)); echo "PASS $name/$arch/$opt/$crt";
      else fail=$((fail+1)); echo "FAIL $name/$arch/$opt/$crt"; diff <(echo "$actual") "$OUT/$name.expected" | head -10; fi
    done
  done
  done
done
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
