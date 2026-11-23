#!/usr/bin/env bash
# Rebuilds lzx.msi: wixl builds the package from lzx.wxs, then its embedded cabinet is replaced by an LZX cabinet made
# with lzxenc.py (needs msitools, cabextract, python3). Not needed to run the tests. Run lzxenc.py fixtures first.
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
rm -f lzx.msi
wixl -o lzx.msi lzx.wxs
# cabinet members in File.Sequence order (the names are the File table keys)
ARGS=()
for id in $(msiinfo export lzx.msi File | tail -n +4 | sort -t$'\t' -k8,8n | cut -f1); do
  case $id in
    tableDat) f=table.dat;; codeExe) f=code.exe;; tinyTxt) f=tiny.txt;; emptyBin) f=empty.bin;; textTxt) f=text.txt;;
    *) echo "unknown file id $id"; exit 1;;
  esac
  ARGS+=("$id=src/$f")
done
echo "cabinet order: ${ARGS[*]}"
python3 lzxenc.py cab "$TMP/cab1.cab" --window 21 --e8 0x1000000 --plan v:70000,a:70000,u:20001 "${ARGS[@]}"
msibuild lzx.msi -q "DELETE FROM \`_Streams\` WHERE \`Name\`='cab1.cab'"
msibuild lzx.msi -a cab1.cab "$TMP/cab1.cab"
rm -rf "$TMP"
