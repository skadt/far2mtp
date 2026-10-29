#!/bin/sh
# Сборка MinGW-w64 (Linux или MSYS2): x64
x86_64-w64-mingw32-g++ -std=c++11 -O2 -D_export= -DUNICODE -D_UNICODE -I. -shared \
  -o FarMTP.dll FarMTP.cpp FarMTP.def -static -static-libgcc -static-libstdc++ \
  -lole32 -loleaut32 -luuid -s
