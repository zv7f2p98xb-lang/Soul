#!/usr/bin/env bash
# Rebuilds ca.msi (custom action test package) and its helper binaries (needs mingw + msitools). Not needed to run the tests.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -s -shared -o ca/msi_ca.dll ../win32/msi_ca.c -lmsi -Wl,--kill-at
i686-w64-mingw32-gcc -O2 -s -o ca/tool.exe ca_tool.c
rm -f ca.msi
wixl -o ca.msi ca.wxs
msibuild ca.msi -i ca_removefile.idt
# wixl does not know component conditions
msibuild ca.msi -q "UPDATE \`Component\` SET \`Condition\`='SKIPME = \"no\"' WHERE \`Component\`='CompNo'"
msibuild ca.msi -q "UPDATE \`Component\` SET \`Condition\`='CA_IN = \"hello\"' WHERE \`Component\`='CompYes'"
# type 34: run an installed program with a working directory (wixl cannot express it)
msibuild ca.msi -q "UPDATE \`CustomAction\` SET \`Type\`=34, \`Source\`='INSTALLDIR', \`Target\`='[INSTALLDIR]tool.exe hello' WHERE \`Action\`='RunTool'"
