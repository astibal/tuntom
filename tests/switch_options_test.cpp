#include "../src/cli.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tuntom;

void require(bool value, const char* why) {
    if (not value) throw std::runtime_error(why);
}

bool parse_fails(std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    Options options;
    try {
        parse_options(static_cast<int>(argv.size()), argv.data(), 0, options);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    std::vector<std::string> arguments {
        "--switch-socket", "/run/tuntom/a.sock",
        "--switch-port-id", "honeypot-42",
        "--switch-label", "0x1234",
        "--switch-exit-node",
    };
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    Options options;
    parse_options(static_cast<int>(argv.size()), argv.data(), 0, options);
    require(options.switch_socket == "/run/tuntom/a.sock", "socket path lost");
    require(options.switch_port_id == "honeypot-42", "port ID lost");
    require(options.switch_label == 0x1234 and options.switch_label_set,
            "switch label lost");
    require(options.switch_exit_node, "exit mode lost");

    require(parse_fails({"--switch-socket", "/tmp/x"}), "missing port ID/label accepted");
    require(parse_fails({"--switch-socket", "/tmp/x", "--switch-label", "1"}),
            "missing port ID accepted");
    require(parse_fails({"--switch-label", "1"}), "label without socket accepted");
    require(parse_fails({"--switch-exit-node"}), "exit without socket accepted");
    require(parse_fails({"--switch-socket", "/tmp/x", "--switch-port-id", "x",
                         "--switch-label", "-1"}),
            "negative label accepted");

    std::cout << "PASS: switch CLI dependencies and label parsing\n";
}
