#!/bin/sh
# Builds and runs the dead link tests on the desktop (image kb-qt5webkit, see
# tests/csscompat/run-pageload.sh).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp -e QT_QPA_PLATFORM=offscreen -e VERBOSE kb-qt5webkit sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  g++ -O2 -fPIC -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtWidgets -I$Q/QtNetwork -I$Q/QtWebKit -I$Q/QtWebKitWidgets \
      -o deadlinks_test /src/tests/deadlinks/deadlinks_test.cpp \
      -lQt5WebKitWidgets -lQt5WebKit -lQt5Widgets -lQt5Network -lQt5Gui -lQt5Core && ./deadlinks_test'
