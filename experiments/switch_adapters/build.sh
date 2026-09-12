#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
cxx="${CXX:-g++}"
flags=(-std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -Isrc)
"$cxx" "${flags[@]}" experiments/switch_workers/main.cpp -o /tmp/tuntom-switch-adapters
"$cxx" "${flags[@]}" experiments/switch_adapters/load.cpp -o /tmp/tuntom-switch-adapters-load
