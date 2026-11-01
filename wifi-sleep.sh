#!/bin/sh
# Wi-Fi around a suspend, for kobo-browser (power.cpp). Uses KOReader's own scripts.
#   wifi-sleep.sh suspend  -> turns Wi-Fi off; last line "wifi-was-on" if it was on
#   wifi-sleep.sh resume   -> turns it back on, reconnects; last line "connected ..." or a reason
# KOReader starts wpa_supplicant without saved networks and adds the network itself from
# its settings/network.lua, so on resume we do the same for the network we were on.
KO="${KOREADER_DIR:-/mnt/onboard/.adds/koreader}"
IF="${INTERFACE:-wlan0}"
STATE=/tmp/kobo-browser-wifi.ssid
LOG=/tmp/kobo-browser-wifi.log
cd "$KO" || { echo "no koreader dir"; exit 1; }

wpa_state() { wpa_cli -i "$IF" status 2>/dev/null | sed -n 's/^wpa_state=//p'; }

case "$1" in
suspend)
    rm -f "$STATE"
    if wpa_cli -i "$IF" status >/dev/null 2>&1; then
        wpa_cli -i "$IF" status 2>/dev/null | sed -n 's/^ssid=//p' > "$STATE"
        ./disable-wifi.sh >> "$LOG" 2>&1
        echo "wifi-was-on"
    else
        echo "wifi-was-off"
    fi
    ;;
resume)
    [ -f "$STATE" ] || { echo "nothing to do"; exit 0; }
    SSID="$(cat "$STATE")"
    rm -f "$STATE"
    ./enable-wifi.sh >> "$LOG" 2>&1
    # Wait for wpa_supplicant's control socket.
    i=0; while [ $i -lt 20 ] && ! wpa_cli -i "$IF" status >/dev/null 2>&1; do usleep 500000; i=$((i + 1)); done
    # It may connect on its own (networks saved in wpa_supplicant.conf).
    i=0; while [ $i -lt 8 ] && [ "$(wpa_state)" != "COMPLETED" ]; do usleep 500000; i=$((i + 1)); done
    if [ "$(wpa_state)" != "COMPLETED" ] && [ -n "$SSID" ]; then
        # Add the network the way KOReader does, from settings/network.lua.
        PSK="$(awk -v ssid="$SSID" '
            index($0, "[\"" ssid "\"] = {") { inb = 1; next }
            inb && /^    },?$/ { exit }
            inb && /\["psk"\]/ { sub(/.*\["psk"\] = "/, ""); sub(/",?$/, ""); print; exit }
        ' settings/network.lua 2>/dev/null)"
        ID="$(wpa_cli -i "$IF" add_network | tail -n 1)"
        wpa_cli -i "$IF" set_network "$ID" ssid "\"$SSID\"" >/dev/null
        if [ -n "$PSK" ]; then
            wpa_cli -i "$IF" set_network "$ID" psk "$PSK" >/dev/null
        else
            wpa_cli -i "$IF" set_network "$ID" key_mgmt NONE >/dev/null
        fi
        wpa_cli -i "$IF" select_network "$ID" >/dev/null
        i=0; while [ $i -lt 30 ] && [ "$(wpa_state)" != "COMPLETED" ]; do usleep 500000; i=$((i + 1)); done
    fi
    if [ "$(wpa_state)" != "COMPLETED" ]; then
        echo "not associated ($(wpa_state))"
        exit 1
    fi
    ./obtain-ip.sh >> "$LOG" 2>&1
    echo "connected $SSID"
    ;;
*)
    echo "usage: $0 suspend|resume"; exit 2 ;;
esac
