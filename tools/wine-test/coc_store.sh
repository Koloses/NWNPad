#!/bin/bash
cd /home/claude/test; . ./lib.sh
start "Contest Of Champions 0492"
T "Enforcer A"; pressuntil 1000 "dialog replies" 0.2
pressuntil 2 "dialog highlight 1" 0.3
pressuntil 1000 "dialog replies 2" 0.3          # -> level correction question
pressuntil 1000 "dialog replies 5" 0.3          # yes
pressuntil 2000 "dialog closed" 0.3
T "Portal"; pressuntil 1000 "area changed" 0.2; sleep 8
T "Warrior Store"; pressuntil 1000 "dialog replies" 0.2
pressuntil 1000 "store: open" 0.3
