#!/bin/sh
# Rebuild adblock-hosts.txt from StevenBlack's unified hosts list (ads + malware, MIT).
# Subdomains whose parent domain is already listed are dropped, since the browser
# matches parent domains anyway (about half the entries).
#   tools/update-adblock-hosts.sh [hosts-file-or-url]
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts}"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
case "$SRC" in http*) curl -fsSL -o "$TMP" "$SRC" ;; *) cp "$SRC" "$TMP" ;; esac
DATE="$(sed -n 's/^# Date: //p' "$TMP" | head -n 1)"
{
    echo "# Ad and tracker hosts for kobo-browser (subdomains of listed domains are blocked too)."
    echo "# Source: StevenBlack/hosts unified list (MIT), ${DATE:-unknown date}; rebuilt by tools/update-adblock-hosts.sh"
    awk '$1 == "0.0.0.0" && $2 != "0.0.0.0" { print tolower($2) }' "$TMP" | sort -u | python3 -I -c '
import sys
hosts = set(sys.stdin.read().split())
def covered(h):
    p = h.split(".")
    return any(".".join(p[i:]) in hosts for i in range(1, len(p) - 1))
print("\n".join(sorted(h for h in hosts if not covered(h))))'
} > "$HERE/adblock-hosts.txt"
echo "$(grep -vc '^#' "$HERE/adblock-hosts.txt") hosts -> $HERE/adblock-hosts.txt"
