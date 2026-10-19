#!/bin/bash
# usage: pair.sh tag 'regex1' 'regex2' ...  -> dec/<tag>_L.c and dec/<tag>_W.c (matched only)
TAG=$1; shift
./dl.sh /home/claude/gh/dec/${TAG}_L.c "$@"
python3 clean.py dec/${TAG}_L.c > dec/${TAG}_Lc.c
ADDRS=$(python3 q.py "$@" | awk '{for(i=1;i<=NF;i++) if($i ~ /^W=1/) print "0x" substr($i,3)}' | sort -u)
echo "W addrs: $ADDRS"
python3 q.py "$@"
[ -n "$ADDRS" ] && ./dw.sh /home/claude/gh/dec/${TAG}_W.c $ADDRS
