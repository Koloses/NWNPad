#!/bin/sh
# Linux build of the offline quest marker audit
cd "$(dirname "$0")" && g++ -O2 -std=c++17 -o qaudit qaudit.cpp ../../src/questplan.cpp ../../src/questdata.cpp ../../src/ncsflow.cpp
