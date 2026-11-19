#!/bin/sh
# Builds and runs the plugin-command test on the desktop (image kb-qt5test, see tests/csscompat/run.sh).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp kb-qt5test sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  moc /src/src/remotecmd.h -o moc_remotecmd.cpp && moc /src/tests/remotecmd/remotecmd_test.cpp -o remotecmd_test.moc &&
  g++ -O2 -fPIC -I. -I$Q -I$Q/QtCore -o remotecmd_test /src/tests/remotecmd/remotecmd_test.cpp /src/src/remotecmd.cpp \
      moc_remotecmd.cpp -lQt5Core && ./remotecmd_test'
