#!/bin/bash
# cg.sh then: gender female, race elf, open portrait
cd /home/claude/test; . ./lib.sh
./cg.sh
s() { ./tap.sh $1 0.25; sleep ${2:-1.5}; }
s 1000 3; s 2; s 1000 2.5      # gender: female, OK
s 1000 3; for i in 1 2 3 4 5; do s 1 0.5; done; s 1000 2.5   # race: elf
s 1000 3; s 8 1; s 2 1; s 1000 2.5      # portrait: 9th, OK
s 1000 3; for i in $(seq 10); do s 2 0.4; done; s 1000 2.5   # class: wizard
s 1000 3; s 1 0.4; s 1 0.4; s 1 0.4; s 1000 2.5   # alignment: true neutral
s 1000 3; s 8000 1; s 10 2.5          # abilities: recommended, OK
s 1000 3; s 4000 3; s 8000 1; s 10 3; s 1000 1.5; s 10 3; s 1000 3; s 8000 1; s 10 3; s 10 3   # packages: configure, skills, feat, school, spells, familiar
