#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "${BUILD_JOBS:-2}"
cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release
cmake --build piet/native/build --parallel "${BUILD_JOBS:-2}"
ctest --test-dir build --output-on-failure
python3 tests/smoke.py
