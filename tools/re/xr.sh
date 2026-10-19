#!/bin/bash
G=/home/claude/tools/ghidra_11.4.2_PUBLIC
OUT=$1; shift
MAXMEM=1500M $G/support/analyzeHeadless /home/claude/gh/proj nwlin -process nwmain-linux -noanalysis -readOnly -scriptPath /home/claude/gh/scripts -postScript Xrefs.java "$OUT" "$@" > /home/claude/gh/xr.log 2>&1
