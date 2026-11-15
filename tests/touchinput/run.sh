#!/bin/sh
# Builds and runs the touch input test on the desktop: evdev frames go into a FIFO that
# stands in for /dev/input/eventN, and the mouse events the window gets are checked.
# Image kb-qt5private = kb-qt5test (see tests/csscompat/run.sh) + qtbase5-private-dev.
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
podman image exists kb-qt5private || {
  podman run --name kbp -e DEBIAN_FRONTEND=noninteractive kb-qt5test sh -c 'apt-get update -qq && apt-get install -y -qq qtbase5-private-dev >/dev/null' &&
  podman commit kbp kb-qt5private >/dev/null && podman rm kbp >/dev/null; }
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp -e QT_QPA_PLATFORM=offscreen kb-qt5private sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  V="$(qmake -query QT_VERSION)"
  moc /src/src/touchinput.h -o moc_touchinput.cpp &&
  g++ -std=c++11 -O2 -fPIC -I/src/src -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtGui/$V -I$Q/QtGui/$V/QtGui -I$Q/QtCore/$V -I$Q/QtCore/$V/QtCore \
      -o touchinput_test /src/tests/touchinput/touchinput_test.cpp /src/src/touchinput.cpp /src/src/rotation.cpp \
      moc_touchinput.cpp -lQt5Gui -lQt5Core && ./touchinput_test'
