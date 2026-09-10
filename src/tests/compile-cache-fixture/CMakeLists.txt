# SPDX-License-Identifier: Apache-2.0
#
# The smallest project that exercises cmake/portable/CompileCache.cmake.
#
# The module decides what it decides during configure and leaves nothing behind
# to assert on afterwards, so the only way to test it is to configure something
# and read what it said. That something has to be a real project — the module
# runs after project() and its probe needs CMAKE_CXX_COMPILER — but it does not
# have to be more than this. No target is ever built.
#
# Driven by scripts/check-compile-cache-autoinstall.cmake, which configures this
# once per table row with different options and matches the output.
cmake_minimum_required(VERSION 3.28)
project(compile-cache-fixture LANGUAGES CXX)

if(NOT FASTCACHED_MODULE_DIR)
    message(FATAL_ERROR "FASTCACHED_MODULE_DIR must point at the directory holding CompileCache.cmake")
endif()

list(APPEND CMAKE_MODULE_PATH "${FASTCACHED_MODULE_DIR}")
include(CompileCache)

# A compile edge, and it has to come AFTER the include (#187).
#
# The module sets `CMAKE_CXX_COMPILER_LAUNCHER` as a normal variable rather than a
# cache entry, so `CMakeCache.txt` is silent about what it chose -- measured across
# eleven build trees on one workstation, unset in every one, including five that
# were demonstrably running sccache on every compile. What DOES carry the decision
# is the generated buildsystem, and a project with no target generates no compile
# edge to carry it. So a check that reads only the configure OUTPUT is asserting
# what the module SAID and never what it WIRED, and a module that printed the right
# sentence while wiring nothing would pass every row.
#
# Still never built: the stand-in launchers the caveat check points this at are
# `cmake` and `ctest`, which are real programs precisely so the module records
# them, and are not compilers.
add_library(compile-cache-fixture-probe OBJECT probe.cpp)
