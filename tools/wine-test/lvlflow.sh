#!/bin/bash
# level-up flow with the pad: class (A), abilities (Y, Start), skills (Y, Start), spells (Y, Start), familiar (Start), summary
cd /home/claude/test; . ./lib.sh
./lvl.sh >/dev/null 2>&1
s() { ./tap.sh $1 0.3; sleep ${2:-2}; }
s 1000 2.5; ./shot.sh /tmp/f1.png
s 8000; s 10 2.5; ./shot.sh /tmp/f2.png
s 8000; s 10 2.5; ./shot.sh /tmp/f3.png
s 8000; s 10 2.5; ./shot.sh /tmp/f4.png
s 10 2.5; ./shot.sh /tmp/f5.png
grep "charpanel\|popup" $LOG | tail -8
