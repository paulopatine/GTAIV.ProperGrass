#!/bin/sh
# Cross-compile from Linux with mingw-w64 (apt install g++-mingw-w64-i686)
set -e
mkdir -p build
i686-w64-mingw32-g++ -std=c++17 -O2 -Wno-multichar -shared -static -s -o build/GTAIV.ProperGrass.asi src/ProperGrass.cpp
echo "built build/GTAIV.ProperGrass.asi"
