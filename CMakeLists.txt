cmake_minimum_required(VERSION 3.20)

project(glscript
    VERSION 0.1.0
    DESCRIPTION "An embeddable scripting language for game engines"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(glscript
    src/source.cpp
    src/diagnostic.cpp
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/host.cpp
    src/checker.cpp
    src/lower.cpp
    src/hir_dump.cpp
    src/value.cpp
    src/compiler.cpp
    src/runtime.cpp
    src/disasm.cpp)

# ---------------------------------------------------------------------------
# Debugger: TCP debug protocol server + expression evaluator. Separate from
# the core library so embedders that don't need the debugger don't link
# sockets or the JSON parser. nlohmann/json is a single header, downloaded
# once into the build directory (same pattern as EnTT) unless
# GLSCRIPT_JSON_INCLUDE_DIR points at a copy.
# ---------------------------------------------------------------------------

set(_json_include_dir "${GLSCRIPT_JSON_INCLUDE_DIR}")
if(NOT _json_include_dir)
    set(_json_include_dir "${CMAKE_BINARY_DIR}/_deps/json-include")
    set(_json_header "${_json_include_dir}/nlohmann/json.hpp")
    if(NOT EXISTS "${_json_header}")
        file(MAKE_DIRECTORY "${_json_include_dir}/nlohmann")
        message(STATUS "Downloading nlohmann/json single-header")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
            "${_json_header}"
            STATUS _json_download_status
            TLS_VERIFY ON)
        list(GET _json_download_status 0 _json_download_code)
        if(NOT _json_download_code EQUAL 0)
            list(GET _json_download_status 1 _json_download_message)
            file(REMOVE "${_json_header}")
            message(WARNING
                "Could not download nlohmann/json (${_json_download_message}). "
                "Set GLSCRIPT_JSON_INCLUDE_DIR to a directory containing "
                "nlohmann/json.hpp, or install nlohmann-json system-wide.")
        endif()
    endif()
endif()

add_library(glscript_debug
    src/debug_server.cpp
    src/dbg_eval.cpp)

target_include_directories(glscript_debug
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${_json_include_dir})

target_link_libraries(glscript_debug PUBLIC glscript)

target_include_directories(glscript
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(glscript PUBLIC cxx_std_20)

add_executable(glc
    tools/glc.cpp
    examples/engine_bindings.cpp)

target_include_directories(glc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_link_libraries(glc PRIVATE glscript glscript_debug)

# A deliberately small embedding host for debugging: frontend + compiler +
# Runtime + DebugServer, plus a `hostdemo::Game` module to break on. Waits for
# the NSH debugger or any protocol client before running main().
add_executable(debugger_host
    examples/debug/host.cpp
    examples/engine_bindings.cpp)

target_include_directories(debugger_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_compile_definitions(debugger_host PRIVATE
    GLSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(debugger_host PRIVATE glscript glscript_debug)

enable_testing()

# Links raylib from the system when installed, otherwise fetches and builds
# it into the build directory. (raylib's installed config exports the target
# as `raylib`.)
function(glscript_link_raylib target)
    find_package(raylib QUIET)
    if(raylib_FOUND)
        target_link_libraries(${target} PRIVATE raylib)
    else()
        include(FetchContent)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(raylib
            URL https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz)
        FetchContent_MakeAvailable(raylib)
        target_link_libraries(${target} PRIVATE raylib)
    endif()
    target_compile_definitions(${target} PRIVATE GLSCRIPT_HAVE_RAYLIB)
endfunction()

# Links Jolt Physics from the system when installed, otherwise fetches and
# builds it (tagged release, static library) into the build directory.
function(glscript_link_jolt target)
    find_package(Jolt QUIET)
    if(Jolt_FOUND)
        target_link_libraries(${target} PRIVATE Jolt)
        return()
    endif()
    include(FetchContent)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(TARGET_PERF_TESTER OFF CACHE BOOL "" FORCE)
    set(TARGET_TEST_SUITE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(jolt
        URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.3.0.tar.gz
        SOURCE_SUBDIR Build) # Jolt keeps its CMakeLists in the Build folder
    FetchContent_MakeAvailable(jolt)
    target_link_libraries(${target} PRIVATE Jolt)
endfunction()

option(GLSCRIPT_BUILD_ENTT_EXAMPLE "Build the EnTT integration example" ON)
option(GLSCRIPT_BUILD_FPS_EXAMPLE "Build the first-person Jolt + raylib example" ON)

# EnTT (used by the demos). Downloaded once into the build directory unless
# GLSCRIPT_ENTT_INCLUDE_DIR points at a copy.
set(_entt_include_dir "${GLSCRIPT_ENTT_INCLUDE_DIR}")
if(NOT _entt_include_dir)
    set(_entt_include_dir "${CMAKE_BINARY_DIR}/_deps/entt-include")
    set(_entt_header "${_entt_include_dir}/entt/entt.hpp")
    if(NOT EXISTS "${_entt_header}")
        file(MAKE_DIRECTORY "${_entt_include_dir}/entt")
        message(STATUS "Downloading EnTT single-header")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/skypjack/entt/refs/heads/main/single_include/entt/entt.hpp"
            "${_entt_header}"
            STATUS _entt_download_status
            TLS_VERIFY ON)
        list(GET _entt_download_status 0 _entt_download_code)
        if(NOT _entt_download_code EQUAL 0)
            list(GET _entt_download_status 1 _entt_download_message)
            file(REMOVE "${_entt_header}")
            message(WARNING
                "Could not download EnTT (${_entt_download_message}). "
                "Set GLSCRIPT_ENTT_INCLUDE_DIR to a directory containing entt/entt.hpp "
                "to build the EnTT-based demos.")
            set(_entt_include_dir "")
        endif()
    endif()
elseif(NOT EXISTS "${GLSCRIPT_ENTT_INCLUDE_DIR}/entt/entt.hpp")
    message(WARNING
        "GLSCRIPT_ENTT_INCLUDE_DIR does not contain entt/entt.hpp; skipping EnTT demos.")
    set(_entt_include_dir "")
endif()

if(_entt_include_dir)
    if(GLSCRIPT_BUILD_ENTT_EXAMPLE)
        add_executable(entt_demo
            examples/entt/entt_demo.cpp
            examples/engine_bindings.cpp)

        target_include_directories(entt_demo PRIVATE
            ${_entt_include_dir}
            ${CMAKE_CURRENT_SOURCE_DIR}/examples)

        target_compile_definitions(entt_demo PRIVATE
            GLSCRIPT_ENTT_DEMO_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/examples/entt/gravity.gl")

        target_link_libraries(entt_demo PRIVATE glscript glscript_debug)

        # Headless run: the physics is fully scripted, so this exercises the
        # runtime end to end without opening a window.
        glscript_link_raylib(entt_demo)
        add_test(NAME entt_demo COMMAND entt_demo --headless 240)
    endif()

    add_executable(sandbox_demo
        examples/sandbox/sandbox_demo.cpp
        examples/engine_bindings.cpp)

    target_include_directories(sandbox_demo PRIVATE
        ${_entt_include_dir}
        ${CMAKE_CURRENT_SOURCE_DIR}/examples)

    target_compile_definitions(sandbox_demo PRIVATE
        GLSCRIPT_SANDBOX_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/examples/sandbox/sandbox.gl")

    target_link_libraries(sandbox_demo PRIVATE glscript glscript_debug)

    glscript_link_raylib(sandbox_demo)

    add_test(NAME sandbox_demo COMMAND sandbox_demo --headless 300)

    if(GLSCRIPT_BUILD_FPS_EXAMPLE)
        add_executable(fps_demo
            examples/fps/fps_demo.cpp
            examples/engine_bindings.cpp)

        target_include_directories(fps_demo PRIVATE
            ${_entt_include_dir}
            ${CMAKE_CURRENT_SOURCE_DIR}/examples)

        target_compile_definitions(fps_demo PRIVATE
            GLSCRIPT_FPS_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/examples/fps/fps.gl")

        target_link_libraries(fps_demo PRIVATE glscript glscript_debug)

        glscript_link_raylib(fps_demo)
        glscript_link_jolt(fps_demo)

        # Headless run: fixed timestep + single-threaded job system keep the
        # physics deterministic, so the fork-cascade assertions hold under ctest.
        add_test(NAME fps_demo COMMAND fps_demo --headless 400)
    endif()
endif()

add_executable(glscript_tests
    tests/test_main.cpp
    examples/engine_bindings.cpp)
target_include_directories(glscript_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_compile_definitions(glscript_tests PRIVATE
    GLSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(glscript_tests PRIVATE glscript)

add_test(NAME glscript_tests COMMAND glscript_tests)

add_executable(debugger_test
    tests/debugger_test.cpp
    examples/engine_bindings.cpp)

target_include_directories(debugger_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_link_libraries(debugger_test PRIVATE glscript glscript_debug)

add_test(NAME debugger_test COMMAND debugger_test)

add_executable(wave_demo
    examples/effects/wave_demo.cpp
    examples/engine_bindings.cpp)

target_include_directories(wave_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_compile_definitions(wave_demo PRIVATE
    GLSCRIPT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(wave_demo PRIVATE glscript glscript_debug)

add_test(NAME wave_demo
         COMMAND wave_demo ${CMAKE_CURRENT_SOURCE_DIR}/examples/effects/wave.gl)
