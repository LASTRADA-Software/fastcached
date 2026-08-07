# SPDX-License-Identifier: Apache-2.0
#
# Compiler-cache launcher selection.
#
# Two launchers are supported, in preference order:
#
#   1. fastcache-cc — this project's own launcher (src/apps/fastcache-cc).
#      Its entries are portable across checkout paths, so a CI runner and a
#      developer working from different directories share cache hits. It is
#      configured purely through the environment and caches nothing unless
#      FASTCACHE_ADDR / FASTCACHE_SRCROOT / FASTCACHE_BUILDTREE are all set, so
#      it is only selected once a daemon address is known; the two roots are
#      injected here via `cmake -E env` because CMake already knows them.
#   2. sccache — the usual third-party launcher, used when ours is unavailable
#      or unconfigured.
#
# Note this module runs long before src/apps is added, so the build can never
# use its *own* freshly built fastcache-cc — only one already installed or
# otherwise on PATH.

find_program(FASTCACHE_CC fastcache-cc)

# Also consumed by src/tests/CMakeLists.txt to register the sccache smoke
# tests, so it is probed unconditionally even when fastcache-cc wins below.
find_program(SCCACHE sccache)

set(FASTCACHE_ADDR "$ENV{FASTCACHE_ADDR}" CACHE STRING
    "host:port of the fastcached compile-cache daemon (enables the fastcache-cc launcher)")

# Candidate table, most-preferred first. Each row <id> is described by:
#   _fc_cache_<id>_label     human-readable name for the status message
#   _fc_cache_<id>_program   the found program (empty when not installed)
#   _fc_cache_<id>_requires  extra condition; the row is skipped when falsy
#   _fc_cache_<id>_env       NAME=VALUE pairs to inject around the invocation
# Supporting a third launcher is adding an id here plus its four variables.
set(_fc_cache_candidates fastcache_cc sccache)

set(_fc_cache_fastcache_cc_label "fastcache-cc")
set(_fc_cache_fastcache_cc_program "${FASTCACHE_CC}")
set(_fc_cache_fastcache_cc_requires "${FASTCACHE_ADDR}")
set(_fc_cache_fastcache_cc_env
    "FASTCACHE_ADDR=${FASTCACHE_ADDR}"
    "FASTCACHE_SRCROOT=${CMAKE_SOURCE_DIR}"
    "FASTCACHE_BUILDTREE=${CMAKE_BINARY_DIR}")

set(_fc_cache_sccache_label "sccache")
set(_fc_cache_sccache_program "${SCCACHE}")
set(_fc_cache_sccache_requires ON)
set(_fc_cache_sccache_env "")

set(_fc_cache_chosen "")
if(USE_COMPILER_CACHE)
  foreach(_id IN LISTS _fc_cache_candidates)
    if(_fc_cache_${_id}_program AND _fc_cache_${_id}_requires)
      set(_fc_cache_chosen "${_id}")
      break()
    endif()
  endforeach()
endif()

if(_fc_cache_chosen)
  set(_fc_cache_label "${_fc_cache_${_fc_cache_chosen}_label}")
  set(_fc_cache_program "${_fc_cache_${_fc_cache_chosen}_program}")

  # `cmake -E env NAME=VALUE ... <program>` is the only way to attach
  # environment to a compiler launcher; rows without env invoke the program
  # directly so they pay no extra process.
  if(_fc_cache_${_fc_cache_chosen}_env)
    set(_fc_cache_launcher
        "${CMAKE_COMMAND}" -E env
        ${_fc_cache_${_fc_cache_chosen}_env}
        "${_fc_cache_program}")
  else()
    set(_fc_cache_launcher "${_fc_cache_program}")
  endif()

  message(STATUS "Enabling ${_fc_cache_label} (${_fc_cache_program}) for C/C++ compilation")
  set(CMAKE_C_COMPILER_LAUNCHER ${_fc_cache_launcher})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${_fc_cache_launcher})

  # Neither launcher reproduces anything but the object file on a cache hit, so
  # a precompiled header (a second, separately produced artefact) cannot be
  # served from cache.
  set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

  # CMake's C++20 module support puts scanning flags on every compile line
  # (-fmodules-ts -fmodule-mapper=<per-object modmap> on GCC). A preprocess-only
  # run with those flags fails, so a launcher that derives its key by
  # preprocessing falls back on *every* translation unit — measured here as a
  # 100% "preprocess failed" fall-back rate. This project has no module units,
  # so the scan is pure overhead; turn it off while a launcher is in use.
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

  # For the same reason neither supports /Zi (shared PDB). Force MSVC to embed
  # debug info in .obj files (/Z7) via the modern CMake knob (CMP0141), and also
  # fix up any legacy /Zi already present in FLAGS_DEBUG / FLAGS_RELWITHDEBINFO.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    foreach(_var
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_C_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS_RELWITHDEBINFO)
      string(REGEX REPLACE "([-/])Zi" "\\1Z7" ${_var} "${${_var}}")
    endforeach()
  endif()
elseif(NOT USE_COMPILER_CACHE)
  message(STATUS "Compiler caching disabled by USE_COMPILER_CACHE=OFF")
elseif(FASTCACHE_CC AND NOT FASTCACHE_ADDR)
  message(STATUS "fastcache-cc found but FASTCACHE_ADDR is unset and sccache was not found, "
                 "caching disabled (set FASTCACHE_ADDR=host:port to use fastcache-cc)")
else()
  message(STATUS "No compiler-cache launcher found (fastcache-cc, sccache), caching disabled")
endif()
