# Which `-fdebug-prefix-map` rules a pair of roots deserves — checked as a pure
# computation, over the layouts nobody has locally.
#
# `cmake/portable/CompileCache.cmake` appends those rules so a replayed object
# names no checkout (#203). The rules themselves are two strings derived from two
# other strings, and that derivation was wrong twice before it was ever run: once
# with a trailing separator that stopped the rewritten path matching the spelling
# the build system passes, and once emitting a "relative" replacement that
# carried the whole checkout path inside it and therefore fixed nothing while
# reading as a fix.
#
# Neither is visible in the layout a developer has. Both are one row here.
#
# Verdict is read from the output, not the exit code: `message(FATAL_ERROR)`
# exits 0 on CMake 3.28, this project's declared minimum. See
# src/tests/CMakeLists.txt, which registers this with FAIL_REGULAR_EXPRESSION.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# Bring in the function without the launcher probing, the download and the
# possible daemon start.
set(FASTCACHE_COMPILE_CACHE_DEFINE_ONLY ON)
include("${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")

if(NOT COMMAND _fc_debug_prefix_map_rules)
    message(FATAL_ERROR
        "_fc_debug_prefix_map_rules is not defined. Either the define-only guard in "
        "cmake/portable/CompileCache.cmake returned before the function, or the function was renamed "
        "and this check now proves nothing.")
endif()

# ---------------------------------------------------------------------------
# The table: binary dir | source dir | expected rules (","-joined) | source mapped
#
# The rules field is comma-joined and converted afterwards: a ";" inside a field
# IS a list separator to `string(REPLACE)`, so the row would split into more
# fields than it has and `list(GET fields 3)` reads off the end.
#
# The build-tree rule is ALWAYS first and always maps to ".", because it is the
# one carrying DW_AT_comp_dir and because GCC and Clang take the first matching
# rule — a nested build tree matched by a source-first rule keeps its position
# inside the checkout.
set(Layouts
    # This project's own layout, and the one every preset produces.
    "/co/src/out/build/gcc-release|/co/src|/co/src/out/build/gcc-release=.,/co/src=../../..|ON"
    # One level down, the shape most projects use.
    "/co/src/build|/co/src|/co/src/build=.,/co/src=..|ON"
    # Out of tree on another mount. `file(RELATIVE_PATH)` answers
    # `../../mnt/d/co/src` here — relative, and carrying the entire checkout path,
    # so the source rule must be DROPPED rather than emitted.
    "/tmp/b|/mnt/d/co/src|/tmp/b=.|OFF"
    # Out of tree as a sibling: `../src` is short and still names the checkout's
    # own directory, so it is dropped for the same reason.
    "/co/build|/co/src|/co/build=.|OFF"
    # An in-source build. RELATIVE_PATH answers empty, which is not a `..` chain,
    # so only the build-tree rule lands — and that rule is a no-op here, which is
    # correct: there is no second root to reconcile.
    "/co/src|/co/src|/co/src=.|OFF"
    # A build tree whose name STARTS with `..`-ish text must not be mistaken for a
    # relative chain by a sloppier predicate.
    "/co/src/..build|/co/src|/co/src/..build=.,/co/src=..|ON"
)

set(problems 0)
foreach(row IN LISTS Layouts)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 binaryDir)
    list(GET fields 1 sourceDir)
    list(GET fields 2 expectedRules)
    string(REPLACE "," ";" expectedRules "${expectedRules}")
    list(GET fields 3 expectedMapped)

    _fc_debug_prefix_map_rules("${binaryDir}" "${sourceDir}" rules mapped)

    if(NOT "${rules}" STREQUAL "${expectedRules}")
        message(STATUS "  binary='${binaryDir}' source='${sourceDir}'")
        message(STATUS "    expected rules: ${expectedRules}")
        message(STATUS "    actual rules:   ${rules}")
        math(EXPR problems "${problems} + 1")
    elseif(NOT "${mapped}" STREQUAL "${expectedMapped}")
        message(STATUS "  binary='${binaryDir}' source='${sourceDir}'")
        message(STATUS "    expected sourceMapped=${expectedMapped}, actual ${mapped}")
        math(EXPR problems "${problems} + 1")
    endif()
endforeach()

# Every replacement this check accepts must itself be checkout-independent, and
# asserting that separately is the point: a table can be edited to expect the
# wrong answer, and then it agrees with the code and both are wrong. This asks
# the property rather than the value.
foreach(row IN LISTS Layouts)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 binaryDir)
    list(GET fields 1 sourceDir)
    _fc_debug_prefix_map_rules("${binaryDir}" "${sourceDir}" rules mapped)
    foreach(rule IN LISTS rules)
        string(REGEX REPLACE "^.*=" "" replacement "${rule}")
        # The replacement is what ends up inside the object. It may name no
        # directory that exists only on this machine — so `.` and `..` chains
        # only.
        if(NOT replacement MATCHES "^(\\.|\\.\\.(/\\.\\.)*)$")
            message(STATUS "  binary='${binaryDir}' source='${sourceDir}'")
            message(STATUS "    replacement '${replacement}' is not checkout-independent")
            math(EXPR problems "${problems} + 1")
        endif()
    endforeach()
endforeach()

list(LENGTH Layouts layoutCount)
if(problems GREATER 0)
    message(FATAL_ERROR "debug-prefix-map rules: ${problems} layout(s) wrong out of ${layoutCount}")
endif()
message(STATUS "debug-prefix-map rules: ${layoutCount} layouts correct")
