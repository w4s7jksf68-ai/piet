#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
source_root=$(pwd -P)
# Keep the builder's local path out of public binaries (including __FILE__).
path_map="-ffile-prefix-map=$source_root=."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 "-DCMAKE_CXX_FLAGS=$path_map"
cmake --build build --parallel "${BUILD_JOBS:-2}"
cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 "-DCMAKE_CXX_FLAGS=$path_map"
cmake --build piet/native/build --parallel "${BUILD_JOBS:-2}"
ctest --test-dir build --output-on-failure
python3 tests/smoke.py
