function(om_apply_project_warnings target)
    if(MSVC)
        target_compile_options(${target} INTERFACE /W4 /WX)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} INTERFACE
                -Wconversion
                -Wshadow
                -Wstrict-prototypes
            )
        endif()
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
            -Wstrict-prototypes
            -Werror
        )
    else()
        message(FATAL_ERROR "Unsupported C compiler: ${CMAKE_C_COMPILER_ID}")
    endif()
endfunction()
