#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
"${CXX:-g++}" -std=c++17 -pthread -O3 -march=native -mtune=native \
    -Wall -Wextra -Wpedantic experiments/ascon_workers/main.cpp \
    -o "${1:-/tmp/tuntom-ascon-workers}"
