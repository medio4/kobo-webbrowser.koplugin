#!/bin/sh
# Builds and runs the dark page tests on the desktop (image kb-qt5webkit, see
# tests/csscompat/run-pageload.sh).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp -e QT_QPA_PLATFORM=offscreen kb-qt5webkit sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  g++ -O2 -fPIC -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtWidgets -I$Q/QtNetwork -I$Q/QtWebKit -I$Q/QtWebKitWidgets \
      -o lightpage_test /src/tests/lightpage/lightpage_test.cpp \
      -lQt5WebKitWidgets -lQt5WebKit -lQt5Widgets -lQt5Network -lQt5Gui -lQt5Core && ./lightpage_test'
