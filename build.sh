#!/bin/bash
# Build WINQ-EMU QEMU in MSYS2 UCRT64 environment
# Prerequisites: pacman -S mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkg-config,glib2,pixman,SDL2,libepoxy,virglrenderer}
set -e
mkdir -p build
cd build
../configure \
    --target-list=x86_64-softmmu \
    --prefix=/ucrt64 \
    --enable-whpx \
    --enable-opengl \
    --enable-virglrenderer \
    --disable-docs \
    --disable-plugins \
    2>&1
ninja
