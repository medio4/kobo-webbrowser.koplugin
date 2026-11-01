#!/bin/sh
# Cross-compile kobo-browser with NickelTC (gcc 4.9.4, Qt 5.2.1, glibc 2.19) in a container.
# Mounts only the kobo/ project folder's browser and downloads subfolders; no SELinux relabeling.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
mkdir -p "$HERE/build"
podman run --rm --security-opt label=disable \
  -v "$HERE":/work/browser -v "$ROOT/downloads":/work/downloads:ro \
  -w /work/browser/build ghcr.io/pgaskin/nickeltc:1.0 \
  sh -c 'qmake -spec linux-arm-nickel-gnueabihf-g++ ../browser.pro && make -j"$(nproc)" && arm-nickel-linux-gnueabihf-strip kobo-browser && ls -la kobo-browser'
