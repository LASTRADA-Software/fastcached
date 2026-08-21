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
