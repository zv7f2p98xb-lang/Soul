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
x86_64-w64-mingw32-gcc -O2 -o "$OUT/net.exe" net.c -lws2_32 || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/net32.exe" net.c -lws2_32 || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/http.exe" http.c http_winhttp.c -lws2_32 -lwininet -lwinhttp -lurlmon || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/http32.exe" http.c http_winhttp.c -lws2_32 -lwininet -lwinhttp -lurlmon || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/audio.exe" audio.c -lwinmm -ldsound -ldxguid || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/audio32.exe" audio.c -lwinmm -ldsound -ldxguid || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/gdiplus.exe" gdiplus.c -lgdiplus -lgdi32 -lole32 -luuid -lshlwapi || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/gdiplus32.exe" gdiplus.c -lgdiplus -lgdi32 -lole32 -luuid -lshlwapi || exit 1
x86_64-w64-mingw32-g++ -O2 -static -o "$OUT/xaudio2.exe" xaudio2.cpp -lole32 || exit 1
i686-w64-mingw32-g++ -O2 -static -o "$OUT/xaudio2_32.exe" xaudio2.cpp -lole32 || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/msi.exe" msi.c -lmsi || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/msi32.exe" msi.c -lmsi || exit 1
x86_64-w64-mingw32-gcc -O2 -o "$OUT/d3d9.exe" d3d9.c -ld3d9 -lgdi32 -luser32 || exit 1
i686-w64-mingw32-gcc -O2 -o "$OUT/d3d932.exe" d3d9.c -ld3d9 -lgdi32 -luser32 || exit 1
cp ../msi/sample.msi ../msi/ca.msi ../msi/lzx/lzx.msi "$OUT/"
pass=0; fail=0
# the committed shader bytecode for d3d9.c must match its sources
./gen_d3d9_shaders.sh "$OUT/d3d9_shaders.h" && cmp -s "$OUT/d3d9_shaders.h" d3d9_shaders.h || { fail=$((fail+1)); echo "FAIL d3d9_shaders.h is stale (run gen_d3d9_shaders.sh)"; }
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
check net net.exe
check net net32.exe
check http --drive . http.exe
check http --drive . http32.exe
check audio --audio audio.wav audio.exe
check audio --audio audio32.wav audio32.exe
# the recorded mix holds every test tone
for wav in audio.wav audio32.wav; do
  heard=$(python3 audio_check.py "$OUT/$wav" 2>&1)
  if [ "$heard" == "$(printf 'heard: 330 440 550 660 880 990\nsound windows>=20: True')" ]; then pass=$((pass+1)); echo "PASS sound $wav";
  else fail=$((fail+1)); echo "FAIL sound $wav"; echo "$heard"; fi
done
check gdiplus gdiplus.exe
check gdiplus gdiplus32.exe
check xaudio2 --audio xaudio2.wav xaudio2.exe
check xaudio2 --audio xaudio2_32.wav xaudio2_32.exe
for wav in xaudio2.wav xaudio2_32.wav; do
  heard=$(python3 audio_check.py --tones 440,660 "$OUT/$wav" 2>&1 | tail -1)
  if [ "$heard" == "expected tones present: True" ]; then echo "ok   xaudio2 tones $wav";  # part of the xaudio2 check above, not counted twice
  else fail=$((fail+1)); echo "FAIL xaudio2 sound $wav"; echo "$heard"; fi
done
check d3d9 d3d9.exe
check d3d9 d3d932.exe
# Windows Installer: the packages write into the drive folder; clean up so other tests and reruns see a fresh drive
msi_clean() { rm -rf "$OUT/Program Files" "$OUT/Program Files (x86)" "$OUT/ProgramData" "$OUT/users" "$OUT/windows" "$OUT/Soul Test"; }
msi_clean
check msi --drive . msi.exe
msi_clean
check msi --drive . msi32.exe
msi_clean
echo "passed=$pass failed=$fail"
[ $fail -eq 0 ]
