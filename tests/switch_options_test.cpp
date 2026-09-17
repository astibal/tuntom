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
    require(parse_tunnel_id("42") == 42, "base tunnel ID changed");
    require(parse_tunnel_id("42_1") == 298, "member key is incorrect");
    require(parse_tunnel_id("255_63") == 16383, "maximum member key is incorrect");
    for (const auto* bad : {"", "0", "256", "298", "42_0", "42_64", "42_1_2",
                            "42_", "42x", "042", "42_01", "-1", "+42", " 42"}) {
        bool rejected = false;
        try { (void)parse_tunnel_id(bad); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid tunnel/member ID accepted");
    }
    std::vector<std::string> arguments {
        "--switch-socket", "/run/tuntom/a.sock",
        "--switch-port-id", "edge-42",
        "--switch-label", "0x1234",
        "--switch-exit-node",
        "--classifier-file", "/etc/tuntom/ingress.rules",
        "--control-socket", "/run/tuntom/42c.control",
    };
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    Options options;
    parse_options(static_cast<int>(argv.size()), argv.data(), 0, options);
    require(options.switch_socket == "/run/tuntom/a.sock", "socket path lost");
    require(options.switch_port_id == "edge-42", "port ID lost");
    require(options.switch_label == 0x1234 and options.switch_label_set,
            "switch label lost");
    require(options.switch_exit_node, "exit mode lost");
    require(options.classifier_file == "/etc/tuntom/ingress.rules", "classifier path lost");
    require(options.control_socket == "/run/tuntom/42c.control", "control socket lost");

    require(parse_fails({"--switch-socket", "/tmp/x"}), "missing port ID/label accepted");
    require(parse_fails({"--switch-socket", "/tmp/x", "--switch-label", "1"}),
            "missing port ID accepted");
    require(parse_fails({"--switch-label", "1"}), "label without socket accepted");
    require(parse_fails({"--switch-exit-node"}), "exit without socket accepted");
    require(parse_fails({"--classifier-file", "/tmp/rules"}), "classifier without switch accepted");
    require(parse_fails({"--classifier-file"}), "classifier without filename accepted");
    require(parse_fails({"--classifier-file", ""}), "empty classifier filename accepted");
    require(parse_fails({"--switch-socket", "/tmp/x", "--switch-port-id", "x",
                         "--switch-label", "-1"}),
            "negative label accepted");

    require(!parse_fails({"--relay-connect", "/tmp/switch", "--relay-port-id", "proxy-link"}), "valid hub relay rejected");
    require(!parse_fails({"--relay-listen", "/tmp/adapters"}), "valid remote relay rejected");
    require(parse_fails({"--relay-connect", "/tmp/switch"}), "relay without port ID accepted");
    require(parse_fails({"--relay-port-id", "proxy-link"}), "relay ID without connect accepted");
    require(parse_fails({"--relay-listen", "/tmp/a", "--relay-connect", "/tmp/b", "--relay-port-id", "p"}), "both relay roles accepted");
    require(parse_fails({"--relay-listen", "/tmp/a", "--classifier-file", "/tmp/r"}), "mixed relay/classifier accepted");
    require(parse_fails({"--relay-listen", ""}), "empty relay socket accepted");
    std::cout << "PASS: switch CLI dependencies and label parsing\n";
}
