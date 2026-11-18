#!/bin/sh
# Ubuntu 22.04+ / Debian 12+, x86-64 or arm64. Dependencies:
# sudo apt install build-essential cmake ninja-build pkg-config python3 \
#   libopenblas-dev liblapacke-dev libwebkit2gtk-4.1-dev
# BUILD_JOBS defaults to 2. Run tests/ui_smoke.py under a desktop or Xvfb too.
set -eu
cd "$(dirname "$0")/.."
test "$(uname -s)" = Linux || { echo 'Run this script on Linux.' >&2; exit 1; }
source_root=$(pwd -P)
python3 tools/lint_source_layout.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCECON_X86_ARCH=x86-64-v2 "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$source_root=."
cmake --build build --parallel "${BUILD_JOBS:-2}"
cmake -S piet/native -B piet/native/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$source_root=."
cmake --build piet/native/build --parallel "${BUILD_JOBS:-2}"
ctest --test-dir build --output-on-failure --timeout 300
python3 tests/public_smoke.py
