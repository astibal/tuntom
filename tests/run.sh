#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tuntom-tests.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

echo "Checking self-contained headers"
while IFS= read -r relative_header; do
    header="${tests_dir}/../${relative_header}"
    printf '#include "%s"\n' "$header" | \
        "${CXX:-g++}" -x c++ -std=c++17 -Wall -Wextra -pedantic -fsyntax-only -
done < <(cd "${tests_dir}/.." && rg --files src -g '*.hpp' | sort)

echo "Building application"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/main.cpp" -o "${build_dir}/tuntom"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/switch/main.cpp" -o "${build_dir}/tuntom-switch"

if command -v python3 >/dev/null 2>&1; then
    python3 "${tests_dir}/switch_test.py" "${build_dir}/tuntom-switch"
    python3 "${tests_dir}/switch_tunnel_test.py" \
        "${build_dir}/tuntom" "${build_dir}/tuntom-switch"
else
    echo "SKIP: tuntom-switch process test requires python3"
fi

for name in compact_protocol_test switch_protocol_test switch_options_test stats_control_test session_stats_test x25519_test akdf_test pfs_session_test replay_test mac_test aead_test encrypted_session_test session_test; do
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

if command -v python3 >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    python3 "${tests_dir}/mk_stop_test.py"
else
    echo "SKIP: process cleanup tests require python3 and cc"
fi
