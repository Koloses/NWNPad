#!/bin/bash
# end-to-end check of the main features in the Prelude module
cd /home/claude/test; . ./lib.sh
fail=0; ok() { echo "OK   $1"; }; bad() { echo "FAIL $1"; fail=1; }
chk() { if pressuntil "$1" "$2" "${3:-0.3}" >/dev/null; then ok "$4"; else bad "$4"; fi; }
start "Prelude" 5
chk 10 "hub: open" 0.2 "Start opens the hub"
./shot.sh /tmp/r_hub.png
chk 1000 "unequip" 0.3 "A unequips main hand"
sleep 1.5
./tap.sh 1000 0.3; sleep 0.8     # A on the empty slot -> backpack filtered
chk 1000 "equip 8" 0.3 "A equips from the filtered backpack"
sleep 1; ./shot.sh /tmp/r_hub2.png
chk 2000 "hub: close" 0.3 "B closes the hub"
T "Chest"; chk 1000 "loot: container" 0.2 "A opens the chest (loot window)"
sleep 1; ./shot.sh /tmp/r_loot.png
chk 1000 "take " 0.3 "A takes an item"
chk 2000 "loot: closed" 0.3 "B closes the loot window"
chk 10 "hub: open" 0.2 "Start opens the hub again"
./tap.sh 200 0.25; sleep 0.4; ./tap.sh 200 0.25; sleep 0.6
chk 1000 "quickbar edit" 0.3 "Quickbar tab: A opens the assign wheel"
sleep 1; ./shot.sh /tmp/r_qb.png
./tap.sh 2000 0.3; sleep 1; ./tap.sh 10 0.3; sleep 1
echo probe > /tmp/pad.txt.cmd; sleep 1
p0=$(grep "probe player" $LOG | tail -1)
./pad.sh 0 0 1; sleep 2; ./pad.sh 0; sleep 1
echo probe > /tmp/pad.txt.cmd; sleep 1
p1=$(grep "probe player" $LOG | tail -1)
[ "${p0#*player}" != "${p1#*player}" ] && ok "left stick moves the hero" || bad "left stick moves the hero"
# pad unplugged -> game HUD back after 5 s
echo "" > /tmp/pad.txt; sleep 7; ./shot.sh /tmp/r_nopad.png
echo "0 0 0 0 0 0 0" > /tmp/pad.txt
grep -iE "signature mismatch|FAILED|exception" $LOG && bad "log errors" || ok "no errors in log"
exit $fail
