#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tuntom-tests.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

for name in replay_test mac_test; do
    echo "Building ${name}"
    "${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/${name}.cpp" -o "${build_dir}/${name}"
    "${build_dir}/${name}"
done
