cmake_minimum_required(VERSION 3.25)

if(DEFINED CMAKE_C_COMPILER AND NOT DEFINED CMAKE_CXX_COMPILER)
    get_filename_component(OM_C_DRIVER_NAME "${CMAKE_C_COMPILER}" NAME)
    get_filename_component(OM_C_DRIVER_DIRECTORY "${CMAKE_C_COMPILER}" DIRECTORY)
    if(OM_C_DRIVER_NAME MATCHES "^clang-cl(\\.exe)?$")
        set(CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH
            "Use the configured Clang toolchain for C++ ABI fixtures.")
    elseif(OM_C_DRIVER_NAME MATCHES "^clang(\\.exe)?$")
        find_program(OM_CXX_DRIVER NAMES clang++ clang++.exe
            HINTS "${OM_C_DRIVER_DIRECTORY}")
        if(OM_CXX_DRIVER)
            set(CMAKE_CXX_COMPILER "${OM_CXX_DRIVER}" CACHE FILEPATH
                "Use the configured Clang toolchain for C++ ABI fixtures.")
        endif()
    endif()
endif()

project(OMBasic VERSION 0.1.0 LANGUAGES C CXX)

if(WIN32)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
       OR NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC"
       OR NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR
            "Windows builds require clang-cl.exe with the MSVC frontend and ABI.")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
       OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR
            "Linux builds require the clang driver with the Linux system ABI.")
    endif()
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckIncludeFile)
include(CheckCSourceCompiles)
check_include_file(stdckdint.h OM_HAVE_STDCKDINT_H)
check_c_source_compiles([=[
#include <stddef.h>
int main(void)
{
    size_t result = 0;
    return __builtin_add_overflow((size_t)1, (size_t)2, &result)
        || __builtin_mul_overflow((size_t)2, (size_t)3, &result);
}
]=] OM_HAVE_CHECKED_SIZE_BUILTINS)
if(NOT OM_HAVE_STDCKDINT_H)
    if(NOT OM_HAVE_CHECKED_SIZE_BUILTINS
       AND NOT MSVC AND NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        message(FATAL_ERROR
            "The selected C compiler must provide <stdckdint.h>, checked-size builtins, or MSVC checked arithmetic.")
    elseif(OM_HAVE_CHECKED_SIZE_BUILTINS)
        message(STATUS
            "<stdckdint.h> is unavailable; using checked-size compiler builtins.")
    else()
        message(STATUS
            "MSVC does not provide <stdckdint.h>; using the documented checked-size shim.")
    endif()
endif()

option(OM_ARENA_DEBUG_POISON "Poison released arena bytes in debug builds" ON)
set(OM_SANITIZER "none" CACHE STRING "Sanitizer profile: none, address-undefined, thread, memory")
set_property(CACHE OM_SANITIZER PROPERTY STRINGS none address-undefined thread memory)
option(OM_BUILD_FUZZERS
    "Build Clang libFuzzer targets (requires OM_SANITIZER=address-undefined)"
    OFF)

include(CTest)
include(cmake/Warnings.cmake)
include(cmake/Sanitizers.cmake)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_package(LLVM REQUIRED CONFIG)
if(NOT LLVM_PACKAGE_VERSION MATCHES "^23\\.")
    message(FATAL_ERROR
        "OMBasic requires LLVM major 23; found ${LLVM_PACKAGE_VERSION}.")
endif()
find_program(OM_LLVM_STRIP NAMES llvm-strip.exe llvm-strip
    HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)

if(BUILD_TESTING)
    if(WIN32)
        find_program(OM_LLVM_DEBUG_INFO_TOOL NAMES llvm-pdbutil.exe llvm-pdbutil
            HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(OM_LLVM_DEBUG_INFO_TOOL NAMES llvm-dwarfdump
            HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    endif()
endif()

if(WIN32)
    find_library(OM_LLVM_C_LIBRARY
        NAMES LLVM-C LLVM-C.lib
        HINTS ${LLVM_LIBRARY_DIRS}
        REQUIRED
    )
    set(OM_LLVM_LINK_LIBRARIES "${OM_LLVM_C_LIBRARY}")
else()
    find_program(OM_LLVM_CONFIG NAMES llvm-config
        HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    execute_process(
        COMMAND "${OM_LLVM_CONFIG}" --link-static --libs --system-libs
        OUTPUT_VARIABLE OM_LLVM_LINK_FLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE OM_LLVM_CONFIG_RESULT
    )
    if(NOT OM_LLVM_CONFIG_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not query LLVM system libraries.")
    endif()
    separate_arguments(OM_LLVM_LINK_LIBRARIES UNIX_COMMAND
        "${OM_LLVM_LINK_FLAGS}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND OM_LLVM_LINK_LIBRARIES stdc++)
    endif()
endif()
if(WIN32)
    find_file(OM_LLVM_C_DLL
        NAMES LLVM-C.dll
        HINTS "${LLVM_TOOLS_BINARY_DIR}"
        REQUIRED
    )
endif()

set(OM_LLVM_TARGET_TRIPLE "" CACHE STRING
    "LLVM target triple; defaults to the configured C compiler target")
if(OM_LLVM_TARGET_TRIPLE STREQUAL "")
    if(CMAKE_C_COMPILER_TARGET)
        set(OM_LLVM_TARGET_TRIPLE "${CMAKE_C_COMPILER_TARGET}")
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
            OUTPUT_VARIABLE OM_LLVM_TARGET_TRIPLE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE OM_LLVM_TRIPLE_RESULT
        )
        if(NOT OM_LLVM_TRIPLE_RESULT EQUAL 0)
            message(FATAL_ERROR "Could not determine the C compiler target triple.")
        endif()
    endif()
endif()
if(OM_LLVM_TARGET_TRIPLE STREQUAL "")
    if(NOT MSVC AND NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
        message(FATAL_ERROR
            "Set OM_LLVM_TARGET_TRIPLE for this compiler toolchain.")
    endif()
endif()

if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    find_program(OM_LLD_LINKER NAMES lld-link.exe lld-link
        HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
else()
    find_program(OM_LLD_LINKER NAMES ld.lld ld.lld.exe
        HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
endif()
get_filename_component(OM_LLD_LINKER_DIRECTORY "${OM_LLD_LINKER}" DIRECTORY)
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND OM_LLD_LINKER)
    add_link_options(-fuse-ld=lld)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_link_options("-B${OM_LLD_LINKER_DIRECTORY}")
    endif()
endif()

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
target_include_directories(ombasic_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_foundation PRIVATE ombasic_warnings ombasic_sanitizers)
if(OM_HAVE_STDCKDINT_H)
    target_compile_definitions(ombasic_foundation PRIVATE OM_HAVE_STDCKDINT_H=1)
endif()
if(OM_HAVE_CHECKED_SIZE_BUILTINS)
    target_compile_definitions(ombasic_foundation PRIVATE
        OM_HAVE_CHECKED_SIZE_BUILTINS=1)
endif()
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
target_include_directories(ombasic_source PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_source
    PUBLIC ombasic::foundation
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_syntax STATIC
    src/syntax/lexer/lexer.c
    src/syntax/name_rules/name_rules.c
    src/syntax/parser/parser.c
)
add_library(ombasic::syntax ALIAS ombasic_syntax)
target_include_directories(ombasic_syntax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_syntax
    PUBLIC ombasic::source
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_module_graph STATIC
    src/module_graph/module_loader.c
    src/module_graph/module_graph.c
)
add_library(ombasic::module_graph ALIAS ombasic_module_graph)
target_include_directories(ombasic_module_graph PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_module_graph
    PUBLIC ombasic::source
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_native_metadata STATIC
    src/native/metadata/omi_parser.c
    src/native/metadata/omi_project.c
)
add_library(ombasic::native_metadata ALIAS ombasic_native_metadata)
target_include_directories(ombasic_native_metadata PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_native_metadata
    PUBLIC ombasic::source
    PRIVATE ombasic::syntax ombasic_warnings ombasic_sanitizers
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
target_include_directories(ombasic_semantic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_semantic
    PUBLIC ombasic::syntax ombasic::module_graph ombasic::native_metadata
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_hir STATIC src/ir/hir/hir.c)
add_library(ombasic::hir ALIAS ombasic_hir)
target_include_directories(ombasic_hir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_hir
    PUBLIC ombasic::semantic
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_specialization STATIC src/ir/specialize/specialize.c)
add_library(ombasic::specialization ALIAS ombasic_specialization)
target_include_directories(ombasic_specialization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_specialization
    PUBLIC ombasic::hir
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_mir STATIC
    src/ir/mir/lower_hir.c
    src/ir/mir/mir.c
)
add_library(ombasic::mir ALIAS ombasic_mir)
target_include_directories(ombasic_mir PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_mir
    PUBLIC ombasic::specialization
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_target STATIC src/target/target.c)
add_library(ombasic::target ALIAS ombasic_target)
target_include_directories(ombasic_target PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_target
    PUBLIC ombasic::foundation
    PRIVATE ombasic_warnings ombasic_sanitizers
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
target_include_directories(ombasic_distribution PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_distribution
    PUBLIC ombasic::foundation
    PRIVATE ombasic_warnings ombasic_sanitizers
)
if(WIN32)
    target_link_libraries(ombasic_distribution PRIVATE ws2_32 bcrypt)
endif()

add_library(ombasic_runtime STATIC
    src/runtime/actor/actor.c
    src/runtime/actor/mailbox.c
    src/runtime/actor/watch.c
    src/runtime/callback/callback.c
    src/runtime/core/context.c
    src/runtime/core/object.c
    src/runtime/core/value.c
    src/runtime/data/list.c
    src/runtime/data/map.c
    src/runtime/data/native_pointer.c
    src/runtime/data/record.c
    src/runtime/data/string.c
    src/runtime/fault/fault.c
    src/runtime/gc/heap.c
    src/runtime/gc/roots.c
    src/runtime/gc/trace.c
    src/runtime/scheduler/scheduler.c
    src/runtime/system_actor/system_actor.c
)
add_library(ombasic::runtime ALIAS ombasic_runtime)
target_include_directories(ombasic_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ombasic_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/actor"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor"
)
target_link_libraries(ombasic_runtime
    PUBLIC ombasic::foundation
    PRIVATE Threads::Threads ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_runtime_services STATIC
    src/runtime/system_actor/blocking_pool.c
    src/runtime/system_actor/timer_service.c
)
add_library(ombasic::runtime_services ALIAS ombasic_runtime_services)
target_include_directories(ombasic_runtime_services
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor"
)
target_link_libraries(ombasic_runtime_services
    PUBLIC ombasic::foundation
    PRIVATE ombasic_warnings ombasic_sanitizers Threads::Threads
)

add_library(ombasic_runtime_distribution STATIC
    src/runtime/distribution/actor_distribution.c
)
add_library(ombasic::runtime_distribution ALIAS ombasic_runtime_distribution)
target_include_directories(ombasic_runtime_distribution
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_runtime_distribution
    PUBLIC ombasic::runtime ombasic::distribution ombasic::foundation
    PRIVATE Threads::Threads ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_tooling STATIC
    src/tooling/diagnostics_json.c
    src/tooling/formatter.c
    src/tooling/interface_cache.c
    src/tooling/type_format.c
)
add_library(ombasic::tooling ALIAS ombasic_tooling)
target_include_directories(ombasic_tooling PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_tooling
    PUBLIC ombasic::syntax ombasic::semantic
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_backend_llvm STATIC
    src/backend/llvm/abi/aapcs64.c
    src/backend/llvm/abi/darwin_arm64.c
    src/backend/llvm/abi/sysv_amd64.c
    src/backend/llvm/abi/win64.c
    src/backend/llvm/actors.c
    src/backend/llvm/backend.c
    src/backend/llvm/callbacks.c
    src/backend/llvm/control_flow.c
    src/backend/llvm/debug_info.c
    src/backend/llvm/descriptors.c
    src/backend/llvm/emit.c
    src/backend/llvm/function.c
    src/backend/llvm/gc_roots.c
    src/backend/llvm/module.c
    src/backend/llvm/native_ffi.c
    src/backend/llvm/runtime_calls.c
    src/backend/llvm/types.c
)
add_library(ombasic::backend_llvm ALIAS ombasic_backend_llvm)
target_include_directories(ombasic_backend_llvm
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/llvm/abi"
            ${LLVM_INCLUDE_DIRS}
)
if(NOT WIN32)
    target_link_directories(ombasic_backend_llvm PUBLIC ${LLVM_LIBRARY_DIRS})
endif()
target_link_libraries(ombasic_backend_llvm
    PUBLIC ombasic::mir ombasic::target
    PRIVATE ${OM_LLVM_LINK_LIBRARIES} ombasic_warnings ombasic_sanitizers
)
target_compile_definitions(ombasic_backend_llvm PRIVATE
    "OM_LLVM_TARGET_TRIPLE=\"${OM_LLVM_TARGET_TRIPLE}\""
)
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_definitions(ombasic_backend_llvm PRIVATE
        "$<$<CONFIG:Debug>:OM_LLVM_OPT_LEVEL=0>"
        "$<$<CONFIG:Release>:OM_LLVM_OPT_LEVEL=2>"
        "$<$<CONFIG:RelWithDebInfo>:OM_LLVM_OPT_LEVEL=2>"
        "$<$<CONFIG:MinSizeRel>:OM_LLVM_OPT_LEVEL=2>"
    )
elseif(CMAKE_BUILD_TYPE STREQUAL "Release"
       OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
       OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    target_compile_definitions(ombasic_backend_llvm PRIVATE
        OM_LLVM_OPT_LEVEL=2)
else()
    target_compile_definitions(ombasic_backend_llvm PRIVATE
        OM_LLVM_OPT_LEVEL=0)
endif()

add_library(ombasic_compiler STATIC src/compiler/pipeline.c)
add_library(ombasic::compiler ALIAS ombasic_compiler)
target_include_directories(ombasic_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_compiler
    PUBLIC ombasic::mir
    PRIVATE ombasic::backend_llvm ombasic_warnings ombasic_sanitizers
)

add_library(ombasic_toolchain STATIC src/toolchain/resources.c)
add_library(ombasic::toolchain ALIAS ombasic_toolchain)
target_include_directories(ombasic_toolchain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_toolchain
    PUBLIC ombasic::foundation
    PRIVATE ombasic_warnings ombasic_sanitizers
)
target_compile_definitions(ombasic_toolchain PRIVATE
    "OM_TOOLCHAIN_TARGET_TRIPLE=\"${OM_LLVM_TARGET_TRIPLE}\"")

add_library(ombasic_linker STATIC src/linker/linker.c)
add_library(ombasic::linker ALIAS ombasic_linker)
target_include_directories(ombasic_linker PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ombasic_linker
    PUBLIC ombasic::foundation ombasic::toolchain
    PRIVATE ombasic_warnings ombasic_sanitizers
)

add_executable(ombasic src/tooling/main.c src/tooling/check.c)
target_link_libraries(ombasic
    PRIVATE ombasic::tooling ombasic::native_metadata ombasic::compiler
            ombasic::linker
            ombasic_warnings ombasic_sanitizers
)
if(WIN32)
    target_link_options(ombasic PRIVATE
        "-Xlinker" "/PDBALTPATH:ombasic.pdb")
endif()
add_dependencies(ombasic ombasic_runtime ombasic_runtime_services
                 ombasic_runtime_distribution ombasic_distribution
                 ombasic_foundation)
if(OM_SANITIZER STREQUAL "address-undefined")
    set(OM_NATIVE_LINK_SANITIZER_FLAG "-fsanitize=address,undefined")
elseif(OM_SANITIZER STREQUAL "thread")
    set(OM_NATIVE_LINK_SANITIZER_FLAG "-fsanitize=thread")
else()
    set(OM_NATIVE_LINK_SANITIZER_FLAG "")
endif()
target_compile_definitions(ombasic_linker PRIVATE
    "OM_NATIVE_LINK_SANITIZER_FLAG=\"${OM_NATIVE_LINK_SANITIZER_FLAG}\""
)
if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    find_program(OM_CLANG_CL NAMES clang-cl.exe
        HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
    set(OM_NATIVE_LINK_DRIVER "clang-cl")
    set(OM_NATIVE_LINK_DRIVER_STYLE_MSVC 1)

    set(OM_MSVC_LIBRARY_DIRECTORY "" CACHE PATH
        "Directory containing the target MSVC runtime libraries")
    if(CMAKE_VS_PLATFORM_NAME MATCHES "ARM64|arm64"
       OR CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64"
       OR CMAKE_C_COMPILER_TARGET MATCHES "aarch64|arm64")
        set(OM_WINDOWS_TARGET_ARCH "arm64")
    elseif(CMAKE_VS_PLATFORM_NAME MATCHES "Win32|x86"
           OR CMAKE_GENERATOR_PLATFORM MATCHES "Win32|x86")
        set(OM_WINDOWS_TARGET_ARCH "x86")
    else()
        set(OM_WINDOWS_TARGET_ARCH "x64")
    endif()

    if(OM_MSVC_LIBRARY_DIRECTORY STREQUAL "")
        set(OM_MSVC_TOOLSET_ROOT "$ENV{VCToolsInstallDir}")
        if(OM_MSVC_TOOLSET_ROOT STREQUAL ""
           AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            get_filename_component(OM_MSVC_COMPILER_ARCH_DIR
                "${CMAKE_C_COMPILER}" DIRECTORY)
            get_filename_component(OM_MSVC_COMPILER_HOST_DIR
                "${OM_MSVC_COMPILER_ARCH_DIR}" DIRECTORY)
            get_filename_component(OM_MSVC_BIN_DIR
                "${OM_MSVC_COMPILER_HOST_DIR}" DIRECTORY)
            get_filename_component(OM_MSVC_TOOLSET_ROOT
                "${OM_MSVC_BIN_DIR}" DIRECTORY)
        endif()
        if(OM_MSVC_TOOLSET_ROOT STREQUAL "")
            set(OM_MSVC_VS_INSTANCE "${CMAKE_GENERATOR_INSTANCE}")
            if(OM_MSVC_VS_INSTANCE STREQUAL "")
                set(OM_MSVC_VS_INSTANCE "$ENV{VSINSTALLDIR}")
            endif()
            if(OM_MSVC_VS_INSTANCE STREQUAL "")
                find_program(OM_VSWHERE_EXECUTABLE NAMES vswhere.exe
                    HINTS
                        "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
                )
                if(OM_VSWHERE_EXECUTABLE)
                    execute_process(
                        COMMAND "${OM_VSWHERE_EXECUTABLE}" -latest -products "*"
                            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                        OUTPUT_VARIABLE OM_MSVC_VS_INSTANCE
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE OM_VSWHERE_RESULT
                    )
                    if(NOT OM_VSWHERE_RESULT EQUAL 0)
                        set(OM_MSVC_VS_INSTANCE "")
                    endif()
                endif()
            endif()
            if(NOT OM_MSVC_VS_INSTANCE STREQUAL "")
                file(GLOB OM_MSVC_TOOLSET_CANDIDATES LIST_DIRECTORIES true
                    "${OM_MSVC_VS_INSTANCE}/VC/Tools/MSVC/*")
                if(OM_MSVC_TOOLSET_CANDIDATES)
                    list(SORT OM_MSVC_TOOLSET_CANDIDATES
                        COMPARE NATURAL ORDER DESCENDING)
                    list(GET OM_MSVC_TOOLSET_CANDIDATES 0 OM_MSVC_TOOLSET_ROOT)
                endif()
            endif()
        endif()
        if(NOT OM_MSVC_TOOLSET_ROOT STREQUAL "")
            set(OM_MSVC_LIBRARY_DIRECTORY
                "${OM_MSVC_TOOLSET_ROOT}/lib/${OM_WINDOWS_TARGET_ARCH}")
        endif()
    endif()
    if(NOT EXISTS "${OM_MSVC_LIBRARY_DIRECTORY}/libcmt.lib")
        message(FATAL_ERROR
            "Could not find the MSVC runtime libraries. Set "
            "OM_MSVC_LIBRARY_DIRECTORY to the MSVC toolset lib/${OM_WINDOWS_TARGET_ARCH} directory.")
    endif()

    set(OM_WINDOWS_KITS_LIB_HINTS)
    set(OM_WINDOWS_KITS_LIB_BASES
        "$ENV{WindowsSdkDir}Lib"
        "$ENV{CMAKE_WINDOWS_KITS_10_DIR}/Lib"
        "C:/Program Files (x86)/Windows Kits/10/Lib"
        "F:/Program Files (x86)/Windows Kits/10/Lib"
    )
    foreach(OM_WINDOWS_KITS_LIB_BASE IN LISTS OM_WINDOWS_KITS_LIB_BASES)
        if(OM_WINDOWS_KITS_LIB_BASE AND IS_DIRECTORY "${OM_WINDOWS_KITS_LIB_BASE}")
            file(GLOB OM_WINDOWS_KITS_VERSION_DIRS LIST_DIRECTORIES true
                "${OM_WINDOWS_KITS_LIB_BASE}/*")
            list(APPEND OM_WINDOWS_KITS_LIB_HINTS
                ${OM_WINDOWS_KITS_VERSION_DIRS})
        endif()
    endforeach()
    foreach(OM_WINDOWS_KITS_VERSION
            "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
            "$ENV{WindowsSDKLibVersion}"
            "$ENV{WindowsSDKVersion}")
        if(NOT OM_WINDOWS_KITS_VERSION STREQUAL "")
            list(APPEND OM_WINDOWS_KITS_LIB_HINTS
                "$ENV{WindowsSdkDir}Lib/${OM_WINDOWS_KITS_VERSION}"
                "$ENV{CMAKE_WINDOWS_KITS_10_DIR}/Lib/${OM_WINDOWS_KITS_VERSION}"
                "C:/Program Files (x86)/Windows Kits/10/Lib/${OM_WINDOWS_KITS_VERSION}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES OM_WINDOWS_KITS_LIB_HINTS)
    find_path(OM_WINDOWS_KITS_LIBRARY_DIRECTORY
        NAMES "ucrt/${OM_WINDOWS_TARGET_ARCH}/libucrt.lib"
        HINTS ${OM_WINDOWS_KITS_LIB_HINTS}
        NO_DEFAULT_PATH
    )
    if(NOT OM_WINDOWS_KITS_LIBRARY_DIRECTORY
       OR NOT EXISTS "${OM_WINDOWS_KITS_LIBRARY_DIRECTORY}/um/${OM_WINDOWS_TARGET_ARCH}/kernel32.lib")
        message(FATAL_ERROR
            "Could not find ${OM_WINDOWS_TARGET_ARCH} Windows SDK libraries for clang-cl/LLD. "
            "Set OM_WINDOWS_KITS_LIBRARY_DIRECTORY to the Windows Kits Lib/<version> directory.")
    endif()
    set(OM_WINDOWS_UCRT_LIBRARY_DIRECTORY
        "${OM_WINDOWS_KITS_LIBRARY_DIRECTORY}/ucrt/${OM_WINDOWS_TARGET_ARCH}")
    set(OM_WINDOWS_UM_LIBRARY_DIRECTORY
        "${OM_WINDOWS_KITS_LIBRARY_DIRECTORY}/um/${OM_WINDOWS_TARGET_ARCH}")
else()
    set(OM_NATIVE_LINK_DRIVER "clang")
    set(OM_NATIVE_LINK_DRIVER_STYLE_MSVC 0)
    set(OM_MSVC_LIBRARY_DIRECTORY "")
    set(OM_WINDOWS_UCRT_LIBRARY_DIRECTORY "")
    set(OM_WINDOWS_UM_LIBRARY_DIRECTORY "")
endif()
set(OM_NATIVE_LINK_DRIVER_CMAKE "${OM_NATIVE_LINK_DRIVER}")
set(OM_LLD_LINKER_DIRECTORY_CMAKE "")
target_compile_definitions(ombasic_linker PRIVATE
    "OM_NATIVE_LINK_DRIVER_STYLE_MSVC=${OM_NATIVE_LINK_DRIVER_STYLE_MSVC}"
    "OM_NATIVE_LINK_DRIVER=\"${OM_NATIVE_LINK_DRIVER_CMAKE}\""
    "OM_LLD_LINKER_DIRECTORY=\"${OM_LLD_LINKER_DIRECTORY_CMAKE}\""
)
if(OM_LLD_LINKER OR OM_NATIVE_LINK_DRIVER_STYLE_MSVC)
    target_compile_definitions(ombasic_linker PRIVATE OM_NATIVE_USE_LLD=1)
else()
    target_compile_definitions(ombasic_linker PRIVATE OM_NATIVE_USE_LLD=0)
endif()
if(WIN32)
    add_custom_command(TARGET ombasic POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${OM_LLVM_C_DLL}" "$<TARGET_FILE_DIR:ombasic>"
        VERBATIM
    )
endif()

set(OM_BUILD_RESOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
set(OM_BUILD_RUNTIME_DIRECTORY
    "${OM_BUILD_RESOURCE_ROOT}/lib/ombasic/${OM_LLVM_TARGET_TRIPLE}")
set(OM_BUILD_TOOL_DIRECTORY "${OM_BUILD_RESOURCE_ROOT}/share/ombasic/tools")
add_custom_target(ombasic_build_resources
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${OM_BUILD_TOOL_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ombasic_runtime>" "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ombasic_runtime_services>" "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ombasic_runtime_distribution>" "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ombasic_distribution>" "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ombasic_foundation>" "${OM_BUILD_RUNTIME_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ombasic_bind.py"
            "${OM_BUILD_TOOL_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/ombasic_lsp.py"
            "${OM_BUILD_TOOL_DIRECTORY}"
    DEPENDS ombasic_runtime ombasic_runtime_services
            ombasic_runtime_distribution ombasic_distribution
            ombasic_foundation
    VERBATIM
)
add_dependencies(ombasic ombasic_build_resources)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/share/ombasic")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/share/ombasic/target-manifest.json"
    "{\n  \"format_version\": 1,\n  \"ombasic_version\": \"${PROJECT_VERSION}\",\n  \"targets\": {\n    \"${OM_LLVM_TARGET_TRIPLE}\": {\n      \"runtime_dir\": \"lib/ombasic/${OM_LLVM_TARGET_TRIPLE}\"\n    }\n  }\n}\n")
install(TARGETS ombasic RUNTIME DESTINATION bin)
install(TARGETS ombasic_runtime ombasic_runtime_services
                ombasic_runtime_distribution ombasic_distribution
                ombasic_foundation
        ARCHIVE DESTINATION "lib/ombasic/${OM_LLVM_TARGET_TRIPLE}")
configure_file(cmake/StripSdkArchives.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/StripSdkArchives.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/StripSdkArchives.cmake")
install(DIRECTORY include/ombasic DESTINATION include)
install(FILES tools/ombasic_bind.py tools/ombasic_lsp.py
        DESTINATION share/ombasic/tools)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/share/ombasic/target-manifest.json"
        DESTINATION share/ombasic)
if(WIN32)
    install(FILES "${OM_LLVM_C_DLL}" DESTINATION bin)
endif()

add_custom_target(architecture-check
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/architecture_check/check.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(ombasic_target_tests tests/target/target_tests.c)
    target_link_libraries(ombasic_target_tests PRIVATE
        ombasic::target ombasic_warnings ombasic_sanitizers)
    add_test(NAME target_model COMMAND ombasic_target_tests)
    add_executable(ombasic_native_abi_tests
        src/backend/llvm/abi/tests/native_abi_tests.c)
    target_include_directories(ombasic_native_abi_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/backend/llvm/abi"
        ${LLVM_INCLUDE_DIRS})
    target_link_libraries(ombasic_native_abi_tests PRIVATE
        ombasic::backend_llvm ombasic_warnings ombasic_sanitizers)
    add_test(NAME native_abi_aarch64 COMMAND ombasic_native_abi_tests)

    add_executable(ombasic_foundation_tests
        tests/unit/foundation_tests.c
        tests/unit/test_main.c
    )
    target_include_directories(ombasic_foundation_tests PRIVATE tests/unit)
    target_link_libraries(ombasic_foundation_tests PRIVATE
        ombasic::foundation ombasic_warnings ombasic_sanitizers)
    add_test(NAME foundation COMMAND ombasic_foundation_tests)
    if(OM_HAVE_CHECKED_SIZE_BUILTINS)
        add_executable(ombasic_checked_size_builtin_tests
            tests/foundation/checked_size_builtin_tests.c)
        target_include_directories(ombasic_checked_size_builtin_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/foundation/memory")
        target_compile_definitions(ombasic_checked_size_builtin_tests PRIVATE
            OM_HAVE_CHECKED_SIZE_BUILTINS=1)
        target_link_libraries(ombasic_checked_size_builtin_tests PRIVATE
            ombasic_warnings ombasic_sanitizers)
        add_test(NAME checked_size_builtin_fallback
            COMMAND ombasic_checked_size_builtin_tests)
    endif()

    add_executable(ombasic_source_tests
        tests/source/file_loader_tests.c
        tests/source/source_test_main.c
        tests/source/source_tests.c
    )
    target_include_directories(ombasic_source_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_source_tests PRIVATE
        "OM_SOURCE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/05_identifiers\"")
    if(MSVC)
        target_compile_definitions(ombasic_source_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(ombasic_source_tests PRIVATE
        ombasic::source ombasic_warnings ombasic_sanitizers)
    add_test(NAME source COMMAND ombasic_source_tests)

    add_executable(ombasic_lexer_tests
        tests/syntax/lexer_test_main.c
        tests/syntax/lexer_tests.c
    )
    target_include_directories(ombasic_lexer_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_lexer_tests PRIVATE
        "OM_LEXER_CONFORMANCE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/05_identifiers\"")
    target_compile_definitions(ombasic_lexer_tests PRIVATE
        "OM_LEXER_LITERAL_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/lexer\"")
    target_link_libraries(ombasic_lexer_tests PRIVATE
        ombasic::syntax ombasic_warnings ombasic_sanitizers)
    add_test(NAME lexer COMMAND ombasic_lexer_tests)

    add_executable(ombasic_parser_tests
        tests/syntax/parser_test_main.c
        tests/syntax/parser_tests.c
    )
    target_include_directories(ombasic_parser_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_parser_tests PRIVATE
        "OM_PARSER_GOLDEN_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/parser\"")
    if(MSVC)
        target_compile_definitions(ombasic_parser_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(ombasic_parser_tests PRIVATE
        ombasic::syntax ombasic_warnings ombasic_sanitizers)
    add_test(NAME parser COMMAND ombasic_parser_tests)

    add_executable(ombasic_module_graph_tests
        tests/module_graph/module_graph_test_main.c
        tests/module_graph/module_graph_tests.c
        tests/module_graph/module_loader_tests.c
    )
    target_include_directories(ombasic_module_graph_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_module_graph_tests PRIVATE
        "OM_MODULE_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/module_graph/fixtures\"")
    target_link_libraries(ombasic_module_graph_tests PRIVATE
        ombasic::module_graph ombasic_warnings ombasic_sanitizers)
    add_test(NAME module_graph COMMAND ombasic_module_graph_tests)

    add_executable(ombasic_semantic_tests
        tests/semantic/name_resolver_test_main.c
        tests/semantic/name_resolver_tests.c
        tests/semantic/type_store_tests.c
    )
    target_include_directories(ombasic_semantic_tests PRIVATE tests/unit)
    target_compile_definitions(ombasic_semantic_tests PRIVATE
        "OM_SEMANTIC_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic/fixtures\"")
    target_link_libraries(ombasic_semantic_tests PRIVATE
        ombasic::semantic ombasic_warnings ombasic_sanitizers)
    add_test(NAME semantic_name_resolution COMMAND ombasic_semantic_tests)

    add_executable(ombasic_hir_tests tests/hir/hir_tests.c)
    target_compile_definitions(ombasic_hir_tests PRIVATE
        "OM_HIR_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic/fixtures\""
        "OM_HIR_TEST_GOLDEN_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/hir/expected\"")
    if(MSVC)
        target_compile_definitions(ombasic_hir_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(ombasic_hir_tests PRIVATE
        ombasic::specialization ombasic_warnings ombasic_sanitizers)
    add_test(NAME hir COMMAND ombasic_hir_tests)

    add_executable(ombasic_mir_tests tests/mir/mir_tests.c)
    target_compile_definitions(ombasic_mir_tests PRIVATE
        "OM_MIR_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/mir/fixtures\""
        "OM_MIR_TEST_GOLDEN_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/mir/expected\"")
    if(MSVC)
        target_compile_definitions(ombasic_mir_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(ombasic_mir_tests PRIVATE
        ombasic::mir ombasic_warnings ombasic_sanitizers)
    add_test(NAME mir COMMAND ombasic_mir_tests)

    add_executable(ombasic_metadata_tests tests/native/metadata/metadata_tests.c)
    target_compile_definitions(ombasic_metadata_tests PRIVATE
        "OM_METADATA_TEST_FIXTURE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/native/metadata/fixtures\"")
    target_link_libraries(ombasic_metadata_tests PRIVATE
        ombasic::native_metadata ombasic_warnings ombasic_sanitizers)
    add_test(NAME native_metadata COMMAND ombasic_metadata_tests)

    add_executable(ombasic_blocking_pool_tests tests/runtime/blocking_pool_tests.c)
    target_include_directories(ombasic_blocking_pool_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor")
    target_link_libraries(ombasic_blocking_pool_tests PRIVATE
        ombasic::runtime_services ombasic_warnings ombasic_sanitizers)
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(ombasic_blocking_pool_tests PRIVATE
            /experimental:c11atomics)
    endif()
    add_test(NAME blocking_pool COMMAND ombasic_blocking_pool_tests)

    add_executable(ombasic_timer_service_tests tests/runtime/timer_service_tests.c)
    target_include_directories(ombasic_timer_service_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/system_actor")
    target_link_libraries(ombasic_timer_service_tests PRIVATE
        ombasic::runtime_services ombasic_warnings ombasic_sanitizers)
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(ombasic_timer_service_tests PRIVATE
            /experimental:c11atomics)
    endif()
    add_test(NAME timer_service COMMAND ombasic_timer_service_tests)

    add_executable(ombasic_native_runtime_tests tests/runtime/native_heap_tests.c)
    target_link_libraries(ombasic_native_runtime_tests PRIVATE
        ombasic::runtime ombasic::runtime_services
        ombasic_warnings ombasic_sanitizers)
    add_test(NAME native_runtime COMMAND ombasic_native_runtime_tests)

    add_library(ombasic_runtime_test_support STATIC
        tests/runtime/native_heap_tests.c)
    target_compile_definitions(ombasic_runtime_test_support PRIVATE
        OM_RUNTIME_TEST_SUPPORT_ONLY=1)
    target_link_libraries(ombasic_runtime_test_support PRIVATE
        ombasic::runtime ombasic::runtime_services
        ombasic_warnings ombasic_sanitizers)

    function(add_runtime_component_test target_name source_file test_name)
        add_executable(${target_name} "${source_file}")
        target_link_libraries(${target_name} PRIVATE
            ombasic_runtime_test_support ombasic_warnings ombasic_sanitizers)
        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    add_runtime_component_test(ombasic_runtime_fault_tests
        tests/runtime/fault_tests.c runtime_fault)
    add_runtime_component_test(ombasic_runtime_root_tests
        tests/runtime/root_tests.c runtime_roots)
    add_runtime_component_test(ombasic_runtime_gc_tests
        tests/runtime/gc_tests.c runtime_gc)
    add_runtime_component_test(ombasic_runtime_string_tests
        tests/runtime/string_tests.c runtime_string)
    add_runtime_component_test(ombasic_runtime_record_tests
        tests/runtime/record_tests.c runtime_record)
    add_runtime_component_test(ombasic_runtime_list_tests
        tests/runtime/list_tests.c runtime_list)
    add_runtime_component_test(ombasic_runtime_map_tests
        tests/runtime/map_tests.c runtime_map)
    add_runtime_component_test(ombasic_runtime_native_pointer_tests
        tests/runtime/native_pointer_tests.c runtime_native_pointer)

    add_executable(ombasic_native_actor_tests tests/runtime/native_actor_tests.c)
    target_link_libraries(ombasic_native_actor_tests PRIVATE
        ombasic::runtime ombasic::runtime_services
        ombasic_warnings ombasic_sanitizers)
    add_test(NAME native_actor_runtime COMMAND ombasic_native_actor_tests)
    add_test(NAME native_actor_stress
        COMMAND ombasic_native_actor_tests --stress)
    set_tests_properties(native_actor_stress PROPERTIES TIMEOUT 120)

    add_executable(ombasic_distribution_tests tests/distribution/protocol_tests.c)
    target_link_libraries(ombasic_distribution_tests PRIVATE
        ombasic::distribution Threads::Threads ombasic_warnings ombasic_sanitizers)
    add_test(NAME distribution_protocol COMMAND ombasic_distribution_tests)

    add_executable(ombasic_runtime_distribution_tests
        tests/distribution/runtime_node_tests.c)
    target_link_libraries(ombasic_runtime_distribution_tests PRIVATE
        ombasic::runtime_distribution ombasic::runtime_services
        Threads::Threads ombasic_warnings ombasic_sanitizers)
    add_test(NAME runtime_distribution_native
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/distribution/runtime_integration.py"
                "$<TARGET_FILE:ombasic_runtime_distribution_tests>")
    set_tests_properties(runtime_distribution_native PROPERTIES TIMEOUT 60)

    add_test(NAME architecture
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/architecture_check/check.py"
                --root "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME conformance_manifest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/validate_manifest.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/manifest.json")

    add_executable(ombasic_tooling_tests tests/tooling/formatter_tests.c)
    target_link_libraries(ombasic_tooling_tests PRIVATE
        ombasic::tooling ombasic_warnings ombasic_sanitizers)
    add_test(NAME tooling_formatter COMMAND ombasic_tooling_tests)
    add_library(ombasic_test_native_math STATIC tests/native/ffi/native_math.c)
    target_sources(ombasic_test_native_math PRIVATE
        tests/native/ffi/native_cpp_fixture.cpp)
    set_target_properties(ombasic_test_native_math PROPERTIES
        OUTPUT_NAME native_math)
    target_link_libraries(ombasic_test_native_math PRIVATE
        Threads::Threads ombasic_sanitizers)
    add_test(NAME tooling_cli
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/cli_tests.py"
                "$<TARGET_FILE:ombasic>"
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${OM_LLVM_DEBUG_INFO_TOOL}"
                "$<TARGET_FILE_DIR:ombasic_test_native_math>")
    add_test(NAME install_sdk_relocation
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/install_sdk_test.py"
                "${CMAKE_COMMAND}"
                "${CMAKE_BINARY_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "$<CONFIG>")
    set_tests_properties(install_sdk_relocation PROPERTIES TIMEOUT 180)
endif()

if(OM_BUILD_FUZZERS)
    if(WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "OM_BUILD_FUZZERS requires Clang on Linux or macOS.")
    endif()
    if(NOT OM_SANITIZER STREQUAL "address-undefined")
        message(FATAL_ERROR
            "OM_BUILD_FUZZERS requires -DOM_SANITIZER=address-undefined.")
    endif()
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "OM_BUILD_FUZZERS requires BUILD_TESTING=ON.")
    endif()

    target_compile_options(ombasic_syntax PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_native_metadata PRIVATE
        -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_source PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_distribution PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_hir PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_mir PRIVATE -fsanitize=fuzzer-no-link)
    target_compile_options(ombasic_backend_llvm PRIVATE
        -fsanitize=fuzzer-no-link)
    target_compile_definitions(ombasic_runtime_distribution PRIVATE
        OM_BUILD_FUZZERS=1)
    target_compile_options(ombasic_runtime_distribution PRIVATE
        -fsanitize=fuzzer-no-link)

    add_executable(ombasic_fuzz_syntax tests/fuzz/syntax_fuzzer.c)
    target_compile_options(ombasic_fuzz_syntax PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_syntax PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_syntax PRIVATE
        ombasic::syntax ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_metadata tests/fuzz/metadata_fuzzer.c)
    target_compile_options(ombasic_fuzz_metadata PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_metadata PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_metadata PRIVATE
        ombasic::native_metadata ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_import_path tests/fuzz/import_path_fuzzer.c)
    target_compile_options(ombasic_fuzz_import_path PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_import_path PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_import_path PRIVATE
        ombasic::source ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_distribution tests/fuzz/distribution_fuzzer.c)
    target_compile_options(ombasic_fuzz_distribution PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_distribution PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_distribution PRIVATE
        ombasic::distribution ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_hir_verifier
        tests/fuzz/hir_verifier_fuzzer.c)
    target_compile_options(ombasic_fuzz_hir_verifier PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_hir_verifier PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_hir_verifier PRIVATE
        ombasic::hir ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_mir_verifier
        tests/fuzz/mir_verifier_fuzzer.c)
    target_compile_options(ombasic_fuzz_mir_verifier PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_mir_verifier PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_mir_verifier PRIVATE
        ombasic::mir ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_distribution_serializer
        tests/fuzz/distribution_serializer_fuzzer.c)
    target_compile_options(ombasic_fuzz_distribution_serializer PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_distribution_serializer PRIVATE
        -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_distribution_serializer PRIVATE
        ombasic::distribution ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_llvm_backend
        tests/fuzz/llvm_backend_fuzzer.c)
    target_compile_options(ombasic_fuzz_llvm_backend PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_llvm_backend PRIVATE -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_llvm_backend PRIVATE
        ombasic::backend_llvm ombasic::mir ombasic_warnings ombasic_sanitizers)

    add_executable(ombasic_fuzz_native_descriptor_codec
        tests/fuzz/native_descriptor_codec_fuzzer.c)
    target_compile_options(ombasic_fuzz_native_descriptor_codec PRIVATE
        -fsanitize=fuzzer-no-link)
    target_link_options(ombasic_fuzz_native_descriptor_codec PRIVATE
        -fsanitize=fuzzer)
    target_link_libraries(ombasic_fuzz_native_descriptor_codec PRIVATE
        ombasic::runtime_distribution ombasic::runtime_services
        ombasic_warnings ombasic_sanitizers)
endif()
