#!/usr/bin/env bash
# Windows-specific programs (Win32 API, C++, threads) compared against recorded output,
# built as 64-bit and as 32-bit executables.
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
x86_64-w64-mingw32-gcc -O2 -o "$OUT/registry.exe" registry.c || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/richedit.exe" richedit.c || exit 1
# the same programs as 32-bit (x86) executables
i686-w64-mingw32-gcc -O2 -o "$OUT/winapi32.exe" winapi.c || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/threads32.exe" threads.c || exit 1
i686-w64-mingw32-g++ -O2 -static -o "$OUT/cpp32.exe" cpp.cpp || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/registry32.exe" registry.c || exit 1
i686-w64-mingw32-gcc -O0 -o "$OUT/x87ops32.exe" x87ops.c || exit 1
i686-w64-mingw32-g++ -O2 -static -o "$OUT/exceptions32.exe" exceptions.cpp || exit 1   # x87 register forms (reference: real hardware)
# several processes (each build starts the other one too)
x86_64-w64-mingw32-gcc -O2 -o "$OUT/multiproc.exe" multiproc.c -lole32 -luuid || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/multiproc32.exe" multiproc.c -lole32 -luuid || exit 1
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
check registry-write registry.exe yaz
check registry-read registry.exe
check richedit richedit.exe
check winapi32 winapi32.exe hello
check threads threads32.exe
check cpp cpp32.exe
check registry-write registry32.exe yaz
check registry-read registry32.exe
check x87ops x87ops32.exe
check exceptions exceptions32.exe
check multiproc --drive . multiproc.exe
check multiproc32 --drive . multiproc32.exe
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
