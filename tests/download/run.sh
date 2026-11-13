#!/bin/sh
# Builds and runs the download tests on the desktop (image kb-qt5webkit, see
# tests/csscompat/run-pageload.sh).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp -e QT_QPA_PLATFORM=offscreen kb-qt5webkit sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  moc /src/src/download.h -o moc_download.cpp && moc /src/tests/download/download_test.cpp -o download_test.moc &&
  g++ -O2 -fPIC -I. -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtWidgets -I$Q/QtNetwork -I$Q/QtWebKit -I$Q/QtWebKitWidgets \
      -o download_test /src/tests/download/download_test.cpp /src/src/download.cpp moc_download.cpp \
      -lQt5WebKitWidgets -lQt5WebKit -lQt5Widgets -lQt5Network -lQt5Gui -lQt5Core && ./download_test'
