# SPDX-License-Identifier: Apache-2.0
# Monocypher.cmake - the vendored Monocypher, built as a C++ static library (#178).
#
# `vendor/monocypher/` is a VERBATIM copy of the Monocypher 4.0.3 release (vendor/VENDOR.md),
# so nothing that states how it is built may live inside it: a CMakeLists.txt there would be
# a file upstream does not have, and a local change on the day of the import. This file is
# OURS, and it is the whole of the build glue.
#
# ## Why C++, in a project that declares `LANGUAGES CXX` only
#
# Monocypher is C99, and it also compiles as C++ by design: upstream's README says so and
# ships the switch this file sets. Enabling C for two translation units would put a second
# language's toolchain detection, flags and compiler-cache wiring into every configure,
# including `cmake/portable/CompileCache.cmake`, which asks `ENABLED_LANGUAGES` before it
# checks a flag precisely because a language nobody enabled is a hard error there. Measured
# before this file was written: both sources compile as C++23 with ZERO warnings under g++
# and clang++-22 at `-Wall -Wextra -Wpedantic`. MSVC was not measured here; the Windows CI
# legs are what say.
#
# `MONOCYPHER_CPP_NAMESPACE` is upstream's documented switch for exactly this build ("allows
# C++ users who compile Monocypher as C++ to wrap it in a namespace"). Without it the header
# assumes the library was compiled as C and wraps its declarations in `extern "C"`, which
# still links, but leaves `crypto_*` as C symbols in the global namespace where any other
# copy of Monocypher -- or libsodium, which uses the same prefix -- collides with them at
# link time. PUBLIC, because a consumer must spell the same namespace the objects carry.
#
# ## Why here, and what the ordering buys
#
# Included from the top-level CMakeLists.txt BEFORE `include(PedanticCompiler)` and
# `include(ClangTidy)`, which install -Werror, -Wconversion and CMAKE_CXX_CLANG_TIDY
# directory-wide and must not reach code that is not ours to fix -- the mechanism yaml-cpp,
# Catch2 and the vendored TUI already rely on. The target also clears `CXX_CLANG_TIDY`
# itself, because an operator's `-DCMAKE_CXX_CLANG_TIDY=` on the command line is a cache
# entry that exists before this file runs, and `vendor/CMakeLists.txt` says the same thing
# for the same reason.
#
# The sanitizers are the opposite case: they must reach this code, and being declared before
# `include(Sanitizers)` is exactly what keeps them off it. So the top-level CMakeLists.txt
# applies them target by target after the includes, to the targets this file names in
# `FASTCACHED_MONOCYPHER_TARGETS`, beside the ones `vendor/` declares. `ctest -R
# vendor-sanitized` reads the result in the objects.
#
# NOT gated on `FASTCACHED_BUILD_TUI`, and not added through `vendor/CMakeLists.txt`, which
# is: this is what every node's identity will be signed with (#178), so there is no build of
# this project that can go without it.
#
# ## Dependency-free, and it must stay so
#
# It depends on nothing -- not FastCache, not the standard library beyond `<stddef.h>` and
# `<stdint.h>`. That is what lets `fastcache-cc`'s `_fc_cc_core`, which deliberately does not
# link FastCache, link this later to verify a signed lease. The ONLY first-party code allowed
# to include a Monocypher header is `src/FastCache/Core/{Ed25519,X25519,Hkdf}.cpp`
# (`ctest -R crypto-seam`); everything else reaches the primitives through those.

set(_fcMonocypherRoot "${CMAKE_CURRENT_LIST_DIR}/../vendor/monocypher")
cmake_path(NORMAL_PATH _fcMonocypherRoot)

set(_fcMonocypherSources
    "${_fcMonocypherRoot}/src/monocypher.c"
    "${_fcMonocypherRoot}/src/optional/monocypher-ed25519.c"
)
foreach(_fcMonocypherSource IN LISTS _fcMonocypherSources)
    if(NOT EXISTS "${_fcMonocypherSource}")
        message(FATAL_ERROR
            "Monocypher: ${_fcMonocypherSource} is not in the tree. vendor/monocypher is a verbatim "
            "copy (vendor/VENDOR.md, \"Monocypher\"); restore it from the release tarball rather than "
            "editing this list, which would build a cryptographic library missing a source.")
    endif()
endforeach()

# Directory-scoped, like every source file property: this file is include()d, so the scope is
# the top-level directory, which is where the target below is declared.
set_source_files_properties(${_fcMonocypherSources} PROPERTIES LANGUAGE CXX)

add_library(fastcache-monocypher STATIC ${_fcMonocypherSources})

# SYSTEM, so a warning a consumer's flags would raise inside upstream's header is upstream's to
# fix and never this project's build failure. Both directories, because upstream installs the
# two headers side by side and `monocypher-ed25519.h` is spelled `<monocypher-ed25519.h>` by
# anything that consumes it that way.
target_include_directories(fastcache-monocypher SYSTEM PUBLIC
    "${_fcMonocypherRoot}/src"
    "${_fcMonocypherRoot}/src/optional"
)
target_compile_definitions(fastcache-monocypher PUBLIC MONOCYPHER_CPP_NAMESPACE=monocypher)
target_compile_features(fastcache-monocypher PUBLIC cxx_std_23)

# Monocypher's sources are UTF-8: measured, three non-ASCII bytes in the four files, one U+2014 in
# a comment in monocypher.c. Stated per TARGET, as vendor/CMakeLists.txt does for the TUI, so it
# is a claim about this code and not about every third-party source in the build.
if(MSVC)
    target_compile_options(fastcache-monocypher PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# No analyser on upstream's code, and no module scan: it declares and imports no module, and a
# scan over a `.c` file compiled as C++ is two edges of nothing.
set_target_properties(fastcache-monocypher PROPERTIES
    CXX_CLANG_TIDY ""
    CXX_SCAN_FOR_MODULES OFF
)

# What the sanitizer block in the top-level CMakeLists.txt instruments. Every target this file
# declares is on it; a second one added here without a row would be built with no sanitizer in
# every sanitizer configuration, which is the defect that block exists to close.
set(FASTCACHED_MONOCYPHER_TARGETS fastcache-monocypher)
