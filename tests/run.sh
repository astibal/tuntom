#!/usr/bin/env bash
set -euo pipefail

tests_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tuntom-tests.XXXXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

echo "Checking self-contained headers"
while IFS= read -r relative_header; do
    header="${tests_dir}/../${relative_header}"
    printf '#include "%s"\n' "$header" | \
        "${CXX:-g++}" -x c++ -std=c++17 -pthread -Wall -Wextra -pedantic -fsyntax-only -
done < <(cd "${tests_dir}/.." && rg --files src -g '*.hpp' | sort)

echo "Building application"
"${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/main.cpp" -o "${build_dir}/tuntom"
"${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/switch/main.cpp" -o "${build_dir}/tuntom-switch"
"${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/adapter/main.cpp" -o "${build_dir}/tuntom-switch-adapter"
"${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
    "${tests_dir}/../src/control/main.cpp" -o "${build_dir}/tuntomctl"

if command -v python3 >/dev/null 2>&1; then
    "${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/../src/adapter/main.cpp" "${tests_dir}/adapter_tun_fixture.cpp" \
        -Wl,--wrap=open -Wl,--wrap=ioctl -o "${build_dir}/adapter_reconnect_fixture"
    "${CXX:-g++}" -std=c++17 -pthread -shared -fPIC -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/runtime_faults.cpp" -ldl -o "${build_dir}/runtime_faults.so"
    "${CXX:-g++}" -std=c++17 -pthread -shared -fPIC -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/logging_faults.cpp" -ldl -o "${build_dir}/logging_faults.so"
    "${CXX:-g++}" -std=c++17 -pthread -shared -fPIC -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/accept_faults.cpp" -ldl -o "${build_dir}/accept_faults.so"
    python3 "${tests_dir}/switch_capacity_test.py" \
        "${build_dir}/tuntom-switch" "${build_dir}/accept_faults.so"
    python3 "${tests_dir}/logging_recovery_test.py" \
        "${build_dir}/tuntom" "${build_dir}/tuntom-switch" \
        "${build_dir}/adapter_reconnect_fixture" "${build_dir}/logging_faults.so"
    python3 "${tests_dir}/runtime_recovery_test.py" \
        "${build_dir}/tuntom" "${build_dir}/tuntom-switch" \
        "${build_dir}/adapter_reconnect_fixture" "${build_dir}/runtime_faults.so"
    for script in mk_tunnel.sh mk_switch.sh mk_adapter.sh tools/mk_local.sh; do
        bash -n "${tests_dir}/../${script}"
    done
    python3 "${tests_dir}/mk_local_test.py" "${build_dir}/tuntom-switch" \
        "${build_dir}/adapter_reconnect_fixture" "${build_dir}/tuntomctl"
    python3 "${tests_dir}/switch_reconnect_test.py" \
        "${build_dir}/tuntom" "${build_dir}/adapter_reconnect_fixture" "${build_dir}/tuntom-switch"
    python3 "${tests_dir}/switch_test.py" \
        "${build_dir}/tuntom-switch" "${build_dir}/tuntomctl"
    python3 "${tests_dir}/switch_tunnel_test.py" \
        "${build_dir}/tuntom" "${build_dir}/tuntom-switch" "${build_dir}/tuntomctl"
    python3 "${tests_dir}/stats_socket_test.py" \
        "${build_dir}/tuntom" "${build_dir}/tuntom-switch" "${build_dir}/tuntomctl"
else
    echo "SKIP: tuntom-switch process test requires python3"
fi

for name in compact_protocol_test switch_protocol_test switch_options_test switch_client_test switch_admission_test runtime_state_test logging_test exit_adapter_test session_stats_test session_latency_test x25519_test akdf_test pfs_session_test replay_test mac_test aead_test encrypted_session_test session_test adaptive_polling_test reassembly_test; do
    echo "Building ${name}"
    link_flags=()
    if [[ "$name" == switch_client_test ]]; then
        link_flags=(-Wl,--wrap=connect -Wl,--wrap=send -Wl,--wrap=getsockopt)
    elif [[ "$name" == runtime_state_test ]]; then
        link_flags=(-Wl,--wrap=getrandom)
    elif [[ "$name" == logging_test ]]; then
        link_flags=(-Wl,--wrap=write -Wl,--wrap=pthread_create)
    fi
    "${CXX:-g++}" -std=c++17 -pthread -O2 -Wall -Wextra -Wconversion -pedantic \
        "${tests_dir}/${name}.cpp" "${link_flags[@]}" -o "${build_dir}/${name}"
    "${build_dir}/${name}"
done

if command -v tshark >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    python3 "${tests_dir}/dissector_test.py"
else
    echo "SKIP: Wireshark dissector tests require tshark and python3"
fi

if command -v python3 >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    python3 "${tests_dir}/mk_stop_test.py"
    python3 "${tests_dir}/mk_switch_args_test.py"
else
    echo "SKIP: process cleanup tests require python3 and cc"
fi
