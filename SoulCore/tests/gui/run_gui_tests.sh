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
pass=0; fail=0
check() {
  local name=$1 exe=$2
  local actual
  actual=$(cd "$OUT" && timeout 300 "${RUNNER[@]}" --quiet --gui-script "$HERE/$name.script" "$exe" 2>"$OUT/$name.stderr" < /dev/null)
  local code=$?
  if [ "$actual" == "$(cat "$name.expected")" ] && [ $code -ne 99 ]; then pass=$((pass+1)); echo "PASS $name";
  else fail=$((fail+1)); echo "FAIL $name (exit $code)"; diff <(echo "$actual") "$name.expected" | head -20; tail -20 "$OUT/$name.stderr"; fi
}
check basic gui_basic.exe
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
