#!/usr/bin/env bash
# GPU backend tests: the 3D programs rendered with SOUL_GPU=<backend> must match the CPU expectations
# within a small colour tolerance, then the two 3D samples are timed with and without the GPU.
#
# Usage: tests/gpu/run_gpu_tests.sh [soulrun-command...]     (default: build/native/soulrun, build/macos/soulrun on macOS)
#   SOUL_GPU=metal|mirror|...   backend to test (default metal). "metal" with a binary that has no Metal backend
#                               prints "skipped" and exits 0 (so CI stays green until the backend exists).
#   SOUL_PIXEL_TOLERANCE=N      allowed difference per colour channel (default 2; 0 for the exact "mirror" backend)
#   SOUL_PREBUILT=1             use the Windows test executables already in build/win32tests and build/guitests
#                               (no MinGW needed)
#   SAMPLES_DIR=DIR             the built sample programs (Kup3B.exe, Kup3BD3D9.exe; default build/samples)
#   GPU_BENCH=0|1               frame-rate comparison of the 3D samples (default: on for metal, off otherwise)
#   GPU_TESTS=0|1               0 = skip the correctness tests, only run the frame-rate comparison (default 1)
#   GPU_BENCH_SECONDS=N         seconds each sample runs (default 8)
#   SOUL_NO_JIT=0|1             1 = interpreter only, 0 = JIT (honoured by soulrun itself; the CI benchmark uses 0)
# Every run prints soulrun's "gpu stats:" line. A test FAILS when the backend accepted no draw at all
# although draws were offered (gpu=0, draws>0): the output would then be the CPU's and prove nothing.
# Exit status: 0 = all passed (or skipped), 1 = failures.
set -u
cd "$(dirname "$0")/../.." || exit 1
CORE=$(pwd)
. tests/portable.sh
RUNNER=("$@")
[ ${#RUNNER[@]} -eq 0 ] && RUNNER=("build/$SOUL_HOST_BUILD/soulrun")
LAST=$((${#RUNNER[@]} - 1))
case "${RUNNER[$LAST]}" in /*) ;; *) RUNNER[$LAST]="$CORE/${RUNNER[$LAST]#../../}" ;; esac

BACKEND=${SOUL_GPU:-metal}
case "$BACKEND" in
  metal) TOL_DEFAULT=2 ;;
  *) TOL_DEFAULT=0 ;;
esac
TOL=${SOUL_PIXEL_TOLERANCE:-$TOL_DEFAULT}
SECS=${GPU_BENCH_SECONDS:-8}
if [ -z "${GPU_BENCH:-}" ]; then [ "$BACKEND" = metal ] && GPU_BENCH=1 || GPU_BENCH=0; fi
SAMPLES_DIR=${SAMPLES_DIR:-$CORE/build/samples}

info=$(SOUL_GPU="$BACKEND" "${RUNNER[@]}" --gpu-info 2>&1 < /dev/null | tail -1)
case "$info" in
  "gpu backend: none"|"") echo "gpu: SKIPPED - the runner has no '$BACKEND' backend here (--gpu-info said: ${info:-nothing})"; exit 0 ;;
  "gpu backend: "*) echo "gpu: backend '${info#gpu backend: }' selected by SOUL_GPU=$BACKEND, tolerance +-$TOL per channel" ;;
  *) echo "gpu: FAIL - could not query the runner (--gpu-info said: $info)"; exit 1 ;;
esac

OUT="$CORE/build/gputests"
rm -rf "$OUT/run" && mkdir -p "$OUT/run"
RUN="$OUT/run"
if [ -n "${SOUL_PREBUILT:-}" ]; then
  cp "$CORE/build/win32tests/d3d9.exe" "$CORE/build/win32tests/d3d932.exe" "$RUN/" || exit 1
  cp "$CORE/build/guitests/kup3b.exe" "$CORE/build/guitests/kup3b32.exe" "$RUN/" || exit 1
  cp "$CORE/build/guitests/d3d9.exe" "$RUN/d3d9gui.exe" && cp "$CORE/build/guitests/d3d932.exe" "$RUN/d3d9gui32.exe" || exit 1
else
  W=tests/win32
  x86_64-w64-mingw32-gcc -O2 -o "$RUN/d3d9.exe" $W/d3d9.c -ld3d9 -lgdi32 -luser32 || exit 1
  i686-w64-mingw32-gcc -O2 -o "$RUN/d3d932.exe" $W/d3d9.c -ld3d9 -lgdi32 -luser32 || exit 1
  cp "$RUN/d3d9.exe" "$RUN/d3d9gui.exe"; cp "$RUN/d3d932.exe" "$RUN/d3d9gui32.exe"
  x86_64-w64-mingw32-gcc -O2 -mwindows -o "$RUN/kup3b.exe" samples/kup3b.c -lopengl32 -lglu32 -lm || exit 1
  i686-w64-mingw32-gcc -O2 -mwindows -o "$RUN/kup3b32.exe" samples/kup3b.c -lopengl32 -lglu32 -lm || exit 1
fi

pass=0; fail=0
tot_draws=0; tot_gpu=0
# stats_of FILE LABEL: print the "gpu stats:" line of a run's stderr, count it, and return 1 when the backend drew nothing
stats_of() {
  local line d g dec; line=$(grep '^gpu stats:' "$1" | tail -1)
  if [ -z "$line" ]; then echo "     (no gpu stats line for $2)"; return 0; fi
  echo "     $line"
  d=$(echo "$line" | sed -n 's/.* draws=\([0-9]*\).*/\1/p'); g=$(echo "$line" | sed -n 's/.* gpu=\([0-9]*\).*/\1/p')
  dec=$(echo "$line" | sed -n 's/.* declined=\([0-9]*\).*/\1/p')
  d=${d:-0}; g=${g:-0}; dec=${dec:-0}
  tot_draws=$((tot_draws + d)); tot_gpu=$((tot_gpu + g))
  [ "$d" -gt 0 ] && echo "     declined to the CPU: $dec of $d draws ($((dec * 100 / d))%)"
  if [ "$d" -gt 0 ] && [ "$g" -eq 0 ]; then
    fail=$((fail+1)); echo "FAIL $2: the '$BACKEND' backend accepted none of $d draws (everything was drawn by the CPU)"; return 1
  fi
  return 0
}
cmp_out() { # name expected-file actual-file
  local r; r=$(python3 -I tests/gpu/compare.py --tol "$TOL" "$2" "$3" 2>&1); local c=$?
  if [ $c -eq 0 ]; then pass=$((pass+1)); echo "PASS $1 ($(echo "$r" | tail -1))"
  else fail=$((fail+1)); echo "FAIL $1"; echo "$r" | head -25; fi
}

if [ "${GPU_TESTS:-1}" = 1 ]; then
# 1. d3d9 console test (every pixel check of the CPU run, GPU result within tolerance)
for exe in d3d9 d3d932; do
  (cd "$RUN" && SOUL_GPU="$BACKEND" SOUL_PIXEL_TOLERANCE="$TOL" timeout 900 "${RUNNER[@]}" --quiet --gpu-stats $exe.exe > "$exe.out" 2> "$exe.err" < /dev/null)
  cmp_out "d3d9-console $exe" tests/win32/d3d9.expected "$RUN/$exe.out"
  stats_of "$RUN/$exe.err" "d3d9-console $exe"
  grep -v '^gpu stats:' "$RUN/$exe.err" | head -5
done

# 2. GUI scripts with pixel checks (the script's "pixel" command honours SOUL_PIXEL_TOLERANCE)
gui() { # label exe-and-args script expected
  local label=$1 exe=$2 script=$3 exp=$4
  (cd "$RUN" && SOUL_GPU="$BACKEND" SOUL_PIXEL_TOLERANCE="$TOL" timeout 600 "${RUNNER[@]}" --quiet --gpu-stats --drive "$RUN/drive" --gui-script "$CORE/tests/gui/$script.script" $exe > "$label.out" 2> "$label.err" < /dev/null)
  local code=$?
  if [ $code -eq 99 ]; then fail=$((fail+1)); echo "FAIL $label (script pixel check failed)"; tail -10 "$RUN/$label.err"; return; fi
  if [ $code -ne 0 ] && [ ! -s "$RUN/$label.out" ]; then fail=$((fail+1)); echo "FAIL $label (exit $code)"; tail -10 "$RUN/$label.err"; return; fi
  cmp_out "$label" "tests/gui/$exp.expected" "$RUN/$label.out"
  stats_of "$RUN/$label.err" "$label"
}
rm -rf "$RUN/drive"
gui opengl-x64 "kup3b.exe --sabit" opengl opengl
gui d3d9-gui-x64 "d3d9gui.exe --gui" d3d9 d3d9
gui opengl-x86 "kup3b32.exe --sabit" opengl opengl
gui d3d9-gui-x86 "d3d9gui32.exe --gui" d3d9 d3d9

fi   # GPU_TESTS

# 3. frame rates of the samples (window title "... N FPS", read through the GUI script's "print"), with the stats line.
# The title is printed several times (the sample refreshes it once a second); the last FPS found wins. The CPU run
# must not be started with `env -u ...`: on macOS `timeout` is a shell function here (portable.sh), which env cannot run.
fps() { # exe backend-or-empty -> prints "FPS" (or "?"); the stats line goes to $RUN/bench-last.stats
  local exe=$1 be=$2 tag; tag=$(basename "$exe" .exe)-${be:-cpu}
  { printf 'waitwin 1\n'; local k; for k in 1 2 3; do printf 'wait %d\nprint\n' $((SECS * 1000 / 3)); done; printf 'close\n'; } > "$RUN/bench.script"
  local err="$RUN/bench-$tag.err"
  ( cd "$RUN"
    if [ -n "$be" ]; then export SOUL_GPU="$be"; else unset SOUL_GPU; fi
    timeout $((SECS + 120)) "${RUNNER[@]}" --quiet --gpu-stats --drive "$RUN/drive" --gui-script "$RUN/bench.script" "$exe" > /dev/null 2> "$err" < /dev/null )
  local f; f=$(sed -n 's/.*[^0-9]\([0-9][0-9]*\) FPS.*/\1/p' "$err" | tail -1)
  grep '^gpu stats:' "$err" | tail -1 > "$RUN/bench-last.stats"
  if [ -z "$f" ]; then echo "     (no FPS in the window title for $tag; log tail:)" >&2; tail -5 "$err" | sed 's/^/     | /' >&2; fi
  echo "${f:-?}"
}
if [ "$GPU_BENCH" = 1 ]; then
  echo "--- frame rates, $SECS s per run (window title FPS; higher is better; SOUL_NO_JIT=${SOUL_NO_JIT:-unset})"
  printf '%-26s %10s %10s\n' sample cpu "$BACKEND"
  stats_lines=""
  for s in "Kup3B.exe|Küp 3B (OpenGL)" "Kup3BD3D9.exe|Küp 3B (Direct3D 9)"; do
    f=${s%%|*}; n=${s#*|}
    if [ ! -f "$SAMPLES_DIR/$f" ]; then echo "(sample $f not found in $SAMPLES_DIR, skipped)"; continue; fi
    cp "$SAMPLES_DIR/$f" "$RUN/$f"
    c=$(fps "$f" ""); cs=$(cat "$RUN/bench-last.stats" 2>/dev/null)
    g=$(fps "$f" "$BACKEND"); gs=$(cat "$RUN/bench-last.stats" 2>/dev/null)
    printf '%-26s %10s %10s\n' "$n" "$c" "$g"
    stats_lines="$stats_lines  $n, $BACKEND run: ${gs:-(no stats line)}
"
  done
  echo "--- gpu stats of the frame-rate runs"
  printf '%s' "$stats_lines"
fi

if [ "$BACKEND" != cpu ] && [ "$tot_draws" -gt 0 ] && [ "$tot_gpu" -eq 0 ]; then fail=$((fail+1)); echo "FAIL: no draw of the whole run was accepted by '$BACKEND'"; fi
echo "gpu draws over the tests: offered=$tot_draws accepted=$tot_gpu"
echo "gpu tests ($BACKEND, +-$TOL): passed=$pass failed=$fail"
[ $fail -eq 0 ]
