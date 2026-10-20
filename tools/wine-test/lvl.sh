#!/bin/bash
# start Prelude, give XP and open the level-up (class panel)
cd /home/claude/test; . ./lib.sh
start "${1:-Prelude}" 5
console "DebugMode 1"; console "GiveXP 20000"
W=$(DISPLAY=:99 xdotool search --name "Neverwinter" | head -1); DISPLAY=:99 xdotool key --window $W grave 2>/dev/null; sleep 1
./tap.sh 8000 0.3; sleep 2
echo levelup > /tmp/pad.txt.cmd; sleep 4
