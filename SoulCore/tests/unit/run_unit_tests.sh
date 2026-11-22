#!/bin/bash
# Compiles and runs the standalone unit tests (no emulator needed).
cd "$(dirname "$0")" || exit 1
B=../../build/unit
mkdir -p "$B"
python3 inflate_vectors.py "$B/inflate" || { echo "FAIL vector generation"; echo "passed=0 failed=1"; exit 1; }
gcc -std=gnu11 -O2 -Wall -Wextra -I../../src -o "$B/inflate_test" inflate_test.c ../../src/inflate.c || { echo "FAIL build"; echo "passed=0 failed=1"; exit 1; }
"$B/inflate_test" "$B/inflate" | tee "$B/inflate_test.log"
grep -q 'failed=0$' "$B/inflate_test.log" || exit 1

# MSI / CAB reader tests (plain build, then once more under ASan/UBSan when the compiler supports it).
SRCS="msidb_test.c ../../src/msi_db.c ../../src/cab.c ../../src/inflate.c"
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I../../src -o "$B/msidb_test" $SRCS || { echo "FAIL msidb build"; echo "passed=0 failed=1"; exit 1; }
"$B/msidb_test" ../msi | tee "$B/msidb_test.log"
grep -q 'failed=0$' "$B/msidb_test.log" || exit 1
echo 'int main(void){return 0;}' > "$B/san_probe.c"
if gcc -fsanitize=address,undefined -o "$B/san_probe" "$B/san_probe.c" 2>/dev/null && "$B/san_probe"; then
    echo "--- msidb_test under -fsanitize=address,undefined"
    gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I../../src -o "$B/msidb_test_asan" $SRCS || { echo "FAIL msidb asan build"; echo "passed=0 failed=1"; exit 1; }
    "$B/msidb_test_asan" ../msi | tee "$B/msidb_test_asan.log"
    grep -q 'failed=0$' "$B/msidb_test_asan.log" || exit 1
else
    echo "(sanitizers unavailable, skipping the ASan run)"
fi
