#!/bin/bash
# Builds build/OwlEars.exe - a single static exe, nothing to install.
#   Linux:   MinGW-w64 (Debian / Ubuntu: apt install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64)
#   Windows: MSYS2 (UCRT64 shell: pacman -S mingw-w64-ucrt-x86_64-gcc), then: ./build.sh
set -e
cd "$(dirname "$0")"
if command -v x86_64-w64-mingw32-g++-posix >/dev/null; then CXX=x86_64-w64-mingw32-g++-posix; CC=x86_64-w64-mingw32-gcc; RC=x86_64-w64-mingw32-windres
elif command -v x86_64-w64-mingw32-g++ >/dev/null; then CXX=x86_64-w64-mingw32-g++; CC=x86_64-w64-mingw32-gcc; RC=x86_64-w64-mingw32-windres
else CXX=g++; CC=gcc; RC=windres; fi
mkdir -p build
$CC -c -O2 -w src/ma_owl.c -o build/ma_owl.o
$RC src/owlears.rc -O coff -o build/rc.o
$CXX -O2 -std=c++17 -Wall -Wno-unused-function -Wno-misleading-indentation -municode -mwindows \
    src/main_win.cpp build/ma_owl.o build/rc.o -o build/OwlEars.exe \
    -static -lwinhttp -lole32 -lshell32 -lcomdlg32 -lgdi32 -luser32 -luuid
echo "built: build/OwlEars.exe"
