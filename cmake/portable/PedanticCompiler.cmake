# SPDX-License-Identifier: Apache-2.0
include(CheckCXXCompilerFlag)
function(try_add_compile_options FLAG)
    # Remove leading - or / from the flag name.
    string(REGEX REPLACE "^[-/]" "" name ${FLAG})
    # Deletes any ':' because it's invalid variable names.
    string(REGEX REPLACE ":" "" name ${name})
    check_cxx_compiler_flag(${FLAG} ${name})
    if(${name})
        message(STATUS "Adding compiler flag: ${FLAG}.")
        # Use generator expression to apply C++ flags only to C++ files
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${FLAG}>)
    else()
        message(STATUS "Adding compiler flag: ${FLAG} failed.")
    endif()

    # If the optional argument passed, store the result there.
    if(ARGV1)
        set(${ARGV1} ${name} PARENT_SCOPE)
    endif()
endfunction()

option(PEDANTIC_COMPILER "Compile the project with almost all warnings turned on." OFF)
option(PEDANTIC_COMPILER_WERROR "Enables -Werror to force warnings to be treated as errors." OFF)

# Always show diagnostics in colored output.
try_add_compile_options(-fdiagnostics-color=always)

if(${PEDANTIC_COMPILER})
    if("${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}" STREQUAL "MSVC")
        message(STATUS "Enabling pedantic compiler options: yes (MSVC)")
        try_add_compile_options(/W4)
        try_add_compile_options(/permissive-)
        try_add_compile_options(/Zc:__cplusplus)
        #try_add_compile_options(/Zc:enumTypes)
        #try_add_compile_options(/Zc:externConstexpr)
        try_add_compile_options(/Zc:inline)
        #try_add_compile_options(/Zc:templateScope)
        # if(${PEDANTIC_COMPILER_WERROR})
        #    try_add_compile_options(/WX)
        # endif()
    elseif(("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU") OR ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
        message(STATUS "Enabling pedantic compiler options: yes (Clang/GCC)")
        # TODO: check https://github.com/lefticus/cppbestpractices/blob/master/02-Use_the_Tools_Available.md#compilers
        try_add_compile_options(-Qunused-arguments)
        try_add_compile_options(-Wall)
        try_add_compile_options(-Wconversion)
        try_add_compile_options(-Wduplicate-enum)
        try_add_compile_options(-Wduplicated-cond)
        try_add_compile_options(-Wextra)
        try_add_compile_options(-Wextra-semi)
        try_add_compile_options(-Wfinal-dtor-non-final-class)
        try_add_compile_options(-Wimplicit-fallthrough)
        try_add_compile_options(-Wlogical-op)
        try_add_compile_options(-Wmissing-declarations)
        try_add_compile_options(-Wnewline-eof)
        try_add_compile_options(-Wno-unknown-attributes)
        try_add_compile_options(-Wno-unknown-pragmas)
        if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
            # -Wdangling-reference will generate false positives on recent GCC versions.
            # See https://gcc.gnu.org/git/gitweb.cgi?p=gcc.git;h=6b927b1297e66e26e62e722bf15c921dcbbd25b9
            try_add_compile_options(-Wno-dangling-reference)
        else()
            try_add_compile_options(-Wdangling-reference)
        endif()
        try_add_compile_options(-Wnull-dereference)
        try_add_compile_options(-Wpessimizing-move)
        try_add_compile_options(-Wredundant-move)
        #try_add_compile_options(-Wsign-conversion)
        try_add_compile_options(-Wsuggest-destructor-override)
        try_add_compile_options(-pedantic)

        # The suppressions, under the SAME condition as the flags that make them
        # necessary -- which is `PEDANTIC_COMPILER`, not `PEDANTIC_COMPILER_WERROR`.
        #
        # `PEDANTIC_COMPILER_WERROR` decides whether a warning is FATAL. It must not
        # decide whether a warning EXISTS. These four sat inside the `WERROR` block
        # while `-pedantic` and `-Wmissing-declarations` were added outside it, so
        # every preset that inherits `PEDANTIC_COMPILER=ON` from `base` and leaves
        # `WERROR` off got the warnings and none of the suppressions -- that is
        # `clang-coverage`, `clang-asan-ubsan`, `clang-tsan` and `clang-tracy`, four
        # of the twelve.
        #
        # Measured on 0db96dc8, each in its own fresh build directory against
        # `/usr/bin/clang++` with no compiler-cache launcher: **7049
        # `-Wc2y-extensions` warnings across 195 translation units, the same number
        # in all four**. Nothing failed, because those presets have `WERROR` off --
        # which is why this survived: the only place it becomes visible is a
        # clang-tidy database from a build directory configured that way, where it
        # reads as a stale cache rather than as a rule
        # ([#454](https://github.com/LASTRADA-Software/fastcached/issues/454),
        # [#611](https://github.com/LASTRADA-Software/fastcached/issues/611)).
        #
        # `ctest -R pedantic-suppressions` is what holds this, by including this
        # file at both `WERROR` settings and refusing any difference that is not
        # purely about fatality. Copying the suppressions out while leaving them in
        # would NOT close it: two conditions naming one diagnostic is the defect.
        #
        # That guard lives in the repository this file came from. A project that
        # vendored this module has the flags and not the check, so the paragraph
        # above is the whole of what travels with it.

        # Don't complain here. That's needed for bitpacking (codepoint_properties) in libunicode dependency.
        try_add_compile_options(-Wno-c++20-extensions)

        # __COUNTER__ is a long-standing vendor extension used by LIGHTWEIGHT_SQL_RELEASE;
        # Clang 22 newly classifies it as a C2y extension. This is the one that fired:
        # every `TEST_CASE` expands to it, so the count is a function of how many
        # tests there are.
        try_add_compile_options(-Wno-c2y-extensions)

        # Not sure how to work around these.
        try_add_compile_options(-Wno-class-memaccess)

        # TODO: Should be addressed.
        #
        # This one cancels the `-Wmissing-declarations` added a few lines up, and
        # deliberately: the pair is how "we want this warning, we are not ready for
        # it" is spelled, so turning it on again is deleting ONE line rather than
        # adding one. That was already true for every `WERROR` preset; what changed
        # is that the other four now agree, and they lose nothing by it -- measured
        # zero `-Wmissing-declarations` diagnostics across all four before the move.
        try_add_compile_options(-Wno-missing-declarations)

        if(${PEDANTIC_COMPILER_WERROR})
            try_add_compile_options(-Werror)

            # Fatality only. `-Wno-error=X` says "keep X visible, do not fail on
            # it", which is a statement about `-Werror` and belongs here; the
            # `-Wno-X` above says "never show me X", which is not.
            #
            # These FOUR -- and only these four, each PAIRED with a `-Wno-X` above
            # -- are belt-and-braces rather than load-bearing. The scope is the
            # whole claim: the two GNU-gated rows further down are UNPAIRED and are
            # what lets GCC 16 build this tree at all, so "these are inert" read as
            # covering the family is how one of them gets deleted and takes
            # `gate-gcc-release` red.
            #
            # A diagnostic disabled by `-Wno-X` can never be an error, so each of
            # these four matters only on a compiler where the `-Wno-X` probe fails
            # while its own succeeds. Measured on clang 22.1.8 and GCC 16.2.1, the
            # two halves of every PAIR succeed and fail TOGETHER -- clang rejects
            # both spellings of `class-memaccess`, GCC rejects both of
            # `c2y-extensions` -- so on those two compilers these four are
            # currently inert. Kept because that is a property of two compilers
            # rather than of the flags.
            try_add_compile_options(-Wno-error=c++20-extensions)
            try_add_compile_options(-Wno-error=c2y-extensions)
            try_add_compile_options(-Wno-error=class-memaccess)
            try_add_compile_options(-Wno-error=missing-declarations)

            # The two below are NOT belt-and-braces: they are load-bearing, and
            # deliberately NOT paired with a `-Wno-` above. The diagnostics stay
            # VISIBLE -- they are worth reading -- they merely stop being fatal.
            #
            # GCC ONLY, because only GCC produces them. Ungated, clang would lose
            # `-Werror` on two diagnostics that name real bugs, to unblock a
            # compiler clang is not. The gate costs nothing: `pedantic-suppressions`
            # still reports both rows under its GNU persona.
            #
            # Both fire inside libstdc++'s own headers, from inlining, on code that
            # has no defect to fix. GCC 16.2.1 reports them and g++-13/g++-14 do not
            # ([#805](https://github.com/LASTRADA-Software/fastcached/issues/805)):
            #
            #   * `-Warray-bounds=` in `EncodeCompileValue`
            #     (`src/FastCache/CompileCache/CompileValue.cpp`), out of
            #     `stl_uninitialized.h`.
            #   * `-Wmaybe-uninitialized` in
            #     `src/FastCache/Server/AdminHttpServer_test.cpp`, inside libstdc++'s
            #     own `_Any_data` union, when an `AdminRoute` is copied out of an
            #     `initializer_list`.
            #
            # This is here rather than at the source because THREE separate source
            # formulations of `EncodeCompileValue` were each measured to silence one
            # compiler and invent a fresh `-Werror` diagnostic on another, which is
            # strictly worse than the warning:
            #
            #   * `reserve()` before the first insert -- silences GCC 16's
            #     `-Warray-bounds=`, produces `-Wfree-nonheap-object` out of
            #     `new_allocator.h` on g++-14. This one SHIPPED, as 166880b6, and
            #     took `Linux-gcc-release` red; bc207960 reverted it.
            #   * grow-then-copy in place of the `insert` -- GCC 16 moves the same
            #     `-Warray-bounds=` onto the `memset` `resize` emits.
            #   * constructing `out` at its full header+blob size -- silences GCC 16
            #     AND g++-14, produces `-Wnull-dereference` out of `stl_algobase.h`
            #     on g++-13.
            #
            # The belief GCC 16 holds is about the five-byte buffer the header alone
            # leaves, not about any particular write, so every spelling of "append to
            # this vector" meets it. `out.insert(out.end(), first, last)` is
            # unconditionally correct and there is no way to write it wrong.
            #
            # A `-Wno-error=` row is additive and cannot invent a diagnostic on a
            # compiler that is not tested here -- clang and MSVC also build these two
            # translation units -- which a fourth source rewrite could.
            if("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU")
                try_add_compile_options(-Wno-error=array-bounds)
                try_add_compile_options(-Wno-error=maybe-uninitialized)
            endif()
        endif()
    else()
        message(STATUS "Enabling pedantic compiler options: unsupported platform")
    endif()
else()
    message(STATUS "Enabling pedantic compiler options: no")
endif()
