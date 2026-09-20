#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out="${1:-/tmp/tuntom-divert-lab-build}"
mkdir -p -- "$out"
cxx="${CXX:-g++}"
flags=(-std=c++17 -pthread -O2 -g -Wall -Wextra -Wpedantic -Isrc)
"$cxx" "${flags[@]}" experiments/divert_lab/switch.cpp -o "$out/divert-switch"
"$cxx" "${flags[@]}" experiments/divert_lab/adapter.cpp -o "$out/divert-adapter"
"$cxx" "${flags[@]}" experiments/divert_lab/unit.cpp -o "$out/divert-unit"
"$cxx" "${flags[@]}" src/main.cpp -o "$out/tuntom"
"$cxx" "${flags[@]}" experiments/divert_lab/tuntom_userns.cpp -o "$out/tuntom-userns"
"$cxx" "${flags[@]}" src/adapter/main.cpp -o "$out/exit-adapter"
"$cxx" "${flags[@]}" src/control/main.cpp -o "$out/tuntomctl"
"$out/divert-unit"
