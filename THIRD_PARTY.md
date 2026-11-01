# Third-party components

## Included in the release zip

| Component | Where | License |
|---|---|---|
| Mozilla Readability 0.4.4 (transpiled to ES5) | compiled into `bin/kobo-browser` (Qt resource `js/Readability.js`) | Apache-2.0, see [js/LICENSE-Readability.md](js/LICENSE-Readability.md) |
| CA certificate bundle (Mozilla root store) | `bin/ca-bundle.crt` | MPL-2.0 |

Apache-2.0 code may be combined with GPL-3.0 code. Everything else in the zip
(`kobo-browser`, `kb-escape`, the shell scripts and the Lua plugin) is this project's own
code under GPL-3.0-or-later.

## Used on the device, not included

These are loaded at runtime from the Kobo's own firmware or from KOReader. The plugin
does not ship or modify them.

| Component | Provided by | License |
|---|---|---|
| Qt 5.2.1 (QtCore, QtGui, QtWidgets, QtNetwork) | Kobo firmware, `/usr/local/Qt-5.2.1-arm/lib` | LGPL-2.1 |
| QtWebKit 5.2.1 (libQt5WebKit, libQt5WebKitWidgets) | Kobo firmware | LGPL-2.1 (WebKit: LGPL-2.0 / BSD) |
| Kobo platform plugin `libkobo.so`, fonts | Kobo firmware | Kobo / Rakuten, proprietary |
| OpenSSL 1.0.1 | Kobo firmware | OpenSSL License |
| fbink, fbdepth | KOReader | GPL-3.0 |
| enable-wifi.sh, disable-wifi.sh | KOReader | AGPL-3.0 |

`kobo-browser` links dynamically against the firmware's Qt and QtWebKit libraries, which
the LGPL allows; you can replace those libraries on your device and the program will use
them.

## Used only to build

| Component | Use | License |
|---|---|---|
| [NickelTC](https://github.com/pgaskin/NickelTC) (`ghcr.io/pgaskin/nickeltc:1.0`) | cross-compiler, glibc 2.19 and Qt 5.2.1 sysroot | GPL / LGPL (GCC, glibc, Qt) |
| QtWebKit 5.2.1 public headers (`qtwebkit-opensource-src-5.2.1`) | compiling against QtWebKit | LGPL-2.1 |
| Kobo firmware `libQtWebKit*.so` | link-time stubs only | as above |
