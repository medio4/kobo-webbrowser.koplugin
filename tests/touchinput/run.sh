#!/bin/sh
# Builds and runs the touch input test on the desktop (needs g++, qtbase5-dev and
# qtbase5-private-dev, e.g. Ubuntu 20.04 or later; no device needed).
set -e
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
Q=/usr/include/x86_64-linux-gnu/qt5
V="$(qmake -query QT_VERSION)"
cd "$OUT"
moc "$DIR/src/touchinput.h" -o moc_touchinput.cpp
g++ -O2 -fPIC -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtGui/$V -I$Q/QtGui/$V/QtGui -I$Q/QtCore/$V -I$Q/QtCore/$V/QtCore \
    -o touchinput_test "$DIR/tests/touchinput/touchinput_test.cpp" "$DIR/src/touchinput.cpp" "$DIR/src/rotation.cpp" \
    moc_touchinput.cpp -lQt5Gui -lQt5Core
QT_QPA_PLATFORM=offscreen ./touchinput_test
