#!/bin/sh
# Minimal Qt test (no WebKit) using the same environment as run.sh
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH=/usr/local/Kobo:/usr/local/Qt-5.2.1-arm/lib:/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib
P=/tmp/kobo-browser-plugins
rm -rf "$P"; mkdir -p "$P/imageformats"
ln -s /usr/local/Kobo/platforms "$P/platforms"
ln -s /usr/local/Kobo/generic "$P/generic"
export QT_PLUGIN_PATH="$P" QT_QPA_PLATFORM_PLUGIN_PATH="$P"
export QT_QPA_FONTDIR=/usr/local/Trolltech/QtEmbedded-4.6.2-arm/lib/fonts
export KB_TOUCH_DEV="${KB_TOUCH_DEV:-/dev/input/event1}"
export KB_SECS="${KB_SECS:-20}"
cd "$DIR"
echo "=== $(date) qtest" >> "$DIR/qtest.log"
"$DIR/qtest" -platform kobo "$@" >> "$DIR/qtest.log" 2>&1
echo "=== exit $?" >> "$DIR/qtest.log"
