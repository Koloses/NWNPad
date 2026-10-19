# helpers for automated tests
LOG=/home/claude/nwn/bin/win32/NWNPad.log
lines() { wc -l < $LOG 2>/dev/null || echo 0; }
T() { echo "target $1" > /tmp/pad.txt.cmd; waitlog "dev: \(no \)*target" 60; }
# waitlog PATTERN TIMEOUT : wait until a log line written after the call matches
waitlog() { local n0=$(lines); local t=0; while [ $t -lt $(( $2 * 5 )) ]; do local n=$(lines); [ $n -lt $n0 ] && n0=0; if tail -n +$((n0+1)) $LOG | grep -q "$1"; then return 0; fi; sleep 0.2; t=$((t+1)); done; echo "timeout waiting for $1"; return 1; }
# pressuntil BTN PATTERN HOLD : press until a new log line matches (3 tries, 10 s each)
pressuntil() { local n0=$(lines); for i in 1 2 3; do echo "$1 0 0 0 0 0 0" > /tmp/pad.txt; sleep ${3:-0.3}; echo "0 0 0 0 0 0 0" > /tmp/pad.txt; local t=0; while [ $t -lt 50 ]; do if tail -n +$((n0+1)) $LOG | grep -q "$2"; then return 0; fi; sleep 0.2; t=$((t+1)); done; done; echo "no '$2' after pressing $1"; return 1; }
# start game and wait until the player is in an area
start() { echo "0 0 0 0 0 0 0" > /tmp/pad.txt; rm -f /tmp/pad.txt.cmd; ./run.sh 1800 +TestNewModule "$1"; sleep 4; waitlog "area changed" 300; sleep ${2:-8}; }
# console CMD : type a console command into the game
console() { local W=$(DISPLAY=:99 xdotool search --name "Neverwinter" | head -1); DISPLAY=:99 xdotool key --window $W grave 2>/dev/null; sleep 1.5; DISPLAY=:99 xdotool type --window $W --delay 150 "$1" 2>/dev/null; sleep 0.5; DISPLAY=:99 xdotool key --window $W Return 2>/dev/null; sleep 1.5; }
console() {
  local W=$(DISPLAY=:99 xdotool search --name "Neverwinter" | head -1)
  DISPLAY=:99 xdotool key --window $W grave 2>/dev/null; sleep 1.5
  local s="$1" i c k
  for (( i=0; i<${#s}; i++ )); do c="${s:$i:1}"
    case "$c" in
      [A-Z]) k="shift+$(echo $c | tr A-Z a-z)";;
      _) k="shift+minus";; " ") k="space";; .) k="period";; -) k="minus";;
      *) k="$c";;
    esac
    DISPLAY=:99 xdotool key --window $W --delay 60 $k 2>/dev/null
  done
  sleep 0.5; DISPLAY=:99 xdotool key --window $W Return 2>/dev/null; sleep 1.5
}
