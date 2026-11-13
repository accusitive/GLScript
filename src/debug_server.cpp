#include "glscript/debug_server.hpp"

#include "glscript/dbg_eval.hpp"
#include "glscript/disasm.hpp"
#include "glscript/runtime.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace glscript {
namespace {

// std::string receive/send helpers. MSG_NOSIGNAL avoids dying on SIGPIPE
// when the GUI disconnects mid-write.
bool send_all(int fd, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        const auto chunk = ::send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

} // namespace

DebugServer::DebugServer(const Program& program, const CompiledProgram& bytecode,
                         std::uint16_t port)
    : program_(program), bytecode_(bytecode), port_(port) {}

DebugServer::~DebugServer() {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

bool DebugServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "glc: debug server: socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    const int reuse = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr, "glc: debug server: bind 127.0.0.1:%u failed: %s\n",
                     static_cast<unsigned>(port_), std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 1) != 0) {
        std::fprintf(stderr, "glc: debug server: listen failed: %s\n",
                     std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    return true;
}

void DebugServer::wait_for_client() {
    if (listen_fd_ < 0 || client_fd_ >= 0) {
        return;
    }
    client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd_ >= 0) {
        std::fprintf(stderr, "glc: debugger attached\n");
    }
}

std::uint16_t DebugServer::bound_port() const {
    if (listen_fd_ < 0) {
        return 0;
    }
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

// ---------------------------------------------------------------------------
// Wire plumbing
// ---------------------------------------------------------------------------

bool DebugServer::send_message(const nlohmann::json& message) {
    if (client_fd_ < 0) {
        return false;
    }
    const auto text = message.dump() + "\n";
    if (!send_all(client_fd_, text.data(), text.size())) {
        std::fprintf(stderr, "glc: debugger disconnected (send failed)\n");
        ::close(client_fd_);
        client_fd_ = -1;
        input_buffer_.clear();
        return false;
    }
    return true;
}

std::string DebugServer::read_line_blocking() {
    while (true) {
        const auto newline = input_buffer_.find('\n');
        if (newline != std::string::npos) {
            auto line = input_buffer_.substr(0, newline);
            input_buffer_.erase(0, newline + 1);
            return line;
        }
        char buffer[4096];
        const auto received = ::recv(client_fd_, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            if (received < 0 && errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "glc: debugger disconnected\n");
            ::close(client_fd_);
            client_fd_ = -1;
            input_buffer_.clear();
            return {};
        }
        input_buffer_.append(buffer, static_cast<std::size_t>(received));
    }
}

void DebugServer::drain_socket() {
    // Accept a first (or replacement) client without blocking.
    if (client_fd_ < 0) {
        const auto fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd >= 0) {
            client_fd_ = fd;
            std::fprintf(stderr, "glc: debugger attached\n");
        }
        return;
    }

    while (true) {
        char buffer[4096];
        const auto received =
            ::recv(client_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "glc: debugger disconnected\n");
            ::close(client_fd_);
            client_fd_ = -1;
            input_buffer_.clear();
            return;
        }
        if (received == 0) {
            std::fprintf(stderr, "glc: debugger disconnected\n");
            ::close(client_fd_);
            client_fd_ = -1;
            input_buffer_.clear();
            return;
        }
        input_buffer_.append(buffer, static_cast<std::size_t>(received));

        // While running only pause / set_breakpoints make sense; anything
        // else gets a not-paused error. The buffer may hold a partial line —
        // process only complete ones.
        std::size_t newline;
        while ((newline = input_buffer_.find('\n')) != std::string::npos) {
            const auto line = input_buffer_.substr(0, newline);
            input_buffer_.erase(0, newline + 1);
            if (line.empty()) {
                continue;
            }
            const auto request = nlohmann::json::parse(line, nullptr, false);
            if (request.is_discarded() || !request.contains("command")) {
                continue;
            }
            const auto id = request.value("id", static_cast<std::int64_t>(-1));
            const auto command = request.value("command", std::string());
            if (command == "pause") {
                pause_requested_ = true;
                send_message(nlohmann::json{{"id", id}});
            } else if (command == "set_breakpoints") {
                set_file_breakpoints(request.value("file", std::string()),
                                     request.value("lines", std::vector<std::int64_t>{}));
                send_message(nlohmann::json{{"id", id}});
            } else {
                send_message(nlohmann::json{
                    {"id", id}, {"error", "not paused: command requires a stop"}});
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

std::uint32_t DebugServer::span_line(const SourceSpan& span) {
    if (span.file == nullptr) {
        return 0;
    }
    return span.file->location(span.begin).line;
}

bool DebugServer::breakpoint_hit(const FunctionBytecode* fn, std::uint32_t ip) const {
    const auto found = breakpoint_ips_.find(fn);
    return found != breakpoint_ips_.end() && found->second.count(ip) > 0;
}

void DebugServer::set_file_breakpoints(const std::string& file,
                                       std::vector<std::int64_t> lines) {
    std::unordered_set<std::uint32_t> normalized;
    for (const auto line : lines) {
        if (line > 0) {
            normalized.insert(static_cast<std::uint32_t>(line));
        }
    }
    breakpoints_[file] = std::move(normalized);

    // Recompile the line breakpoints into instruction indices. Breakpoint
    // lines are matched exactly (the source panel only offers statement
    // starts, so this stays predictable).
    breakpoint_ips_.clear();
    for (const auto& function : bytecode_.functions) {
        auto& ips = breakpoint_ips_[&function];
        std::uint32_t ip = 0;
        while (ip < function.code.size()) {
            const auto op = static_cast<Op>(function.code[ip]);
            if (ip < function.spans.size()) {
                const auto& span = function.spans[ip];
                if (span.file != nullptr) {
                    const auto file_it = breakpoints_.find(span.file->path());
                    if (file_it != breakpoints_.end() &&
                        file_it->second.count(span_line(span)) > 0) {
                        ips.insert(ip);
                    }
                }
            }
            ip += 1 + static_cast<std::uint32_t>(operand_count(op));
        }
    }
}

void DebugServer::on_instruction(Runtime& runtime, const DebugFrame& frame,
                                 const SourceSpan& span) {
    if (client_fd_ < 0) {
        // No client: poll for a (re)connect occasionally instead of ignoring
        // the listener forever. A client that attaches mid-run gets the same
        // entry stop as one that was waiting before the script started.
        if (listen_fd_ >= 0 && ++poll_counter_ >= poll_interval()) {
            poll_counter_ = 0;
            drain_socket();
            if (client_fd_ >= 0) {
                entered_ = false;
            }
        }
        return;
    }

    const char* reason = nullptr;
    const auto source_line = span_line(span);
    if (breakpoint_line_fn_ != nullptr &&
        (frame.fn != breakpoint_line_fn_ || source_line != breakpoint_line_)) {
        // Execution moved off the line that last hit a breakpoint.
        breakpoint_line_fn_ = nullptr;
        breakpoint_line_ = 0;
    }
    const bool repeated_breakpoint =
        breakpoint_line_fn_ == frame.fn && source_line == breakpoint_line_;
    if (!entered_) {
        entered_ = true;
        reason = "entry";
    } else if (pause_requested_) {
        pause_requested_ = false;
        reason = "pause";
    } else if (!repeated_breakpoint && breakpoint_hit(frame.fn, frame.ip)) {
        reason = "breakpoint";
    } else if (step_.kind == StepMode::Kind::In && span_line(span) != step_.line) {
        reason = "step";
    } else if (step_.kind == StepMode::Kind::Over &&
               runtime.debug_frame_count() <= step_.depth &&
               span_line(span) != step_.line) {
        reason = "step";
    }

    if (reason == nullptr) {
        if (++poll_counter_ >= poll_interval()) {
            poll_counter_ = 0;
            drain_socket();
        }
        return;
    }

    if (std::strcmp(reason, "breakpoint") == 0) {
        breakpoint_line_fn_ = frame.fn;
        breakpoint_line_ = source_line;
    } else {
        breakpoint_line_fn_ = nullptr;
        breakpoint_line_ = 0;
    }

    step_ = StepMode{};
    pause_requested_ = false;

    // --- paused ------------------------------------------------------------
    clear_variable_refs();
    send_stopped(frame, span, reason, runtime.debug_frame_count() - 1);
    bool resume = false;
    StepMode mode;
    while (!resume) {
        const auto line = read_line_blocking();
        if (line.empty()) {
            // Client dropped while paused: let the script proceed.
            clear_variable_refs();
            return;
        }
        const auto request = nlohmann::json::parse(line, nullptr, false);
        if (request.is_discarded()) {
            send_error(-1, "malformed request line");
            continue;
        }
        handle_request(runtime, request, frame, resume, mode);
    }
    step_ = mode;
    clear_variable_refs();
}

void DebugServer::send_stopped(const DebugFrame& frame, const SourceSpan& span,
                               const char* reason, std::uint32_t frame_index) {
    send_message(nlohmann::json{
        {"event", "stopped"},
        {"reason", reason},
        {"frame_index", frame_index},
        {"function", frame.fn->qualified_name},
        {"function_index", frame.function},
        {"ip", frame.ip},
        {"line", span_line(span)},
        {"file", span.file != nullptr ? span.file->path() : ""}});
}

void DebugServer::send_error(std::int64_t id, const std::string& message) {
    send_message(nlohmann::json{{"id", id}, {"error", message}});
}

// ---------------------------------------------------------------------------
// Paused-state requests
// ---------------------------------------------------------------------------

void DebugServer::clear_variable_refs() {
    variable_refs_.clear();
    next_ref_ = 1;
}

std::string DebugServer::stringify_for_display(Runtime& runtime, const Value& value,
                                               bool& expandable, std::int64_t& ref) {
    expandable = false;
    ref = 0;
    if (value.is_heap()) {
        const auto kind = value.as_heap()->kind;
        if (kind == GcObject::Kind::Array || kind == GcObject::Kind::Object ||
            kind == GcObject::Kind::Enum) {
            expandable = true;
            ref = next_ref_++;
            variable_refs_[ref] = value;
        }
    }
    auto text = runtime.stringify(value);
    if (text.size() > 240) {
        text = text.substr(0, 240) + " …";
    }
    return text;
}

void DebugServer::handle_request(Runtime& runtime, const nlohmann::json& request,
                                 const DebugFrame& paused_frame, bool& resume,
                                 StepMode& mode) {
    const auto id = request.value("id", static_cast<std::int64_t>(-1));
    const auto command = request.value("command", std::string());

    if (command == "continue") {
        resume = true;
        send_message(nlohmann::json{{"id", id}});
        return;
    }
    if (command == "step_in" || command == "step_over") {
        mode.kind = command == "step_in" ? StepMode::Kind::In : StepMode::Kind::Over;
        mode.depth = runtime.debug_frame_count();
        mode.line = paused_frame.fn->spans.size() > paused_frame.ip
                        ? span_line(paused_frame.fn->spans[paused_frame.ip])
                        : 0;
        resume = true;
        send_message(nlohmann::json{{"id", id}});
        return;
    }
    if (command == "pause") {
        // Already paused.
        send_message(nlohmann::json{{"id", id}});
        return;
    }
    if (command == "set_breakpoints") {
        set_file_breakpoints(request.value("file", std::string()),
                             request.value("lines", std::vector<std::int64_t>{}));
        const auto& file = request.value("file", std::string());
        std::vector<std::uint32_t> accepted(breakpoints_[file].begin(),
                                            breakpoints_[file].end());
        std::sort(accepted.begin(), accepted.end());
        send_message(nlohmann::json{{"id", id}, {"accepted", accepted}});
        return;
    }
    if (command == "stack_trace") {
        nlohmann::json frames = nlohmann::json::array();
        for (std::uint32_t i = runtime.debug_frame_count(); i > 0; --i) {
            const auto frame = runtime.debug_frame(i - 1);
            const bool top = i - 1 == runtime.debug_frame_count() - 1;
            // The paused top frame reports the pending instruction; deeper
            // frames report where they will resume.
            const auto ip = top ? paused_frame.ip : frame.ip;
            const auto& span = ip < frame.fn->spans.size() ? frame.fn->spans[ip]
                                                           : SourceSpan{};
            frames.push_back(nlohmann::json{
                {"index", i - 1},
                {"function", frame.fn->qualified_name},
                {"function_index", frame.function},
                {"ip", ip},
                {"file", span.file != nullptr ? span.file->path() : ""},
                {"line", span_line(span)},
                {"resumed", frame.resumed_slice}});
        }
        send_message(nlohmann::json{{"id", id}, {"frames", frames}});
        return;
    }
    if (command == "scopes") {
        const auto frame_index = request.value("frame_index", std::uint32_t{0});
        if (frame_index >= runtime.debug_frame_count()) {
            send_error(id, "no such frame");
            return;
        }
        const auto frame = runtime.debug_frame(frame_index);
        nlohmann::json locals = nlohmann::json::array();
        for (std::uint32_t slot = 0; slot < frame.local_count; ++slot) {
            if (slot >= frame.fn->local_names.size()) {
                continue;
            }
            const auto& name = frame.fn->local_names[slot];
            if (name.empty() || name.front() == '$') {
                continue; // compiler-generated slot
            }
            bool expandable = false;
            std::int64_t ref = 0;
            const auto text =
                stringify_for_display(runtime, runtime.debug_stack(frame.base + slot),
                                      expandable, ref);
            nlohmann::json entry{{"name", name},
                                 {"type", runtime.debug_stack(frame.base + slot).type_name()},
                                 {"value", text}};
            if (expandable) {
                entry["ref"] = ref;
            }
            locals.push_back(std::move(entry));
        }
        send_message(nlohmann::json{{"id", id}, {"locals", locals}});
        return;
    }
    if (command == "variables") {
        const auto ref = request.value("ref", std::int64_t{0});
        const auto found = variable_refs_.find(ref);
        if (found == variable_refs_.end()) {
            send_error(id, "stale variable reference");
            return;
        }
        const auto& value = found->second;
        nlohmann::json variables = nlohmann::json::array();
        const auto push = [&](std::string name, const Value& element) {
            bool expandable = false;
            std::int64_t element_ref = 0;
            const auto text = stringify_for_display(runtime, element, expandable,
                                                    element_ref);
            nlohmann::json entry{{"name", std::move(name)},
                                 {"type", element.type_name()},
                                 {"value", text}};
            if (expandable) {
                entry["ref"] = element_ref;
            }
            variables.push_back(std::move(entry));
        };

        if (value.as_heap()->kind == GcObject::Kind::Array) {
            const auto* array = as_array(value);
            for (std::size_t i = 0; i < array->elements.size(); ++i) {
                push("[" + std::to_string(i) + "]", array->elements[i]);
            }
        } else if (value.as_heap()->kind == GcObject::Kind::Enum) {
            const auto* enum_value = as_enum(value);
            if (enum_value->variant != nullptr) {
                push("variant", runtime.make_string(enum_value->variant->name));
            }
            for (std::size_t i = 0; i < enum_value->payload.size(); ++i) {
                push("[" + std::to_string(i) + "]", enum_value->payload[i]);
            }
        } else if (value.as_heap()->kind == GcObject::Kind::Object) {
            // Fields ordered by slot, base chain first (mirrors find_field_slot).
            std::vector<std::pair<std::uint32_t, const std::string*>> fields;
            std::vector<const TypeSymbol*> chain;
            const auto* instance = as_object(value);
            for (auto* type = instance->type; type != nullptr;
                 type = type->base) {
                chain.push_back(type);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                for (const auto& [name, field] : (*it)->fields) {
                    fields.emplace_back(field.index, &name);
                }
            }
            std::sort(fields.begin(), fields.end(),
                      [](const auto& left, const auto& right) {
                          return left.first < right.first;
                      });
            for (const auto& [index, name] : fields) {
                push(*name, runtime.field(value, *name));
            }
        }
        send_message(nlohmann::json{{"id", id}, {"variables", variables}});
        return;
    }
    if (command == "evaluate") {
        const auto frame_index = request.value("frame_index", std::uint32_t{0});
        if (frame_index >= runtime.debug_frame_count()) {
            send_error(id, "no such frame");
            return;
        }
        const auto frame = runtime.debug_frame(frame_index);
        const auto expression = request.value("expression", std::string());
        std::string error;
        const auto value = evaluate_expression(runtime, frame, expression, error);
        if (!error.empty()) {
            send_message(nlohmann::json{{"id", id}, {"error", error}});
        } else {
            send_message(nlohmann::json{{"id", id},
                                        {"value", runtime.stringify(value)},
                                        {"type", value.type_name()}});
        }
        return;
    }
    if (command == "disassemble") {
        const auto function_index = request.value("function_index", paused_frame.function);
        if (function_index >= bytecode_.functions.size()) {
            send_error(id, "no such function");
            return;
        }
        const auto& function = bytecode_.functions[function_index];
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& line : disassemble(function, bytecode_)) {
            rows.push_back(nlohmann::json{
                {"ip", line.ip},
                {"text", line.text},
                {"line", line.line},
                {"current", function_index == paused_frame.function &&
                                line.ip == paused_frame.ip}});
        }
        send_message(nlohmann::json{{"id", id}, {"rows", rows},
                                    {"function", function.qualified_name}});
        return;
    }
    if (command == "source") {
        const auto path = request.value("file", std::string());
        for (const auto& source : program_.sources) {
            if (source != nullptr && source->path() == path) {
                send_message(nlohmann::json{{"id", id}, {"content", source->text()}});
                return;
            }
        }
        send_error(id, "unknown source file: " + path);
        return;
    }
    if (command == "program") {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& source : program_.sources) {
            if (source != nullptr) {
                files.push_back(source->path());
            }
        }
        nlohmann::json functions = nlohmann::json::array();
        for (std::size_t i = 0; i < bytecode_.functions.size(); ++i) {
            const auto& function = bytecode_.functions[i];
            functions.push_back(nlohmann::json{
                {"index", i},
                {"name", function.name},
                {"qualified_name", function.qualified_name},
                {"param_count", function.param_count},
                {"local_count", function.local_count}});
        }
        send_message(nlohmann::json{{"id", id}, {"files", files},
                                    {"functions", functions}});
        return;
    }
    send_error(id, "unknown command: " + command);
}

void DebugServer::emit_output(const std::string& text) {
    if (client_fd_ >= 0) {
        send_message(nlohmann::json{{"event", "output"}, {"text", text}});
    }
}

void DebugServer::finish(bool failed, const Value& result) {
    if (client_fd_ >= 0) {
        send_message(nlohmann::json{{"event", "exited"},
                                    {"failed", failed},
                                    {"result", result.is_unit() ? "" : "done"}});
        ::close(client_fd_);
        client_fd_ = -1;
    }
    entered_ = false;
}

} // namespace glscript
