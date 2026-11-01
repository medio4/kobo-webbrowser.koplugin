# kobo-browser

Minimal touch web browser for Kobo e-readers that reuses the Qt 5.2.1 + QtWebKit
libraries already inside Kobo's firmware (Nickel). No Nickel process needed; it is
meant to be launched from KOReader via applauncher.koplugin while KOReader waits.

Files
- main.cpp / browser.h  — the app (QWebView with swipe scrolling, on-screen keyboard)
- browser.pro           — qmake project (links the firmware's libQt5WebKit*, see lib/)
- build.sh              — cross-compile with NickelTC (../downloads/nickeltc)
- run.sh                — device launcher: sets LD_LIBRARY_PATH / Qt plugin paths,
                          runs `kobo-browser -platform kobo`
- ca-bundle.crt         — CA certificates for HTTPS

Install on the Kobo: copy build/kobo-browser, run.sh and ca-bundle.crt into
/mnt/onboard/.adds/kobo-browser/ (that is .adds/kobo-browser on the USB drive).
Log: /mnt/onboard/.adds/kobo-browser/browser.log
