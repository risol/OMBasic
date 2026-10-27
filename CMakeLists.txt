cmake_minimum_required(VERSION 3.25)

project(OMBasic VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckIncludeFile)
check_include_file(stdckdint.h OM_HAVE_STDCKDINT_H)
if(NOT OM_HAVE_STDCKDINT_H)
    message(FATAL_ERROR
        "The selected C23 compiler and C library must provide <stdckdint.h>. "
        "Select a supported toolchain or add a documented compatibility shim.")
endif()

option(OM_ARENA_DEBUG_POISON "Poison released arena bytes in debug builds" ON)
set(OM_SANITIZER "none" CACHE STRING "Sanitizer profile: none, address-undefined, thread, memory")
set_property(CACHE OM_SANITIZER PROPERTY STRINGS none address-undefined thread memory)

include(CTest)
include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

add_library(ombasic_warnings INTERFACE)
om_apply_project_warnings(ombasic_warnings)

add_library(ombasic_sanitizers INTERFACE)
om_apply_sanitizer(ombasic_sanitizers "${OM_SANITIZER}")

add_library(ombasic_foundation STATIC
    src/foundation/arena/arena.c
    src/foundation/hash/fingerprint.c
    src/foundation/hash/u64_map.c
    src/foundation/memory/memory.c
    src/foundation/status.c
    src/foundation/string/string_view.c
    src/foundation/utf8/utf8.c
    src/foundation/vector/vector.c
)
add_library(ombasic::foundation ALIAS ombasic_foundation)
target_include_directories(ombasic_foundation
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_foundation
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)
if(OM_ARENA_DEBUG_POISON)
    target_compile_definitions(ombasic_foundation PRIVATE OM_ARENA_DEBUG_POISON=1)
endif()

add_library(ombasic_source STATIC
    src/source/file_loader/file_loader.c
    src/source/import_path/import_path.c
    src/source/source_text/source_map.c
    src/source/source_text/source_text.c
    src/source/span.c
)
add_library(ombasic::source ALIAS ombasic_source)
target_include_directories(ombasic_source
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_source
    PUBLIC
        ombasic::foundation
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_syntax STATIC
    src/syntax/lexer/lexer.c
    src/syntax/name_rules/name_rules.c
    src/syntax/parser/parser.c
)
add_library(ombasic::syntax ALIAS ombasic_syntax)
target_include_directories(ombasic_syntax
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_syntax
    PUBLIC
        ombasic::source
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_ffi STATIC
    src/ffi/abi_type.c
    src/ffi/c_string.c
    src/ffi/dynamic_library.c
    src/ffi/native_call.c
    src/ffi/omi_parser.c
    src/ffi/omi_project_loader/omi_project.c
)
add_library(ombasic::ffi ALIAS ombasic_ffi)
set(OM_LIBFFI_ROOT "" CACHE PATH "Root of the libffi development installation")
if(NOT OM_LIBFFI_ROOT AND WIN32
   AND EXISTS "C:/msys64/mingw64/include/ffi.h")
    set(OM_LIBFFI_ROOT "C:/msys64/mingw64")
endif()
find_path(OM_LIBFFI_INCLUDE_DIR
    NAMES ffi.h
    HINTS "${OM_LIBFFI_ROOT}" ENV LIBFFI_ROOT
    PATH_SUFFIXES include
)
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    find_file(OM_LIBFFI_LIBRARY
        NAMES libffi.dll.a
        HINTS "${OM_LIBFFI_ROOT}" ENV LIBFFI_ROOT
        PATH_SUFFIXES lib
    )
else()
    find_library(OM_LIBFFI_LIBRARY
        NAMES ffi libffi
        HINTS "${OM_LIBFFI_ROOT}" ENV LIBFFI_ROOT
        PATH_SUFFIXES lib
    )
endif()
if(NOT OM_LIBFFI_INCLUDE_DIR OR NOT OM_LIBFFI_LIBRARY)
    message(FATAL_ERROR
        "libffi development files are required for typed Native calls. "
        "Install libffi or set OM_LIBFFI_ROOT to its installation prefix.")
endif()
message(STATUS "Using libffi headers: ${OM_LIBFFI_INCLUDE_DIR}")
message(STATUS "Using libffi library: ${OM_LIBFFI_LIBRARY}")
# Keep the libffi headers in a small include directory.  Adding the complete
# MinGW include tree to clang-cl's search path lets MinGW windows.h shadow the
# Windows SDK headers and conflict with Clang's MSVC intrinsics.
set(OM_LIBFFI_COMPAT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/libffi-include")
file(MAKE_DIRECTORY "${OM_LIBFFI_COMPAT_INCLUDE_DIR}")
configure_file(
    "${OM_LIBFFI_INCLUDE_DIR}/ffi.h"
    "${OM_LIBFFI_COMPAT_INCLUDE_DIR}/ffi.h"
    COPYONLY
)
configure_file(
    "${OM_LIBFFI_INCLUDE_DIR}/ffitarget.h"
    "${OM_LIBFFI_COMPAT_INCLUDE_DIR}/ffitarget.h"
    COPYONLY
)
target_include_directories(ombasic_ffi
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(ombasic_ffi SYSTEM PRIVATE
        "${OM_LIBFFI_COMPAT_INCLUDE_DIR}"
)
target_link_libraries(ombasic_ffi
    PUBLIC
        ombasic::foundation
        ombasic::source
        ombasic::syntax
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
        "${OM_LIBFFI_LIBRARY}"
        ${CMAKE_DL_LIBS}
)

add_library(ombasic_distribution STATIC
    src/distribution/connection_handshake.c
    src/distribution/handshake.c
    src/distribution/node_id.c
    src/distribution/protocol.c
    src/distribution/serializer.c
    src/distribution/transport.c
)
add_library(ombasic::distribution ALIAS ombasic_distribution)
target_include_directories(ombasic_distribution
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_distribution
    PUBLIC
        ombasic::foundation
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)
if(WIN32)
    target_link_libraries(ombasic_distribution PRIVATE ws2_32 bcrypt)
endif()

add_library(ombasic_module_graph STATIC
    src/module_graph/module_loader.c
    src/module_graph/module_graph.c
)
add_library(ombasic::module_graph ALIAS ombasic_module_graph)
target_include_directories(ombasic_module_graph
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_module_graph
    PUBLIC
        ombasic::source
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_semantic STATIC
    src/semantic/actor_protocol/actor_protocol.c
    src/semantic/effect/effect_analysis.c
    src/semantic/name_resolver/name_resolver.c
    src/semantic/semantic_model/semantic_analysis.c
    src/semantic/serialization/serializable_analysis.c
    src/semantic/type_inference/type_inference.c
    src/semantic/type_inference/type_store.c
)
add_library(ombasic::semantic ALIAS ombasic_semantic)
target_include_directories(ombasic_semantic
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_semantic
    PUBLIC
        ombasic::syntax
        ombasic::module_graph
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_bytecode STATIC
    src/bytecode/builder/bytecode_builder.c
    src/bytecode/emitter/bytecode_emitter.c
    src/bytecode/format/bytecode_format.c
    src/bytecode/verifier/bytecode_verifier.c
)
add_library(ombasic::bytecode ALIAS ombasic_bytecode)
target_include_directories(ombasic_bytecode
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode"
)
target_link_libraries(ombasic_bytecode
    PUBLIC
        ombasic::semantic
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_runtime STATIC
    src/runtime/heap/heap.c
    src/runtime/system_actor/blocking_pool.c
    src/runtime/system_actor/timer_service.c
    src/runtime/distribution/vm_distribution.c
    src/runtime/value/value.c
    src/runtime/vm/vm.c
)
add_library(ombasic::runtime ALIAS ombasic_runtime)
target_include_directories(ombasic_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_runtime
    PUBLIC
        ombasic::bytecode
        ombasic::distribution
        ombasic::ffi
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
        Threads::Threads
)
target_include_directories(ombasic_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor"
)

add_library(ombasic_compiler STATIC
    src/compiler/native_binding/native_binding.c
)
add_library(ombasic::compiler ALIAS ombasic_compiler)
target_include_directories(ombasic_compiler
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_compiler
    PUBLIC
        ombasic::runtime
        ombasic::semantic
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)

add_library(ombasic_tooling STATIC
    src/tooling/diagnostics_json.c
    src/tooling/formatter.c
    src/tooling/interface_cache.c
    src/tooling/type_format.c
)
add_library(ombasic::tooling ALIAS ombasic_tooling)
target_include_directories(ombasic_tooling
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(ombasic_tooling
    PUBLIC
        ombasic::syntax
        ombasic::semantic
    PRIVATE
        ombasic_warnings
        ombasic_sanitizers
)
add_executable(ombasic src/tooling/main.c)
target_sources(ombasic PRIVATE src/tooling/check.c)
file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" OM_PYTHON_EXECUTABLE_CMAKE)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/ombasic_bind.py"
     OM_BIND_SCRIPT_PATH_CMAKE)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/ombasic_lsp.py"
     OM_LSP_SCRIPT_PATH_CMAKE)
target_compile_definitions(ombasic PRIVATE
    "OM_PYTHON_EXECUTABLE=\"${OM_PYTHON_EXECUTABLE_CMAKE}\""
    "OM_BIND_SCRIPT_PATH=\"${OM_BIND_SCRIPT_PATH_CMAKE}\""
    "OM_LSP_SCRIPT_PATH=\"${OM_LSP_SCRIPT_PATH_CMAKE}\""
)
target_link_libraries(ombasic
    PRIVATE
        ombasic::tooling
        ombasic::compiler
        ombasic::ffi
        ombasic_warnings
        ombasic_sanitizers
)

add_custom_target(architecture-check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/architecture_check/check.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)

if(BUILD_TESTING)
    if(NOT DEFINED CMAKE_CXX_COMPILER
       AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
       AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH
            "C++ compiler matching the clang-cl C toolchain")
    endif()
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    add_executable(ombasic_foundation_tests
        tests/unit/foundation_tests.c
        tests/unit/test_main.c
    )
    target_include_directories(ombasic_foundation_tests PRIVATE tests/unit)
    target_link_libraries(ombasic_foundation_tests
        PRIVATE
            ombasic::foundation
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME foundation COMMAND ombasic_foundation_tests)
    add_executable(ombasic_source_tests
        tests/source/file_loader_tests.c
        tests/source/source_test_main.c
        tests/source/source_tests.c
    )
    target_include_directories(ombasic_source_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_source_tests PRIVATE
        "OM_SOURCE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/05_identifiers\""
    )
    target_link_libraries(ombasic_source_tests
        PRIVATE
            ombasic::source
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME source COMMAND ombasic_source_tests)
    add_executable(ombasic_lexer_tests
        tests/syntax/lexer_test_main.c
        tests/syntax/lexer_tests.c
    )
    target_include_directories(ombasic_lexer_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_lexer_tests PRIVATE
        "OM_LEXER_CONFORMANCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/05_identifiers\""
        "OM_LEXER_LITERAL_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/lexer\""
    )
    target_link_libraries(ombasic_lexer_tests
        PRIVATE
            ombasic::syntax
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME lexer COMMAND ombasic_lexer_tests)
    add_executable(ombasic_parser_tests
        tests/syntax/parser_test_main.c
        tests/syntax/parser_tests.c
    )
    target_include_directories(ombasic_parser_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_parser_tests PRIVATE
        "OM_PARSER_GOLDEN_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/parser\""
    )
    target_link_libraries(ombasic_parser_tests
        PRIVATE
            ombasic::syntax
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME parser COMMAND ombasic_parser_tests)
    add_executable(ombasic_module_graph_tests
        tests/module_graph/module_graph_test_main.c
        tests/module_graph/module_graph_tests.c
        tests/module_graph/module_loader_tests.c
    )
    target_include_directories(ombasic_module_graph_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_module_graph_tests PRIVATE
        "OM_MODULE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/module_graph/fixtures\""
    )
    target_link_libraries(ombasic_module_graph_tests
        PRIVATE
            ombasic::module_graph
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME module_graph COMMAND ombasic_module_graph_tests)
    add_executable(ombasic_semantic_tests
        tests/semantic/name_resolver_test_main.c
        tests/semantic/name_resolver_tests.c
        tests/semantic/type_store_tests.c
    )
    target_include_directories(ombasic_semantic_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_semantic_tests PRIVATE
        "OM_SEMANTIC_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic/fixtures\""
    )
    target_link_libraries(ombasic_semantic_tests
        PRIVATE
            ombasic::semantic
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME semantic_name_resolution COMMAND ombasic_semantic_tests)
    add_executable(ombasic_bytecode_tests
        tests/bytecode/bytecode_test_main.c
        tests/bytecode/bytecode_tests.c
    )
    target_include_directories(ombasic_bytecode_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_bytecode_tests PRIVATE
        "OM_BYTECODE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode/fixtures\""
    )
    target_link_libraries(ombasic_bytecode_tests
        PRIVATE
            ombasic::bytecode
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME bytecode COMMAND ombasic_bytecode_tests)
    add_executable(ombasic_runtime_tests
        tests/runtime/gc_tests.c
        tests/runtime/vm_test_main.c
        tests/runtime/vm_tests.c
    )
    target_include_directories(ombasic_runtime_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    )
    target_compile_definitions(ombasic_runtime_tests PRIVATE
        "OM_VM_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/basic\""
        "OM_VM_ACTOR_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/actors\""
        "OM_VM_SYSTEM_ACTOR_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/system_actors\""
        "OM_VM_NATIVE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/native_ffi\""
        "OM_VM_NATIVE_CALLBACK_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/native_callback\""
        "OM_VM_NATIVE_BLOCKING_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/native_blocking\""
        "OM_VM_DISTRIBUTION_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime/fixtures/distribution\""
    )
    target_link_libraries(ombasic_runtime_tests
        PRIVATE
            ombasic::compiler
            Threads::Threads
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME runtime_vm COMMAND ombasic_runtime_tests)
    add_executable(ombasic_blocking_pool_tests
        tests/runtime/blocking_pool_tests.c
    )
    target_include_directories(ombasic_blocking_pool_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor"
    )
    target_link_libraries(ombasic_blocking_pool_tests
        PRIVATE
            ombasic::runtime
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME blocking_pool COMMAND ombasic_blocking_pool_tests)
    add_executable(ombasic_timer_service_tests
        tests/runtime/timer_service_tests.c
    )
    target_include_directories(ombasic_timer_service_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor"
    )
    target_link_libraries(ombasic_timer_service_tests
        PRIVATE
            ombasic::runtime
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME timer_service COMMAND ombasic_timer_service_tests)
    add_executable(ombasic_ffi_tests
        tests/ffi/omi_parser_tests.c
    )
    add_library(ombasic_native_fixture SHARED
        tests/ffi/native_fixture.c
    )
    set_target_properties(ombasic_native_fixture PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_link_libraries(ombasic_native_fixture PRIVATE Threads::Threads)
    add_dependencies(ombasic_ffi_tests ombasic_native_fixture)
    add_dependencies(ombasic_runtime_tests ombasic_native_fixture)
    target_compile_definitions(ombasic_runtime_tests PRIVATE
        "OM_NATIVE_FIXTURE_PATH=\"$<TARGET_FILE:ombasic_native_fixture>\""
    )
    target_compile_definitions(ombasic_ffi_tests PRIVATE
        "OM_NATIVE_FIXTURE_PATH=\"$<TARGET_FILE:ombasic_native_fixture>\""
        "OM_OMI_PROJECT_TEST_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/fixtures/native_project\""
    )
    add_library(ombasic_cpp_native_fixture SHARED
        tests/ffi/cpp_wrapper.cpp
    )
    set_target_properties(ombasic_cpp_native_fixture PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    if(MSVC)
        target_compile_options(ombasic_cpp_native_fixture PRIVATE
            /W4 /WX /EHsc
        )
    else()
        target_compile_options(ombasic_cpp_native_fixture PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
    endif()
    add_dependencies(ombasic_ffi_tests ombasic_cpp_native_fixture)
    target_compile_definitions(ombasic_ffi_tests PRIVATE
        "OM_CPP_NATIVE_FIXTURE_PATH=\"$<TARGET_FILE:ombasic_cpp_native_fixture>\""
    )
    target_link_libraries(ombasic_ffi_tests
        PRIVATE
            ombasic::ffi
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME native_omi COMMAND ombasic_ffi_tests)
    add_executable(ombasic_distribution_tests
        tests/distribution/protocol_tests.c
    )
    target_link_libraries(ombasic_distribution_tests
        PRIVATE
            ombasic::distribution
            Threads::Threads
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME distribution_protocol COMMAND ombasic_distribution_tests)
    add_test(NAME architecture
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/architecture_check/check.py"
                --root "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME conformance_manifest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/validate_manifest.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/manifest.json"
    )
    add_executable(ombasic_tooling_tests
        tests/tooling/formatter_tests.c
    )
    target_link_libraries(ombasic_tooling_tests
        PRIVATE
            ombasic::tooling
            ombasic_warnings
            ombasic_sanitizers
    )
    add_test(NAME tooling_formatter COMMAND ombasic_tooling_tests)
    add_test(NAME tooling_cli
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/cli_tests.py"
                "$<TARGET_FILE:ombasic>"
                "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endif()
