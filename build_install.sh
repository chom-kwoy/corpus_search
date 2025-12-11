#!/usr/bin/env bash
set -e
cmake -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build --parallel
sudo cmake --install build
