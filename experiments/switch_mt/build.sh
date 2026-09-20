#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
cxx="${CXX:-g++}"
flags=(-std=c++17 -pthread -O3 -march=native -mtune=native -Wall -Wextra -Isrc)
"$cxx" "${flags[@]}" src/switch/main.cpp -o /tmp/tuntom-switch-reference
"$cxx" "${flags[@]}" experiments/switch_mt/main.cpp -o /tmp/tuntom-switch-mt
"$cxx" "${flags[@]}" experiments/switch_mt/load.cpp -o /tmp/tuntom-switch-mt-load
"$cxx" -std=c++17 -pthread -O2 -Wall -Wextra experiments/switch_mt/queues_test.cpp -o /tmp/tuntom-switch-mt-queues-test
"$cxx" -std=c++17 -pthread -O2 src/main.cpp -o /tmp/tuntom-mt-integration
"$cxx" -std=c++17 -pthread -O2 src/control/main.cpp -o /tmp/tuntom-mt-ctl
