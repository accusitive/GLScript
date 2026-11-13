#pragma once

#include "glscript/debug.hpp"
#include "glscript/value.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace glscript {

class Runtime;

// TCP debug server speaking the NDJSON protocol documented in
// docs/debugger-plan.md. Implements DebugHook so it can be installed on a
// Runtime with set_debug_hook(); all protocol work runs on the interpreter's
// thread (cooperative: on_instruction pumps the socket and blocks while
// paused, so the runtime is never touched from another thread).
//
// start() binds 127.0.0.1:<port>; wait_for_client() blocks until the first
// GUI connects. emit_output() taps the print sink; finish() sends the exited
// event. Between stops, requests are drained only every poll_interval()
// instructions — breakpoints stay exact, other traffic waits.
class DebugServer final : public DebugHook {
public:
    DebugServer(const Program& program, const CompiledProgram& bytecode,
                std::uint16_t port);
    ~DebugServer() override;

    DebugServer(const DebugServer&) = delete;
    DebugServer& operator=(const DebugServer&) = delete;

    // Binds and listens. Returns false (with a message on stderr) on failure.
    // Port 0 asks the OS for an ephemeral port; read it back with bound_port().
    bool start();
    // Blocks until a client connects. No-op when one already has.
    void wait_for_client();
    // The port actually bound by start() (0 when not listening).
    std::uint16_t bound_port() const;

    // DebugHook: checks breakpoints / step state / pause flag; when a stop is
    // due, sends a stopped event and pumps requests until a resume command.
    void on_instruction(Runtime& runtime, const DebugFrame& frame,
                        const SourceSpan& span) override;

    // Forwards print-sink output as an output event (same thread only).
    void emit_output(const std::string& text);
    // Sends the exited event and closes the client connection.
    void finish(bool failed, const Value& result);

    bool client_connected() const { return client_fd_ >= 0; }

    // How often (in instructions) to poll the socket while running.
    static constexpr std::uint32_t poll_interval() { return 64; }

private:
    struct StepMode {
        enum class Kind { None, In, Over };
        Kind kind = Kind::None;
        std::uint32_t depth = 0;
        std::uint32_t line = 0;
    };

    // --- protocol plumbing -------------------------------------------------
    void send_text(const std::string& text);
    bool send_message(const nlohmann::json& message);
    // Nonblocking drain: processes complete requests, buffers the rest.
    void drain_socket();
    // Blocking read of one request line; empty string = client disconnected.
    std::string read_line_blocking();
    // True when a complete line is buffered.
    bool has_complete_line() const;

    // --- request handling --------------------------------------------------
    // Handles one request; returns true when the interpreter should resume.
    void handle_request(Runtime& runtime, const nlohmann::json& request,
                        const DebugFrame& paused_frame, bool& resume,
                        StepMode& mode);
    void send_stopped(const DebugFrame& frame, const SourceSpan& span,
                      const char* reason, std::uint32_t frame_index);
    void send_error(std::int64_t id, const std::string& message);

    // --- debugger state ----------------------------------------------------
    void set_file_breakpoints(const std::string& file, std::vector<std::int64_t> lines);
    bool breakpoint_hit(const FunctionBytecode* fn, std::uint32_t ip) const;
    static std::uint32_t span_line(const SourceSpan& span);

    void clear_variable_refs();
    std::string stringify_for_display(Runtime& runtime, const Value& value,
                                      bool& expandable, std::int64_t& ref);

    int listen_fd_ = -1;
    int client_fd_ = -1;
    std::string input_buffer_;

    const Program& program_;
    const CompiledProgram& bytecode_;
    std::uint16_t port_;

    // file path -> breakpoint lines; and the compiled instruction indices.
    std::unordered_map<std::string, std::unordered_set<std::uint32_t>> breakpoints_;
    std::unordered_map<const FunctionBytecode*, std::unordered_set<std::uint32_t>>
        breakpoint_ips_;

    StepMode step_;
    bool pause_requested_ = false;
    bool entered_ = false; // first hook call after attach stops with reason "entry"
    std::uint32_t poll_counter_ = 0;

    // A source line compiles to several instructions. After stopping on a
    // breakpoint, ignore further breakpoints on that same line until execution
    // reaches a different line; otherwise `continue` would stop again on every
    // instruction of the statement.
    const FunctionBytecode* breakpoint_line_fn_ = nullptr;
    std::uint32_t breakpoint_line_ = 0;

    // Expandable-value references handed out while paused (invalid on resume).
    std::int64_t next_ref_ = 1;
    std::unordered_map<std::int64_t, Value> variable_refs_;
};

} // namespace glscript
