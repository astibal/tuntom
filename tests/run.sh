#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tuntom-tests.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

echo "Checking self-contained headers"
for header in "${tests_dir}/../src/"*.hpp; do
    printf '#include "%s"\n' "$header" | \
        "${CXX:-g++}" -x c++ -std=c++17 -Wall -Wextra -pedantic -fsyntax-only -
done

echo "Building application"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/main.cpp" -o "${build_dir}/tuntom"

for name in replay_test mac_test aead_test encrypted_session_test session_test; do
    echo "Building ${name}"
    "${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/${name}.cpp" -o "${build_dir}/${name}"
    "${build_dir}/${name}"
done

if command -v tshark >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    python3 "${tests_dir}/dissector_test.py"
else
    echo "SKIP: Wireshark dissector tests require tshark and python3"
fi
