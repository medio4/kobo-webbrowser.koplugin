# Web browser for KOReader on Kobo

A touch web browser for Kobo e-readers, shipped as a KOReader plugin
(`kobo-webbrowser.koplugin`). It reuses the Qt 5.2.1 + QtWebKit libraries already in
Kobo's firmware, so the whole plugin is about 1.5 MB (most of it the ad blocking list). Developed on a Kobo Clara BW.

<p align="center">
  <img src="docs/screenshots/reading.png" width="30%" alt="Reading a Wikipedia article">
  <img src="docs/screenshots/menu.png" width="30%" alt="The Menu, grouped into Go to, This page and Settings">
  <img src="docs/screenshots/search.png" width="30%" alt="Search results">
</p>
<p align="center">
  <img src="docs/screenshots/keyboard.png" width="30%" alt="Built-in on-screen keyboard">
  <img src="docs/screenshots/contents.png" width="30%" alt="Contents: jump to a heading">
  <img src="docs/screenshots/bookmarks.png" width="30%" alt="Bookmarks">
</p>
<p align="center">
  <img src="docs/screenshots/phone.png" width="30%" alt="Send to Kobo: scan the QR code to send links from a phone">
</p>

## Features

- **E-ink friendly**: one-row toolbar, flat buttons, page-turn swipes, refreshes limited
  to what changed, a single clean flash when a pop-up opens or closes.
- **Readable text**: a minimum font size and a remembered text size (Menu > Text bigger / smaller).
- **Save pages as EPUB** into your library, cleaned up with Readability. On a page that
  lists the parts of a series, save the whole series as one book with a chapter list.
- **Read in KOReader**: open the article (or a whole series) as a temporary book in
  KOReader; it is removed when you close it, and you can go back to the page.
- **Send to Kobo**: open the page shown by the QR code on your phone and paste a link;
  it opens on the Kobo.
- **Bookmarks, History** (can be turned off) and a home page of your choice, also listed
  in KOReader's menu under Web browser to launch straight onto a page.
- **Ad blocking** with a built-in list, plus your own allowed / blocked sites.
- **Contents** to jump between a page's headings; Back/Fwd; error page with Try again.
- Works with KOReader: its top menu opens with a swipe down over the toolbar, KOReader
  handles sleep and wake, and long-pressing Web browser in its menu opens the browser.

## Install

Download the release zip, unzip it, and copy `kobo-webbrowser.koplugin` into
`.adds/koreader/plugins/` on the Kobo. Restart KOReader, then open it from the menu:
Tools > Web browser (or long-press that entry). See [BUILDING.md](BUILDING.md) to build it.

---

## Internals

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

Install on the Kobo: copy build/kobo-browser, build/kb-escape, run.sh, wifi-sleep.sh and ca-bundle.crt into
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
- KB_KO_SLEEP=1 (applauncher.koplugin sets it, plus KB_AUTOSLEEP_MIN from KOReader's
  auto-suspend setting): power button, sleep cover and auto-sleep don't suspend here.
  The browser writes `sleep<TAB>url<TAB>scrollY<TAB>zoom` to the handoff file and exits;
  the launcher calls UIManager:suspend() so KOReader sleeps with its own screensaver
  (wallpapers) and Wi-Fi handling, and on Resume relaunches the browser with the URL and
  KB_RESTORE_Y / KB_RESTORE_ZOOM (after waiting up to 15 s for Wi-Fi). History is not kept.
- KB_KO_LIVE=1 (with KB_POWER_DEV=none and KB_NO_ESCAPE=1; set by the KOReader plugin):
  KOReader keeps running in the background with its input devices open. The browser
  grabs the touchscreen, so taps never reach KOReader, but KOReader keeps the power button
  and sleep cover. On a power press or cover close the plugin sends the browser SIGTERM;
  with KB_KO_LIVE=1 that writes the same `sleep` handoff line as above before quitting,
  and KOReader then shows its screensaver and suspends. The Exit button and SIGINT quit
  without a sleep line. Auto-sleep still comes from the browser's own idle timer
  (KB_AUTOSLEEP_MIN with KB_KO_SLEEP=1); the plugin keeps KOReader's AutoSuspend quiet
  while the browser runs.
- Testing switches: KB_SLEEP_SUSPEND=0 (sleep screen only, no kernel suspend),
  KB_SLEEP_WIFI=0 (leave Wi-Fi alone), KB_SLEEP_COVER=0, KB_POWER_DEV=/dev/input/eventN|none.
  Log lines start with "power:" / "frontlight:".

Frozen-browser escape (kb-escape.c). run.sh starts a tiny watchdog next to the browser that
reads the power button itself (without grabbing it), outside the Qt event loop:
- Hold the power button for 5 s (KB_ESCAPE_HOLD_SEC) to quit: SIGTERM, then SIGKILL after
  2 s. The dead process drops its touchscreen grab; run.sh does a full flashing refresh and
  returns to KOReader. A short press still means sleep (power.cpp acts on the press, so on
  a browser that still responds, a hold just puts it to sleep as before).
- Optional heartbeat: the browser touches KB_HEARTBEAT_FILE every second; with
  KB_HEARTBEAT_SEC=N set, kb-escape also kills it after N s without a tick (off by default,
  since a heavy page can block the UI thread for a while).
- Hold time and heartbeat use CLOCK_MONOTONIC, so time asleep doesn't count.
  KB_NO_ESCAPE=1 turns it off. Log lines start with "kb-escape:".

KOReader keyboard (textentry.cpp + plugin): when launched by kobo-webbrowser.koplugin with
KB_KBD_FILE/KB_KBD_REPLY set, tapping the address bar, the Keys button or a text field in a
page asks KOReader for the text: the browser writes the request file, stops refreshing and
releases the touchscreen; the plugin lifts its draw guard, shows an InputDialog over the
frozen page and writes the reply; the browser then takes the screen back (full flash) and
fills the field (Go = load / Enter in the field). Falls back to the built-in keyboard when
nobody picks the request up within 2.5 s. It is used only when the plugin sets KB_NATIVE_KBD=1 (its experimental "Use KOReader keyboard" option; KOReader must be restarted after a plugin update). KB_NO_NATIVE_KBD=1 forces the built-in keyboard.

KOReader menus over the browser (same hand-off, KB_MENU_FILE/KB_MENU_REPLY, always set by
the plugin): swipe down at least 120 px starting on the toolbar or the status line (anywhere above the page). The browser freezes and
releases the touchscreen; the plugin copies the frozen page into its cover, lifts the draw
guard and opens KOReader's top menu (file manager or reader menu). Menus, their dialogs and
settings work normally over the page. Once nothing is left open above the page, the reply
"back" hands the screen back: same page, no reload, one full flash. If KOReader moves on
instead (the file-browser/home icon, a book opened from History, "Close web browser" in
Tools > Web browser), the reply "quit" makes the browser exit without drawing, and KOReader
stays on its new screen. KOReader exiting or restarting from the menu kills the browser.
