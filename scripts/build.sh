#!/bin/sh
set -e
make -C third_party/mupdf -j"$(nproc)" build=release libs
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
ninja -C build
echo "OK: build/glance"
