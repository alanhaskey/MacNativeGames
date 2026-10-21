#!/bin/sh
# Install or remove maplinkd as a per-user launchd agent (no sudo).
#   tools/maplinkd/install_launchd.sh install
#   tools/maplinkd/install_launchd.sh status
#   tools/maplinkd/install_launchd.sh uninstall
set -eu

LABEL="com.macnativegames.maplinkd"
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT="$HERE/maplinkd.py"
AGENT="$HOME/Library/LaunchAgents/$LABEL.plist"
LOGS="$HOME/Library/Logs/MacNativeGames"
FILE="$HOME/Library/Application Support/MacNativeGames/TOTK/maplink.json"
PYTHON=$(command -v python3 || true)

usage() { sed -n '2,6p' "$0" >&2; exit 2; }

write_plist() {
  [ -n "$PYTHON" ] || { echo "python3 not found in PATH" >&2; exit 1; }
  mkdir -p "$(dirname "$AGENT")" "$LOGS"
  cat > "$AGENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PYTHON</string>
    <string>$SCRIPT</string>
    <string>--file</string>
    <string>$FILE</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$LOGS/maplinkd.log</string>
  <key>StandardErrorPath</key><string>$LOGS/maplinkd.log</string>
</dict>
</plist>
EOF
}

bootout() { launchctl bootout "gui/$(id -u)/$LABEL" >/dev/null 2>&1 || true; }

case "${1:-}" in
  install)
    write_plist
    bootout
    launchctl bootstrap "gui/$(id -u)" "$AGENT"
    launchctl kickstart -k "gui/$(id -u)/$LABEL"
    echo "installed $LABEL -> http://127.0.0.1:17862 (log: $LOGS/maplinkd.log)"
    ;;
  uninstall)
    bootout
    rm -f "$AGENT"
    echo "removed $LABEL"
    ;;
  status)
    launchctl print "gui/$(id -u)/$LABEL" 2>/dev/null | sed -n '1,12p' || echo "$LABEL is not loaded"
    ;;
  *) usage ;;
esac
