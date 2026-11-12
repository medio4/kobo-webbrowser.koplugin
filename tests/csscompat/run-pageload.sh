#!/bin/sh
# Builds and runs pageload.cpp (kb-qt5webkit image: kb-qt5test + libqt5webkit5-dev). Args: URLs.
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp -e QT_QPA_PLATFORM=offscreen kb-qt5webkit sh -c '
  Q=/usr/include/x86_64-linux-gnu/qt5
  moc -I/src /src/memtune.h -o moc_memtune.cpp && moc -I/src /src/adblock.h -o moc_adblock.cpp &&
  g++ -O2 -fPIC -I/src -I$Q -I$Q/QtCore -I$Q/QtGui -I$Q/QtWidgets -I$Q/QtNetwork -I$Q/QtWebKit -I$Q/QtWebKitWidgets \
      -o pageload /src/tests/csscompat/pageload.cpp /src/src/memtune.cpp /src/src/adblock.cpp /src/src/csscompat.cpp moc_memtune.cpp moc_adblock.cpp \
      -lQt5WebKitWidgets -lQt5WebKit -lQt5Widgets -lQt5Network -lQt5Gui -lQt5Core -ldl && ./pageload "$@"' sh "$@"
