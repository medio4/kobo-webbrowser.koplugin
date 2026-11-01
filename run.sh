#!/bin/sh
# Launch kobo-browser using the Qt/WebKit libraries from Kobo's own firmware.
# Expects Nickel to NOT be running (KOReader has already stopped it).
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/browser.log"
export LD_LIBRARY_PATH=/usr/local/Kobo:/usr/local/Qt-5.2.1-arm/lib:/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib

# The Kobo display plugin picks its backend (MediaTek / NXP / AllWinner) from Nickel's
# PLATFORM / PRODUCT variables. Take them from the running system if present.
if [ -z "$PLATFORM" ] || [ -z "$PRODUCT" ]; then
    for pid in $(pidof reader.lua nickel 2>/dev/null); do
        eval "$(tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | grep -E '^(PLATFORM|PRODUCT)=' | sed 's/^/export /')"
        [ -n "$PLATFORM" ] && break
    done
fi
export PLATFORM="${PLATFORM:-mt8113t-ntx}"
export PRODUCT="${PRODUCT:-spaBW}"

# Display backend (KB_DISPLAY): "linuxfb" (default) = Qt's own framebuffer plugin shipped in
# ./qt/platforms, with e-ink refreshes done through KOReader's fbink; "kobo" = Kobo's own
# platform plugin from the firmware (its own blit path doesn't work outside Nickel, so we copy
# its screen image to the framebuffer ourselves and refresh with fbink).
KB_DISPLAY="${KB_DISPLAY:-kobo}"
FBINK=/mnt/onboard/.adds/koreader/fbink
FBDEPTH=/mnt/onboard/.adds/koreader/fbdepth
if [ "$KB_DISPLAY" = "kobo" ]; then
    P=/tmp/kobo-browser-plugins
    rm -rf "$P"; mkdir -p "$P/imageformats"
    ln -s /usr/local/Kobo/platforms "$P/platforms"
    ln -s /usr/local/Kobo/generic "$P/generic"
    for f in /usr/local/Trolltech/QtEmbedded-4.6.2-arm/plugins/imageformats/*.so /usr/local/Kobo/imageformats/libkimg.so; do
        [ -e "$f" ] && ln -s "$f" "$P/imageformats/$(basename "$f")"
    done
    export QT_PLUGIN_PATH="$P" QT_QPA_PLATFORM_PLUGIN_PATH="$P"
    # e-ink refresh: "grab" = copy the plugin's screen image to /dev/fb0 ourselves + fbink refresh
    export KB_EINK="${KB_EINK:-grab}" KB_FBINK="$FBINK"
    PLATFORM_ARG="${KB_PLATFORM:-kobo}"
else
    P=/tmp/kobo-browser-plugins
    rm -rf "$P"; mkdir -p "$P/imageformats" "$P/platforms"
    cp "$DIR/qt/platforms/libqlinuxfb.so" "$P/platforms/"
    for f in /usr/local/Trolltech/QtEmbedded-4.6.2-arm/plugins/imageformats/*.so /usr/local/Kobo/imageformats/libkimg.so; do
        [ -e "$f" ] && ln -s "$f" "$P/imageformats/$(basename "$f")"
    done
    export QT_PLUGIN_PATH="$P" QT_QPA_PLATFORM_PLUGIN_PATH="$P"
    export KB_EINK=fbink KB_FBINK="$FBINK"
    PLATFORM_ARG="${KB_PLATFORM:-linuxfb:fb=/dev/fb0:nographicsmodeswitch}"
fi
export QT_QPA_FONTDIR=/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/fonts
export QT_QPA_PLATFORM=kobo
# Touch / button input devices (override with KB_TOUCH_DEV / KB_KEYS_DEV)
export KB_TOUCH_DEV="${KB_TOUCH_DEV:-/dev/input/event1}"
export KB_KEYS_DEV="${KB_KEYS_DEV:-}"   # Kobo keys plugin off by default (set to /dev/input/event0 to try)
export KB_HOME="${KB_HOME:-https://lite.duckduckgo.com/lite/}"
# Where "Save" puts EPUBs: KOReader's home folder (settings.reader.lua home_dir) + /Web
if [ -z "$KB_LIBRARY_DIR" ]; then
    KO_HOME="$(sed -n 's/^ *\["home_dir"\] = "\([^"]*\)",*$/\1/p' /mnt/onboard/.adds/koreader/settings.reader.lua 2>/dev/null | head -n 1)"
    [ -d "$KO_HOME" ] || KO_HOME=/mnt/onboard/Books
    [ -d "$KO_HOME" ] || KO_HOME=/mnt/onboard
    KB_LIBRARY_DIR="$KO_HOME/Web"
fi
export KB_LIBRARY_DIR
# Debug plugin loading: KB_DEBUG=1 sh run.sh
[ -n "$KB_DEBUG" ] && export QT_DEBUG_PLUGINS=1

# Keep the log small
[ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 500000 ] && tail -c 100000 "$LOG" > "$LOG.tmp" && mv "$LOG.tmp" "$LOG"
echo "=== $(date) starting kobo-browser $*" >> "$LOG"
cd "$DIR"
ORIG_BPP=""
if [ "$KB_DISPLAY" != "kobo" ] && [ -x "$FBDEPTH" ]; then
    ORIG_BPP="$($FBDEPTH -g 2>/dev/null)"
    [ "$ORIG_BPP" != "32" ] && "$FBDEPTH" -q -d 32 >> "$LOG" 2>&1
fi
"$DIR/kobo-browser" -platform "$PLATFORM_ARG" $KB_EXTRA_ARGS "$@" >> "$LOG" 2>&1
RC=$?
if [ -n "$ORIG_BPP" ] && [ "$ORIG_BPP" != "32" ]; then
    "$FBDEPTH" -q -d "$ORIG_BPP" >> "$LOG" 2>&1
fi
echo "=== exit $RC" >> "$LOG"
exit $RC
