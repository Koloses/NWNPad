#!/bin/bash
# pad.sh BUTTONS_HEX LX LY RX RY LT RT
echo "$1 ${2:-0} ${3:-0} ${4:-0} ${5:-0} ${6:-0} ${7:-0}" > /tmp/pad.txt
