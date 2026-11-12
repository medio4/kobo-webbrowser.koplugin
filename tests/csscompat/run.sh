#!/bin/sh
# Builds and runs the csscompat tests on the desktop in an Ubuntu 20.04 container with qtbase5-dev
# (image kb-qt5test: `podman run ubuntu:20.04 ... apt-get install g++ qtbase5-dev`, then commit).
# Extra arguments: CSS files to time (paths inside the repository or under /data, see DATA).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DATA="${DATA:-$DIR}"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -v "$DATA:/data" -w /src/tests/csscompat kb-qt5test sh -c '
  g++ -O2 -fPIC -I/usr/include/x86_64-linux-gnu/qt5 -I/usr/include/x86_64-linux-gnu/qt5/QtCore \
      -o /tmp/csscompat_test csscompat_test.cpp ../../src/csscompat.cpp -lQt5Core && /tmp/csscompat_test "$@"' sh "$@"
