#!/bin/bash
# usage: dw.sh out.c 0xADDR...
G=/home/claude/tools/ghidra_11.4.2_PUBLIC
OUT=$1; shift
NOTHIS=1 MAXMEM=2500M $G/support/analyzeHeadless /home/claude/gh/proj nwwin -process nwmain.exe -noanalysis -readOnly -scriptPath /home/claude/gh/scripts -postScript Decomp.java "$OUT" "$@" > /home/claude/gh/dw.log 2>&1
python3 /home/claude/gh/clean.py "$OUT" > "${OUT%.c}_c.c"
