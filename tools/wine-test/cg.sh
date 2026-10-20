#!/bin/bash
# new-character flow: Prelude -> character list -> New Character (pad UI on the generation panel)
cd /home/claude/test; . ./lib.sh
cp /home/claude/nwnpad/build/version.dll /home/claude/nwn/bin/win32/
echo "0 0 0 0 0 0 0" > /tmp/pad.txt; rm -f /tmp/pad.txt.cmd
./run.sh 1800 +LoadNewModule Prelude
for i in $(seq 60); do sleep 1; grep -q "font atlas" $LOG 2>/dev/null && break; done; sleep 12
DISPLAY=:99 xdotool mousemove 356 591 click 1; sleep 4
