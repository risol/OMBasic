function(om_apply_sanitizer target profile)
    if(profile STREQUAL "none")
        return()
    endif()

    if(WIN32 OR MSVC)
        message(FATAL_ERROR
            "Sanitizer profile '${profile}' requires a Linux or macOS Clang/GCC build. "
            "Use the Linux container for sanitizer runs.")
    endif()

    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "Sanitizers require Clang or GCC.")
    endif()

    if(profile STREQUAL "address-undefined")
        set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
    elseif(profile STREQUAL "thread")
        set(flags -fsanitize=thread -fno-omit-frame-pointer -O1 -g)
    elseif(profile STREQUAL "memory")
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "MemorySanitizer requires Clang.")
        endif()
        set(flags -fsanitize=memory -fno-omit-frame-pointer -O1 -g)
    else()
        message(FATAL_ERROR "Unknown OM_SANITIZER profile: ${profile}")
    endif()

    target_compile_options(${target} INTERFACE ${flags})
    target_link_options(${target} INTERFACE ${flags})
endfunction()
