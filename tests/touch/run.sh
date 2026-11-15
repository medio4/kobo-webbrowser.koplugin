#!/bin/sh
# Builds and runs the touch protocol tests on the desktop (image kb-qt5test, see
# tests/csscompat/run.sh; only its g++ is used).
DIR="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm --security-opt label=disable -v "$DIR:/src" -w /tmp kb-qt5test sh -c '
  g++ -O2 -Wall -o touchproto_test /src/tests/touch/touchproto_test.cpp && ./touchproto_test'
