#!/usr/bin/env bash
# Windowed (GUI) programs driven by soulrun --gui-script and compared against recorded output.
# Usage: tests/gui/run_gui_tests.sh [soulrun-command...]
set -u
cd "$(dirname "$0")"
HERE=$(pwd)
RUNNER=("$@")
[ ${#RUNNER[@]} -eq 0 ] && RUNNER=("$HERE/../../build/native/soulrun")
case "${RUNNER[-1]}" in /*) ;; *) RUNNER[-1]="$HERE/../../${RUNNER[-1]#../../}" ;; esac
OUT="$HERE/../../build/guitests"
mkdir -p "$OUT"
x86_64-w64-mingw32-windres gui_basic.rc -O coff -o "$OUT/gui_basic.res" || exit 1
x86_64-w64-mingw32-gcc -O2 -mwindows -o "$OUT/gui_basic.exe" gui_basic.c "$OUT/gui_basic.res" -lcomctl32 || exit 1
x86_64-w64-mingw32-gcc -O2 -mwindows -o "$OUT/notepad.exe" ../../samples/notdefteri.c -lcomdlg32 || exit 1
x86_64-w64-mingw32-gcc -O2 -mwindows -o "$OUT/kup3b.exe" ../../samples/kup3b.c -lopengl32 -lglu32 -lm || exit 1
# the same programs as 32-bit (x86) executables
i686-w64-mingw32-windres gui_basic.rc -O coff -o "$OUT/gui_basic32.res" || exit 1
i686-w64-mingw32-gcc -O2 -mwindows -o "$OUT/gui_basic32.exe" gui_basic.c "$OUT/gui_basic32.res" -lcomctl32 || exit 1
i686-w64-mingw32-gcc -O2 -mwindows -o "$OUT/notepad32.exe" ../../samples/notdefteri.c -lcomdlg32 || exit 1
i686-w64-mingw32-gcc -O2 -mwindows -o "$OUT/kup3b32.exe" ../../samples/kup3b.c -lopengl32 -lglu32 -lm || exit 1
rm -rf "$OUT/drive"
pass=0; fail=0
check() {
  local name=$1 exe=$2 label=${3:-$1}
  local actual
  actual=$(cd "$OUT" && timeout 300 "${RUNNER[@]}" --quiet --drive "$OUT/drive" --gui-script "$HERE/$name.script" $exe 2>"$OUT/$name.stderr" < /dev/null)
  local code=$?
  if [ "$actual" == "$(cat "$name.expected")" ] && [ $code -ne 99 ]; then pass=$((pass+1)); echo "PASS $label";
  else fail=$((fail+1)); echo "FAIL $label (exit $code)"; diff <(echo "$actual") "$name.expected" | head -20; tail -20 "$OUT/$name.stderr"; fi
}
check basic gui_basic.exe
check notepad notepad.exe
check opengl "kup3b.exe --sabit"
if [ "$(cat "$OUT/drive/users/soul/Documents/deneme.txt" 2>/dev/null)" == "Soul test" ]; then pass=$((pass+1)); echo "PASS notepad-file";
else fail=$((fail+1)); echo "FAIL notepad-file: $(cat "$OUT/drive/users/soul/Documents/deneme.txt" 2>&1)"; fi
rm -f "$OUT/drive/users/soul/Documents/deneme.txt"
check basic gui_basic32.exe basic-x86
check notepad notepad32.exe notepad-x86
check opengl "kup3b32.exe --sabit" opengl-x86
if [ "$(cat "$OUT/drive/users/soul/Documents/deneme.txt" 2>/dev/null)" == "Soul test" ]; then pass=$((pass+1)); echo "PASS notepad-file-x86";
else fail=$((fail+1)); echo "FAIL notepad-file-x86: $(cat "$OUT/drive/users/soul/Documents/deneme.txt" 2>&1)"; fi
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
