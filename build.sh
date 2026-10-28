#!/bin/sh
# Cross-compile from Linux with mingw-w64 (apt install g++-mingw-w64-i686)
set -e
mkdir -p build
# -mstackrealign: the game (x87 build) calls our hooks with any stack alignment, and code below them (the game and plugins such as ZMenu) uses movaps on stack locals
i686-w64-mingw32-g++ -std=c++17 -O2 -mstackrealign -Wno-multichar -shared -static -s -o build/GTAIV.ProperGrass.asi src/ProperGrass.cpp
echo "built build/GTAIV.ProperGrass.asi"
