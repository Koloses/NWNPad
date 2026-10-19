#!/bin/bash
# run.sh <seconds> [args...]  : launches nwmain under wine on :99 (kills old)
export WINEPREFIX=/root/wineprefix WINEDEBUG=-all DISPLAY=:99 WINEDLLOVERRIDES="version=n,b"
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &) ; sleep 1
wineserver -k 2>/dev/null; sleep 1
T=$1; shift
cd /home/claude/nwn/bin/win32
nohup timeout $T wine nwmain.exe "$@" > /tmp/nwn_run.log 2>&1 &
echo started
