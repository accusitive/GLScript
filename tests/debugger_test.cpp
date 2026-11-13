// End-to-end tests for the TCP debug protocol. The script runs under a
// DebugServer on a background thread; this thread is a raw POSIX socket
// client driving the NDJSON protocol, exactly like the GUI does.

#include "glscript/checker.hpp"
#include "glscript/debug_server.hpp"
#include "glscript/diagnostic.hpp"
#include "glscript/host.hpp"
#include "glscript/runtime.hpp"

#include "engine_bindings.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int passes = 0;

void fail(const std::string& name, const std::string& message) {
    ++failures;
    std::cerr << "FAIL: " << name << "\n  " << message << "\n";
}

void pass(const std::string& name) {
    ++passes;
    std::cout << "ok: " << name << "\n";
}

void expect(bool condition, const std::string& name, const std::string& message) {
    if (condition) {
        pass(name);
    } else {
        fail(name, message);
    }
}

// 1-based line number of the first line containing `needle`.
std::uint32_t line_of(const std::string& source, const std::string& needle) {
    std::uint32_t line = 1;
    std::size_t start = 0;
    while (start <= source.size()) {
        const auto end = source.find('\n', start);
        const auto text = source.substr(start, end == std::string::npos
                                                   ? std::string::npos
                                                   : end - start);
        if (text.find(needle) != std::string::npos) {
            return line;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
        ++line;
    }
    return 0;
}

class Client {
public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    ~Client() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connected() const { return fd_ >= 0; }

    bool send(std::int64_t id, const std::string& command,
              nlohmann::json payload = nlohmann::json::object()) {
        payload["id"] = id;
        payload["command"] = command;
        const auto line = payload.dump() + "\n";
        std::size_t sent = 0;
        while (sent < line.size()) {
            const auto chunk = ::send(fd_, line.data() + sent, line.size() - sent,
                                      MSG_NOSIGNAL);
            if (chunk <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(chunk);
        }
        return true;
    }

    std::optional<nlohmann::json> read() {
        while (true) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                const auto line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                if (line.empty()) {
                    continue;
                }
                auto message = nlohmann::json::parse(line, nullptr, false);
                if (message.is_discarded()) {
                    continue;
                }
                return message;
            }
            char buffer[4096];
            const auto received = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                return std::nullopt;
            }
            buffer_.append(buffer, static_cast<std::size_t>(received));
        }
    }

    // Reads until the response carrying `id`, buffering any events seen first.
    std::optional<nlohmann::json> response(std::int64_t id) {
        while (true) {
            auto message = read();
            if (!message) {
                return std::nullopt;
            }
            if (message->contains("id") && (*message)["id"] == id) {
                return message;
            }
            events_.push_back(*message);
        }
    }

    // Reads until the named event arrives, discarding/buffering others.
    std::optional<nlohmann::json> event(const std::string& name) {
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (events_[i].value("event", std::string()) == name) {
                auto found = events_[i];
                events_.erase(events_.begin() + static_cast<std::ptrdiff_t>(i));
                return found;
            }
        }
        while (true) {
            auto message = read();
            if (!message) {
                return std::nullopt;
            }
            if (message->value("event", std::string()) == name) {
                return message;
            }
            events_.push_back(*message);
        }
    }

    // Events read while waiting for a response/event that has not been
    // returned to the caller yet.
    const std::vector<nlohmann::json>& buffered_events() const { return events_; }

private:
    int fd_ = -1;
    std::string buffer_;
    std::vector<nlohmann::json> events_;
};

const char* kScript = R"GL(
function total(values: array<int>) -> int {
    let sum = 0;
    let index = 0;
    while (index < values.len()) {
        let value = values[index];
        sum = sum + value;
        index = index + 1;
    }
    return sum;
}

function main() {
    let values = [1, 2, 3, 4];
    let result = total(values);
    println(`result: ${result}`);
}
)GL";

void test_debug_protocol() {
    const std::string source = kScript;
    const auto break_line = line_of(source, "sum = sum + value;");
    const auto entry_line = line_of(source, "let values = [1, 2, 3, 4];");

    glscript::HostRegistry host;
    glscript::DebugServer* debug_server = nullptr;
    glscript::register_engine_bindings(host, [&](const std::string& line) {
        if (debug_server != nullptr) {
            debug_server->emit_output(line + "\n");
        }
    });
    glscript::DiagnosticSink diagnostics;
    glscript::Frontend frontend(host, diagnostics);
    const bool checked = frontend.check_source("debugger_test", "debugger_test.gl",
                                               source);
    if (!checked || diagnostics.has_errors()) {
        fail("debugger: check script", diagnostics.format());
        return;
    }
    pass("debugger: check script");

    const auto bytecode = glscript::compile_program(frontend.program());
    glscript::Runtime runtime(frontend.program(), bytecode, host, diagnostics);
    glscript::DebugServer server(frontend.program(), bytecode, 0);
    if (!server.start()) {
        fail("debugger: start server", "could not bind an ephemeral port");
        return;
    }
    const auto port = server.bound_port();
    expect(port != 0, "debugger: start server", "bound_port() returned 0");
    debug_server = &server;

    std::thread worker([&]() {
        server.wait_for_client();
        runtime.set_debug_hook(&server);
        const auto result = runtime.run("debugger_test", "main");
        server.finish(runtime.debug_failed(), result);
    });

    Client client(port);
    if (!client.connected()) {
        fail("debugger: connect", "could not connect to 127.0.0.1:" +
                                      std::to_string(port));
        worker.join();
        return;
    }
    pass("debugger: connect");

    const auto entry = client.event("stopped");
    expect(entry && entry->value("reason", std::string()) == "entry" &&
               entry->value("line", 0u) == entry_line,
           "debugger: entry stop", entry ? entry->dump() : "no stopped event");

    auto response = [&](std::int64_t id, const std::string& command,
                        nlohmann::json payload = nlohmann::json::object()) {
        if (!client.send(id, command, std::move(payload))) {
            return std::optional<nlohmann::json>{};
        }
        return client.response(id);
    };

    const auto program = response(1, "program");
    expect(program && program->contains("functions") &&
               (*program)["functions"].size() >= 2 &&
               (*program)["files"].dump().find("debugger_test.gl") !=
                   std::string::npos,
           "debugger: program", program ? program->dump() : "no response");

    const auto source_response = response(2, "source", {{"file", "debugger_test.gl"}});
    expect(source_response && (*source_response)["content"] == source,
           "debugger: source", source_response ? "content mismatch" : "no response");

    // Step in from entry: the next source line in main.
    auto step_response = response(3, "step_in");
    auto stopped = client.event("stopped");
    expect(step_response.has_value() && stopped &&
               stopped->value("reason", std::string()) == "step" &&
               stopped->value("function", std::string()).ends_with("::main"),
           "debugger: step_in", stopped ? stopped->dump() : "no stopped event");

    const auto break_response =
        response(4, "set_breakpoints",
                 {{"file", "debugger_test.gl"}, {"lines", {break_line}}});
    expect(break_response && (*break_response)["accepted"] ==
                                 nlohmann::json::array({break_line}),
           "debugger: set_breakpoints",
           break_response ? break_response->dump() : "no response");

    response(5, "continue");
    stopped = client.event("stopped");
    expect(stopped && stopped->value("reason", std::string()) == "breakpoint" &&
               stopped->value("line", 0u) == break_line &&
               stopped->value("function", std::string()).ends_with("::total"),
           "debugger: breakpoint hit",
           stopped ? stopped->dump() : "no stopped event");
    if (!stopped) {
        client.send(99, "continue");
        worker.join();
        return;
    }
    const auto frame_index = stopped->value("frame_index", 0u);

    const auto stack = response(6, "stack_trace");
    expect(stack && (*stack)["frames"].size() == 2 &&
               (*stack)["frames"][0]["function"].get<std::string>().ends_with(
                   "::total"),
           "debugger: stack_trace", stack ? stack->dump() : "no response");

    const auto scopes = response(7, "scopes", {{"frame_index", frame_index}});
    std::string locals_dump = scopes ? scopes->dump() : "";
    const auto has_local = [&](const std::string& name) {
        return locals_dump.find("\"name\":\"" + name + "\"") !=
               std::string::npos;
    };
    expect(scopes && has_local("values") && has_local("sum") &&
               has_local("index") && has_local("value"),
           "debugger: scopes", scopes ? scopes->dump() : "no response");

    // Expandable array: ref from scopes -> indexed elements.
    if (scopes) {
        std::int64_t values_ref = 0;
        for (const auto& local : (*scopes)["locals"]) {
            if (local.value("name", std::string()) == "values") {
                values_ref = local.value("ref", std::int64_t{0});
            }
        }
        const auto variables = response(8, "variables", {{"ref", values_ref}});
        expect(variables && (*variables)["variables"].size() == 4 &&
                   (*variables)["variables"][2]["value"] == "3",
               "debugger: variables", variables ? variables->dump() : "no response");
    }

    const auto evaluation =
        response(9, "evaluate", {{"frame_index", frame_index},
                                 {"expression", "sum + value"}});
    expect(evaluation && evaluation->value("value", std::string()) == "1" &&
               evaluation->value("type", std::string()) == "int",
           "debugger: evaluate", evaluation ? evaluation->dump() : "no response");

    const auto indexed =
        response(10, "evaluate", {{"frame_index", frame_index},
                                  {"expression", "values[2]"}});
    expect(indexed && indexed->value("value", std::string()) == "3",
           "debugger: evaluate index",
           indexed ? indexed->dump() : "no response");

    const auto bad_eval =
        response(11, "evaluate", {{"frame_index", frame_index},
                                  {"expression", "values.len()"}});
    expect(bad_eval && bad_eval->contains("error"),
           "debugger: evaluate rejects calls",
           bad_eval ? bad_eval->dump() : "no response");

    const auto disassembly = response(12, "disassemble");
    bool found_current = false;
    if (disassembly) {
        for (const auto& row : (*disassembly)["rows"]) {
            found_current = found_current || row.value("current", false);
        }
    }
    expect(disassembly && disassembly->contains("rows") &&
               !(*disassembly)["rows"].empty() && found_current,
           "debugger: disassemble",
           disassembly ? disassembly->dump() : "no response");

    // Step over the current source line; breakpoints are cleared first so
    // they cannot pre-empt the step stop.
    response(13, "set_breakpoints",
             {{"file", "debugger_test.gl"}, {"lines", nlohmann::json::array()}});
    const auto step_over = response(14, "step_over");
    const auto stepped = client.event("stopped");
    expect(step_over.has_value() && stepped &&
               stepped->value("reason", std::string()) == "step" &&
               stepped->value("line", 0u) != break_line,
           "debugger: step_over", stepped ? stepped->dump() : "no stopped event");

    // A breakpoint on a multi-instruction line must stop once, not once per
    // instruction. Stop on `println` (after the loop, so it runs exactly once)
    // and make sure continuing from it runs to exit without another stop on
    // the same line.
    const auto print_line = line_of(source, "println(`result: ${result}`);");
    const auto print_break =
        response(15, "set_breakpoints",
                 {{"file", "debugger_test.gl"}, {"lines", {print_line}}});
    expect(print_break && (*print_break)["accepted"] ==
                              nlohmann::json::array({print_line}),
           "debugger: breakpoint rearm",
           print_break ? print_break->dump() : "no response");

    response(16, "continue");
    stopped = client.event("stopped");
    expect(stopped && stopped->value("reason", std::string()) == "breakpoint" &&
               stopped->value("line", 0u) == print_line,
           "debugger: breakpoint one stop per line",
           stopped ? stopped->dump() : "no stopped event");

    response(17, "continue");
    std::optional<nlohmann::json> exited;
    bool saw_result = false;
    int repeated_breakpoints = 0;
    const auto scan_output = [&](const nlohmann::json& message) {
        if (message.value("event", std::string()) == "output" &&
            message.value("text", std::string()).find("result: 10") !=
                std::string::npos) {
            saw_result = true;
        }
    };
    for (const auto& message : client.buffered_events()) {
        scan_output(message);
    }
    std::int64_t next_id = 18;
    while (auto message = client.read()) {
        scan_output(*message);
        const auto event = message->value("event", std::string());
        if (event == "stopped") {
            if (message->value("line", 0u) == print_line) {
                ++repeated_breakpoints;
            }
            // If the server stopped again (the bug), keep it moving so the
            // test still reaches exit and can report the count.
            client.send(next_id++, "continue");
        } else if (event == "exited") {
            exited = *message;
            break;
        }
    }
    expect(repeated_breakpoints == 0,
           "debugger: breakpoint does not repeat within a line",
           std::to_string(repeated_breakpoints) +
               " extra stop(s) on line " + std::to_string(print_line));
    expect(exited && !exited->value("failed", true),
           "debugger: exited", exited ? exited->dump() : "no exited event");
    expect(saw_result, "debugger: output event", "missing output event");

    worker.join();
}

} // namespace

int main() {
    test_debug_protocol();

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
