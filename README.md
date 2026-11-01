# kobo-browser

Minimal touch web browser for Kobo e-readers that reuses the Qt 5.2.1 + QtWebKit
libraries already inside Kobo's firmware (Nickel). No Nickel process needed; it is
meant to be launched from KOReader via applauncher.koplugin while KOReader waits.

Files
- main.cpp / browser.h  — the app (QWebView with swipe scrolling, flat KOReader-style
                          buttons, on-screen keyboard that floats over the page)
- hwtcon.cpp/h          — e-ink refresh via the MTK hwtcon ioctl (no fbink spawn); falls
                          back to fbink if the ioctl fails. KB_NO_HWTCON=1 forces fbink.
- fbwriter.cpp/h        — copies painted pixels to /dev/fb0; reports whether anything changed
                          so unchanged repaints (button press/release) skip the refresh
- browser.pro           — qmake project (links the firmware's libQt5WebKit*, see lib/)
- build.sh              — cross-compile with NickelTC (../downloads/nickeltc)
- run.sh                — device launcher: sets LD_LIBRARY_PATH / Qt plugin paths,
                          runs `kobo-browser -platform kobo`
- ca-bundle.crt         — CA certificates for HTTPS
- epub.cpp / epub.h     — "Save" button: runs Mozilla's Readability.js inside the page
                          (js/Readability.js, transpiled to ES5 for the old JavaScriptCore,
                          plus js/prelude.js polyfills), fetches the article's images
                          (downscaled to 1200 px, JPEG/PNG), and writes a stored-zip EPUB to
                          $KB_LIBRARY_DIR (run.sh: KOReader home folder + /Web, i.e.
                          /mnt/onboard/Books/Web). Status line shows progress and the result.

Install on the Kobo: copy build/kobo-browser, run.sh and ca-bundle.crt into
/mnt/onboard/.adds/kobo-browser/ (that is .adds/kobo-browser on the USB drive).
Log: /mnt/onboard/.adds/kobo-browser/browser.log

Refresh policy (eink.cpp): widgets with the Qt property `kb_fast` (toolbar, keyboard)
refresh with the fast DU waveform; everything else uses the driver's AUTO waveform;
full refreshes use GC16. The text cursor does not blink. Keys show no pressed state
unless KB_KEY_FLASH=1 is set (that costs an extra refresh per tap).
Rebuilding js/Readability.js: take mozilla/readability (tag 0.4.4, Apache-2.0, see
js/LICENSE-Readability.md), transpile with Babel preset-env (targets ie 11, loose) and change
`_createForOfIteratorHelperLoose(nodeList)` to `_createForOfIteratorHelperLoose(nodeList, true)`
so NodeLists iterate without Symbol.iterator.
