Wine test harness used during development (Linux, Xvfb on :99, game data in /home/claude/nwn).
NWNPad.ini needs `[Developer] InjectFile=Z:\tmp\pad.txt`; the scripts write the virtual pad
state there (`buttons lx ly rx ry lt rt`, XInput button bits in hex) and drive the game.
`regress.sh` runs the Prelude end-to-end check, `coc_store.sh` opens a merchant in
"Contest Of Champions 0492".
