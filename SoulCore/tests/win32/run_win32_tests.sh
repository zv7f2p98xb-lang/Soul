#!/usr/bin/env bash
# Windows-specific programs (Win32 API, C++, threads) compared against recorded output.
# Usage: tests/win32/run_win32_tests.sh [soulrun-command...]
set -u
cd "$(dirname "$0")"
RUNNER=("$@")
[ ${#RUNNER[@]} -eq 0 ] && RUNNER=(../../build/native/soulrun)
OUT=../../build/win32tests
mkdir -p "$OUT"
x86_64-w64-mingw32-gcc -O2 -o "$OUT/winapi.exe" winapi.c || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/threads.exe" threads.c || exit 1
x86_64-w64-mingw32-g++ -O2 -static -o "$OUT/cpp.exe" cpp.cpp || exit 1
pass=0; fail=0
check() {
  local name=$1; shift
  local actual
  actual=$(cd "$OUT" && timeout 300 "${RUNNER[@]/#..\/..\//../../}" --quiet "$@" 2>&1 < /dev/null)
  if [ "$actual" == "$(cat "$name.expected")" ]; then pass=$((pass+1)); echo "PASS $name";
  else fail=$((fail+1)); echo "FAIL $name"; diff <(echo "$actual") "$name.expected" | head -20; fi
}
check winapi winapi.exe hello
check threads threads.exe
check cpp cpp.exe
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
