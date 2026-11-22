#!/bin/bash
# Regenerates sample.msi, reference dumps and test cabinets (needs msitools + gcab). Not needed to run the tests.
cd "$(dirname "$0")" || exit 1
wixl -o sample.msi sample.wxs || exit 1
rm -rf expected; mkdir expected
for t in $(msiinfo tables sample.msi); do msiinfo export sample.msi "$t" > "expected/$t.idt"; done
msiinfo suminfo sample.msi > expected/_suminfo.txt
(cd cabs && rm -f stored.cab mszip.cab && gcab -c stored.cab mixed.dat runs.dat && gcab -c -z mszip.cab mixed.dat runs.dat)
