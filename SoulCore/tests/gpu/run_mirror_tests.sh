#!/usr/bin/env bash
# Runs the win32 and GUI suites with the sgpu "mirror" debug backend (SOUL_GPU=mirror): render targets and
# textures live in shadow copies that are only synchronised through the backend hooks, and every
# N-th draw (SOUL_GPU_MIRROR_DECLINE, default 7) is declined to exercise the CPU fallback. The results
# must be identical to the plain runs - any missing sync / cpu_wrote / sg_texture_touch call in a front
# end shows up as stale pixels. The msi suite does not draw and is not part of this.
# Usage: tests/gpu/run_mirror_tests.sh [DECLINE_EVERY_N]   (extra env: SOUL_GPU_MIRROR_STRICT=1 aborts on protocol violations)
set -u
cd "$(dirname "$0")/.." || exit 1
export SOUL_GPU=mirror
export SOUL_GPU_MIRROR_DECLINE="${1:-${SOUL_GPU_MIRROR_DECLINE:-7}}"
export SOUL_GPU_MIRROR_STRICT="${SOUL_GPU_MIRROR_STRICT:-1}"
status=0
for suite in win32/run_win32_tests.sh gui/run_gui_tests.sh; do
  echo "=== $suite with SOUL_GPU=mirror (decline every $SOUL_GPU_MIRROR_DECLINE)"
  out=$(bash "$suite" 2>&1)
  echo "$out" | grep -E "^FAIL|passed=" 
  echo "$out" | grep -q "failed=0$" || status=1
done
[ $status -eq 0 ] && echo "mirror: all suites identical" || echo "mirror: FAILURES"
exit $status
