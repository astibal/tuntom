#pragma once
#include "control_protocol.hpp"
#include <functional>
#include <cstdint>
#include <utility>

namespace tuntom {
enum class ControlPermission : unsigned { read = 1, classifier_validate = 2, classifier_write = 4, rules_validate = 8, rules_write = 16, divert_write = 32 };
struct ControlAccess {
    unsigned permissions = 0;
    static ControlAccess all() { return {63}; }
    bool allows(ControlPermission p) const { return (permissions & static_cast<unsigned>(p)) != 0; }
};
struct ControlRequest {
    std::string command, body;
};
struct ControlResponse {
    bool success = true;
    std::string body;
    bool rejected = false;
};
struct ControlOperation {
    std::string family, operation;
    std::size_t length = 0;
    ControlPermission permission = ControlPermission::read;
    bool cheap = false, mutation = false, framed = true;
};
class ControlDispatcher {
public:
    using Provider = std::function<std::string()>;
    using Handler = std::function<std::string(const std::string&, const std::string&)>;
    Provider stats, flows, ports;
    Handler rules, classifier;
    static ControlOperation parse(std::string command) {
        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.pop_back();
        if (command == "show stats") return {"stats", "show", 0, ControlPermission::read, true, false, false};
        if (command == "show ports") return {"ports", "show", 0, ControlPermission::read, false, false, true};
        if (command == "show flows") return {"flows", "show", 0, ControlPermission::read, false, false, true};
        const auto a = command.find(' '), b = a == std::string::npos ? a : command.find(' ', a + 1);
        if (b == std::string::npos) throw std::runtime_error("unknown_command");
        ControlOperation op;
        op.family = command.substr(0, a); op.operation = command.substr(a + 1, b - a - 1);
        op.length = control_length(command.substr(b + 1));
        if (op.family != "classifier" && op.family != "rules" && op.family != "divert") throw std::runtime_error("unknown_command");
        if (op.operation == "show") {
            if (op.length) throw std::runtime_error("show has no body");
        } else if (op.family == "divert") {
            if (op.length || (op.operation != "enable" && op.operation != "stop")) throw std::runtime_error("expected divert enable|stop|show 0");
            op.permission = ControlPermission::divert_write; op.mutation = true;
        } else if (op.operation == "check") {
            op.permission = op.family == "classifier" ? ControlPermission::classifier_validate : ControlPermission::rules_validate;
        } else if (op.operation == "load" || (op.family == "classifier" && (op.operation == "load-flush" || op.operation == "disable"))) {
            if (op.operation == "disable" && op.length) throw std::runtime_error("disable has no body");
            op.permission = op.family == "classifier" ? ControlPermission::classifier_write : ControlPermission::rules_write;
            op.mutation = true;
        } else throw std::runtime_error("unknown operation");
        return op;
    }
    ControlResponse execute(const ControlRequest& request, ControlAccess access) const {
        ControlOperation op;
        try {
            op = parse(request.command);
            if (!access.allows(op.permission)) return {false, "control access denied\n", true};
            if (op.length != request.body.size()) return {false, "invalid control body length\n", true};
        } catch (const std::exception& e) { return {false, std::string(e.what()) + "\n", true}; }
        try {
            if (op.family == "stats" && stats) return {true, stats()};
            if (op.family == "ports" && ports) return {true, ports()};
            if (op.family == "flows" && flows) return {true, flows()};
            if (op.family == "classifier" && classifier) return {true, classifier(op.operation, request.body)};
            if ((op.family == "rules" || op.family == "divert") && rules)
                return {true, rules(op.family == "divert" ? "divert." + op.operation : op.operation, request.body)};
            return {false, "command unsupported by this component\n", true};
        } catch (const std::bad_alloc&) { throw; }
        catch (const std::exception& e) { return {false, std::string(e.what()) + "\n"}; }
    }
};
} // namespace tuntom
