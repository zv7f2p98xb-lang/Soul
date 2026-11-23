# Sourced by the test runner scripts: portability helpers for macOS (BSD userland, bash 3.2, no GNU timeout)
# and the "prebuilt" mode used by the macOS CI job.
#
#  * timeout: macOS has no GNU timeout. Uses gtimeout (brew coreutils) when present, else a perl alarm wrapper.
#    (exit status 124 on GNU timeout, 142 with the perl fallback)
#  * SOUL_PREBUILT=1: the Windows test executables (and the reference outputs of tests/run_tests.sh) were built
#    on another machine and unpacked into build/ - the MinGW compiler calls become no-ops and the native
#    reference build is skipped. Missing executables then show up as FAILs when the tests run.
if ! command -v timeout >/dev/null 2>&1; then
  if command -v gtimeout >/dev/null 2>&1; then
    timeout() { gtimeout "$@"; }
  else
    timeout() { local secs=$1; shift; perl -e 'alarm shift; exec @ARGV or exit 127' "$secs" "$@"; }
  fi
fi
if [ -n "${SOUL_PREBUILT:-}" ]; then
  for _arch in x86_64 i686; do
    for _tool in gcc g++ windres; do
      eval "${_arch}-w64-mingw32-${_tool}() { :; }"
    done
  done
  unset _arch _tool
fi
# directory under build/ that holds the host's soulrun (the default runner of the scripts)
if [ "$(uname -s)" = Darwin ]; then SOUL_HOST_BUILD=macos; else SOUL_HOST_BUILD=native; fi
