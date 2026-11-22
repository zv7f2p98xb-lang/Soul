#!/usr/bin/env bash
# Windows Installer tests: msi.dll API + msiexec.exe (native and, when available, JIT under qemu-aarch64),
# and the msiexec full UI driven by GUI scripts. Usage: tests/msi/run_msi_tests.sh
set -u
cd "$(dirname "$0")"
HERE=$(pwd)
CORE=$HERE/../..
OUT=$CORE/build/win32tests
mkdir -p "$OUT"
export TMPDIR=${TMPDIR:-$(mktemp -d)}
x86_64-w64-mingw32-gcc -O2 -o "$OUT/msi.exe" ../win32/msi.c -lmsi || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/msi32.exe" ../win32/msi.c -lmsi || exit 1
cp sample.msi ca.msi "$OUT/"
pass=0; fail=0
clean() { rm -rf "$OUT/Program Files" "$OUT/Program Files (x86)" "$OUT/ProgramData" "$OUT/users" "$OUT/windows" "$OUT/Soul Test"; }
check() { # name command...
  local name=$1; shift
  clean
  local actual; actual=$(cd "$OUT" && timeout 600 "$@" 2>&1 < /dev/null)
  if [ "$actual" == "$(cat "$CORE/tests/win32/msi.expected")" ]; then pass=$((pass+1)); echo "PASS $name"
  else fail=$((fail+1)); echo "FAIL $name"; diff <(echo "$actual") "$CORE/tests/win32/msi.expected" | head -20; fi
}
check msi-64 ../native/soulrun --quiet --drive . msi.exe
check msi-32 ../native/soulrun --quiet --drive . msi32.exe
if command -v qemu-aarch64 >/dev/null && [ -x "$CORE/build/arm64/soulrun" ]; then
  check msi-64-jit qemu-aarch64 ../arm64/soulrun --quiet --drive . msi.exe
  check msi-32-jit qemu-aarch64 ../arm64/soulrun --quiet --drive . msi32.exe
fi
clean
# GUI: install and remove through the wizard
(cd "$CORE/tools/msiexec" && ./build.sh >/dev/null) || exit 1
D=$(mktemp -d)
cp "$CORE/tools/msiexec/msiexec.exe" sample.msi "$D/"
guirun() { (cd "$D" && timeout 120 "$CORE/build/native/soulrun" --quiet --drive "$D" --gui-script "$CORE/tests/gui/$1.script" msiexec.exe "${@:2}" 2>&1 < /dev/null); }
guirun msiexec /i 'C:\sample.msi' > "$D/out1.txt"
if [ -f "$D/Program Files (x86)/Soul Örnek/readme.txt" ] && [ ! -s "$D/out1.txt" ]; then pass=$((pass+1)); echo "PASS msiexec-gui-install"; else fail=$((fail+1)); echo "FAIL msiexec-gui-install"; cat "$D/out1.txt"; fi
guirun msiexec_remove /x '{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}' > "$D/out2.txt"
if [ ! -e "$D/Program Files (x86)/Soul Örnek" ] && [ ! -s "$D/out2.txt" ]; then pass=$((pass+1)); echo "PASS msiexec-gui-remove"; else fail=$((fail+1)); echo "FAIL msiexec-gui-remove"; cat "$D/out2.txt"; fi
rm -rf "$D"
echo "msi tests: $pass passed, $fail failed"
[ $fail -eq 0 ]
