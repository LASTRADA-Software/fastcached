# SPDX-License-Identifier: Apache-2.0
# Sanitizers.cmake - Clang/GCC sanitizer support
#
# Options:
#   ENABLE_SANITIZER_ADDRESS         - Enable AddressSanitizer (detects memory errors)
#   ENABLE_SANITIZER_UNDEFINED       - Enable UndefinedBehaviorSanitizer
#   ENABLE_SANITIZER_THREAD          - Enable ThreadSanitizer (detects data races)
#
# Note: ASan and TSan cannot be combined.

option(ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)

set(SANITIZER_COMPILE_OPTIONS "")
set(SANITIZER_LINK_OPTIONS "")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(SANITIZER_FLAGS "")

    if(ENABLE_SANITIZER_ADDRESS)
        list(APPEND SANITIZER_FLAGS "address")
    endif()

    if(ENABLE_SANITIZER_UNDEFINED)
        list(APPEND SANITIZER_FLAGS "undefined")
    endif()

    if(ENABLE_SANITIZER_THREAD)
        if(ENABLE_SANITIZER_ADDRESS)
            message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
        endif()
        list(APPEND SANITIZER_FLAGS "thread")
    endif()

    if(SANITIZER_FLAGS)
        string(REPLACE ";" "," SANITIZER_FLAGS_STR "${SANITIZER_FLAGS}")
        message(STATUS "[Sanitizers] Enabling: ${SANITIZER_FLAGS_STR}")

        # Assembled in LOCAL variables and only then published, which is not style.
        # Under CMP0126 NEW -- which any `cmake_minimum_required(3.21)` or later
        # selects -- `set(X ... CACHE INTERNAL ...)` no longer removes a normal
        # variable of the same name, so the empty `X` set at the top of this file
        # keeps shadowing the cache. A later `list(APPEND X ...)` then appends to ""
        # rather than to the flags, and `add_compile_options` receives the appended
        # item alone: a build carrying `-fno-sanitize-recover=undefined` and no
        # `-fsanitize=` at all, on a COMPLETELY FRESH build directory, while the
        # cache reports the options ON and the line above prints "Enabling". Every
        # signal an author would check says yes and nothing is instrumented.
        # Measured on this repository, where it meant the sanitizer preset had never
        # once run a sanitizer -- locally or in CI.
        set(_sanitizer_compile_options
            -fsanitize=${SANITIZER_FLAGS_STR}
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
        )
        set(_sanitizer_link_options -fsanitize=${SANITIZER_FLAGS_STR})

        if(ENABLE_SANITIZER_UNDEFINED)
            list(APPEND _sanitizer_compile_options -fno-sanitize-recover=undefined)
        endif()

        set(SANITIZER_COMPILE_OPTIONS ${_sanitizer_compile_options} CACHE INTERNAL "Sanitizer compile options")
        set(SANITIZER_LINK_OPTIONS ${_sanitizer_link_options} CACHE INTERNAL "Sanitizer link options")
        # Published as normal variables too, so a consumer reading them in this scope
        # cannot pick up a stale shadow for the same reason.
        set(SANITIZER_COMPILE_OPTIONS ${_sanitizer_compile_options})
        set(SANITIZER_LINK_OPTIONS ${_sanitizer_link_options})

        add_compile_options(${_sanitizer_compile_options})
        add_link_options(${_sanitizer_link_options})
    else()
        message(STATUS "[Sanitizers] None enabled")
    endif()
else()
    message(STATUS "[Sanitizers] Sanitizers require Clang or GCC. Skipping.")
endif()
