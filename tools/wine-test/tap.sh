#!/bin/bash
# tap.sh BUTTONS_HEX [hold_seconds]
echo "$1 0 0 0 0 0 0" > /tmp/pad.txt; sleep ${2:-0.5}; echo "0 0 0 0 0 0 0" > /tmp/pad.txt
