function(om_apply_sanitizer target profile)
    if(profile STREQUAL "none")
        return()
    endif()

    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC.")
    endif()

    if(profile STREQUAL "address-undefined")
        if(WIN32 AND NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
            message(FATAL_ERROR
                "Windows address/undefined sanitizers require clang-cl with the MSVC ABI.")
        endif()
        if(WIN32)
            set(flags -fsanitize=address,undefined /Oy- /O1 /Zi)
            execute_process(
                COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
                OUTPUT_VARIABLE OM_CLANG_RESOURCE_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY
            )
            set(OM_WINDOWS_SANITIZER_LIB_DIR
                "${OM_CLANG_RESOURCE_DIR}/lib/windows")
            foreach(runtime IN ITEMS
                    clang_rt.asan_dynamic-x86_64
                    clang_rt.asan_dynamic_runtime_thunk-x86_64
                    clang_rt.ubsan_standalone-x86_64)
                find_library(OM_SANITIZER_${runtime}_LIB
                    NAMES "${runtime}"
                    PATHS "${OM_WINDOWS_SANITIZER_LIB_DIR}"
                    NO_DEFAULT_PATH
                    REQUIRED
                )
                target_link_libraries(${target} INTERFACE
                    "${OM_SANITIZER_${runtime}_LIB}")
            endforeach()
        else()
            set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
        endif()
    elseif(profile STREQUAL "thread")
        if(WIN32 OR MSVC)
            message(FATAL_ERROR "ThreadSanitizer is not supported on Windows.")
        endif()
        set(flags -fsanitize=thread -fno-omit-frame-pointer -O1 -g)
    elseif(profile STREQUAL "memory")
        if(WIN32 OR MSVC)
            message(FATAL_ERROR "MemorySanitizer is not supported on Windows.")
        endif()
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "MemorySanitizer requires Clang.")
        endif()
        set(flags -fsanitize=memory -fno-omit-frame-pointer -O1 -g)
    else()
        message(FATAL_ERROR "Unknown OM_SANITIZER profile: ${profile}")
    endif()

    target_compile_options(${target} INTERFACE ${flags})
    if(NOT WIN32)
        target_link_options(${target} INTERFACE ${flags})
    endif()
endfunction()
