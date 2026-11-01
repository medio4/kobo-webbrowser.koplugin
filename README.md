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
- power.cpp / power.h   — power button, sleep cover, auto-sleep and frontlight (see below)
- wifi-sleep.sh         — turns Wi-Fi off before a suspend and reconnects after, with
                          KOReader's enable/disable-wifi.sh and its saved networks
- epub.cpp / epub.h     — "Save" button: runs Mozilla's Readability.js inside the page
                          (js/Readability.js, transpiled to ES5 for the old JavaScriptCore,
                          plus js/prelude.js polyfills), fetches the article's images
                          (downscaled to 1200 px, JPEG/PNG), and writes a stored-zip EPUB to
                          $KB_LIBRARY_DIR (run.sh: KOReader home folder + /Web, i.e.
                          /mnt/onboard/Books/Web). Status line shows progress and the result.

Install on the Kobo: copy build/kobo-browser, run.sh, wifi-sleep.sh and ca-bundle.crt into
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

Sleep and frontlight (power.cpp). KOReader closes its input devices while the browser
runs, so the browser answers the power button itself, the way KOReader's Kobo code does:
- Power button press (or closing a sleep cover): "Sleeping" screen with a full refresh,
  frontlight off, Wi-Fi off (wifi-sleep.sh), then `1 > /sys/power/state-extended`, 2 s,
  `mem > /sys/power/state`. Another press (or opening the cover) wakes it: frontlight
  back, full refresh, Wi-Fi reconnects in the background (status line says how it went).
  A press during the 3 s before the suspend cancels it.
- Plugged in: the MTK kernel hangs if it suspends while charging, so it only shows the
  sleep screen and retries every minute until unplugged.
- Woke up without a power press (RTC, charger): back to sleep after 15 s, up to 20 times.
- Auto-sleep after KB_AUTOSLEEP_MIN minutes without a tap (default 30, 0 = never).
- Frontlight: a vertical swipe that starts within KB_LIGHT_EDGE px (48) of the left edge
  sets brightness (up = brighter), of the right edge sets warmth. Horizontal swipes from
  the edges are still history back/forward. sysfs: lm3630a_led/brightness (0..100, else mxc_msp430.0) and
  lm3630a_led/color (0..max_color, inverted), override with KB_FL_BRIGHTNESS / KB_FL_MIXER.
- Testing switches: KB_SLEEP_SUSPEND=0 (sleep screen only, no kernel suspend),
  KB_SLEEP_WIFI=0 (leave Wi-Fi alone), KB_SLEEP_COVER=0, KB_POWER_DEV=/dev/input/eventN|none.
  Log lines start with "power:" / "frontlight:".
