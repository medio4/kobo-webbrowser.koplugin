# Building

The browser is cross-compiled for the Kobo with [NickelTC](https://github.com/pgaskin/NickelTC),
Kobo's own toolchain setup (GCC 4.9.4, glibc 2.19, Qt 5.2.1), inside a podman (or docker)
container. It links against the QtWebKit that ships in the Kobo firmware, so you need two
things NickelTC doesn't include: the QtWebKit 5.2.1 headers and the firmware's QtWebKit
libraries.

## 1. Prerequisites

- Linux with `podman` (docker works too: replace `podman` with `docker` in `build.sh`).
- `curl`, `unzip`, `tar`, `zip`.
- About 3 GB of disk for the toolchain image, firmware and headers.

## 2. Folder layout

`build.sh` expects a `downloads/` folder next to the repository:

```
work/
├── kobo-webbrowser.koplugin/        this repository
└── downloads/
    ├── qtwebkit-headers/qtwebkit-opensource-src-5.2.1/include/...
    └── firmware-4.46.23852/extracted/root/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/
        ├── libQtWebKit.so.4.6.2
        └── libQtWebKitWidgets.so.4.6.2
```

`lib/libQt5WebKit.so` and `lib/libQt5WebKitWidgets.so` in this repo are symlinks into
that firmware folder.

## 3. Fetch the dependencies

```sh
cd work
mkdir -p downloads && cd downloads

# QtWebKit 5.2.1 headers (only the include/ tree is used)
curl -LO https://download.qt.io/new_archive/qt/5.2/5.2.1/submodules/qtwebkit-opensource-src-5.2.1.tar.xz
mkdir -p qtwebkit-headers
tar -C qtwebkit-headers -xJf qtwebkit-opensource-src-5.2.1.tar.xz

# Kobo firmware: take the two QtWebKit libraries out of KoboRoot.tgz.
# The URL is for the Clara BW; any firmware 4.x with Qt 5.2.1 has the same libraries.
FW=4.46.23852
mkdir -p firmware-$FW/extracted/root && cd firmware-$FW
curl -LO https://ereaderfiles.kobo.com/firmwares/kobo12/Oct2026/kobo-update-$FW.zip
unzip -o kobo-update-$FW.zip KoboRoot.tgz
tar -C extracted/root -xzf KoboRoot.tgz \
    ./usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/libQtWebKit.so.4.6.2 \
    ./usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/libQtWebKitWidgets.so.4.6.2
cd ../..
```

If you use a different firmware version, rename the folder to match or update the two
symlinks in `lib/`.

## 4. Build

```sh
cd kobo-webbrowser.koplugin
./build.sh
```

This pulls `ghcr.io/pgaskin/nickeltc:1.0` on the first run and produces
`build/kobo-browser` and `build/kb-escape` (both stripped).

On SELinux systems (Fedora), `build.sh` mounts with `--security-opt label=disable`. Don't
switch it to `:Z`: that relabels your folders.

## 5. Package

```sh
./package.sh
```

This writes `dist/kobo-webbrowser.koplugin/` (ready to copy to
`.adds/koreader/plugins/`) and `dist/kobo-webbrowser.koplugin-<version>.zip`. The version
comes from `kobo-webbrowser.koplugin/_meta.lua`.

## Plugin layout

```
kobo-webbrowser.koplugin/
├── _meta.lua, main.lua    KOReader plugin: menu, dispatcher action, launch/pause/resume,
│                          EPUB handoff and sleep handoff
├── LICENSE, THIRD_PARTY.md
└── bin/
    ├── kobo-browser       the browser (Qt 5.2.1 + QtWebKit from the firmware)
    ├── kb-escape          watchdog: hold power 5 s to kill a frozen browser
    ├── run.sh             sets up library and Qt plugin paths, starts both
    ├── wifi-sleep.sh      Wi-Fi off/on around a suspend (standalone sleep mode)
    └── ca-bundle.crt      CA certificates for HTTPS
```

The browser writes `browser.log`, `launcher.log`, `epub-handoff.txt` and a `cache/`
folder inside `bin/` at runtime.

## Testing on the device

With KOReader's SSH server running (Tools → More tools → SSH server):

```sh
scp -r dist/kobo-webbrowser.koplugin root@<kobo-ip>:/mnt/onboard/.adds/koreader/plugins/
```

then restart KOReader. To stop a running browser from SSH:
`pkill -f '/kobo-browser( |$)'` (BusyBox `pkill -x` won't match it).

How the browser drives the screen, touch, power and sleep is described in
[docs/INTERNALS.md](docs/INTERNALS.md).
