# House C standard, build side (c-coding-standard skill, reference/tooling.md section 1).
#
# Pins the language to C11 with GNU extensions - never the compiler default, which moves between
# compiler releases - and defines one INTERFACE target, house_warnings, carrying the Tier A warning
# set. Every library and test target links it PRIVATE. Blanket -Werror is a CI-only switch
# (XMSG_WERROR); the four -Werror= lines are real errors on every compiler and stay on everywhere.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT TARGET house_warnings)
    add_library(house_warnings INTERFACE)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(house_warnings INTERFACE
            -Wall -Wextra
            -Wformat=2 -Wimplicit-fallthrough
            -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
            -Wshadow -Wpointer-arith -Wundef -Wvla -Wwrite-strings
            -Werror=implicit-function-declaration
            -Werror=incompatible-pointer-types
            -Werror=int-conversion
            -Werror=format-security)
        if(XMSG_WERROR)
            target_compile_options(house_warnings INTERFACE -Werror)
        endif()
    elseif(MSVC)
        # MSVC is not a supported compiler for the library (the targets are GCC and Clang), but a
        # host build must not fail to configure on a machine that only has it.
        target_compile_options(house_warnings INTERFACE /W4)
        if(XMSG_WERROR)
            target_compile_options(house_warnings INTERFACE /WX)
        endif()
    endif()
endif()
