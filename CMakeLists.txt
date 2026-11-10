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
    src/value.cpp
    src/compiler.cpp
    src/runtime.cpp)

target_include_directories(glscript
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(glscript PUBLIC cxx_std_20)

add_executable(glc
    tools/glc.cpp
    examples/engine_bindings.cpp)

target_include_directories(glc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/examples)

target_link_libraries(glc PRIVATE glscript)

enable_testing()

option(GLSCRIPT_BUILD_ENTT_EXAMPLE "Build the EnTT integration example" ON)

if(GLSCRIPT_BUILD_ENTT_EXAMPLE)
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
                    "to build the entt_demo example.")
                set(_entt_include_dir "")
            endif()
        endif()
    elseif(NOT EXISTS "${GLSCRIPT_ENTT_INCLUDE_DIR}/entt/entt.hpp")
        message(WARNING
            "GLSCRIPT_ENTT_INCLUDE_DIR does not contain entt/entt.hpp; skipping entt_demo.")
        set(_entt_include_dir "")
    endif()

    if(_entt_include_dir)
        add_executable(entt_demo
            examples/entt/entt_demo.cpp)

        target_include_directories(entt_demo PRIVATE
            ${_entt_include_dir}
            ${CMAKE_CURRENT_SOURCE_DIR}/examples)

        target_compile_definitions(entt_demo PRIVATE
            GLSCRIPT_ENTT_DEMO_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/examples/entt/entt_demo.gl")

        target_link_libraries(entt_demo PRIVATE glscript)

        add_test(NAME entt_demo COMMAND entt_demo)
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
