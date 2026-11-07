#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if pgrep -x "ytmusic-popup" > /dev/null; then
    pkill -x "ytmusic-popup"
else
    "$DIR/ytmusic_ctl.py" start-daemon >/dev/null 2>&1 &
    "$DIR/ytmusic-popup" &
fi
