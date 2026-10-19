#!/bin/bash
# usage: dl.sh out.c regex...
G=/home/claude/tools/ghidra_11.4.2_PUBLIC
OUT=$1; shift
MAXMEM=1500M $G/support/analyzeHeadless /home/claude/gh/proj nwlin -process nwmain-linux -noanalysis -readOnly -scriptPath /home/claude/gh/scripts -postScript Decomp.java "$OUT" "$@" > /home/claude/gh/dl.log 2>&1
grep -E "ERROR|Exception" /home/claude/gh/dl.log | head -5
