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

if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}")
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR is not a directory: ${FASTCACHED_SOURCE_DIR}")
endif()

# Under `cmake -P` the module returns after its pure computations, so this brings
# in the function without the launcher probing, the download and the possible
# daemon start.
include("${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")

if(NOT COMMAND _fc_debug_prefix_map_rules)
    message(FATAL_ERROR
        "_fc_debug_prefix_map_rules is not defined. Either the define-only guard in "
        "cmake/portable/CompileCache.cmake returned before the function, or the function was renamed "
        "and this check now proves nothing.")
endif()

# ---------------------------------------------------------------------------
# The table: binary dir | source dir | expected rules (","-joined)
#
# The rules field is comma-joined and converted afterwards: a ";" inside a field
# IS a list separator to `string(REPLACE)`, so the row would split into more
# fields than it has and the last `list(GET)` reads off the end.
#
# There is no "was the source mapped" column. It is exactly "two rules rather than
# one", so a column would be a second place to write one fact and the only thing
# it could add is a row that disagrees with itself -- at which point the check
# fails for a layout that describes nothing and the reader has to work out which
# of the two spellings is the lie. It is DERIVED and still asserted, because what
# is being checked is that the function's two outputs agree.
#
# The build-tree rule is ALWAYS first and always maps to ".", because it is the
# one carrying DW_AT_comp_dir and because GCC and Clang take the first matching
# rule — a nested build tree matched by a source-first rule keeps its position
# inside the checkout.
set(Layouts
    # This project's own layout, and the one every preset produces.
    "/co/src/out/build/gcc-release|/co/src|/co/src/out/build/gcc-release=.,/co/src=../../.."
    # One level down, the shape most projects use.
    "/co/src/build|/co/src|/co/src/build=.,/co/src=.."
    # Out of tree on another mount. `file(RELATIVE_PATH)` answers
    # `../../mnt/d/co/src` here — relative, and carrying the entire checkout path,
    # so the source rule must be DROPPED rather than emitted.
    "/tmp/b|/mnt/d/co/src|/tmp/b=."
    # Out of tree as a sibling: `../src` is short and still names the checkout's
    # own directory, so it is dropped for the same reason.
    "/co/build|/co/src|/co/build=."
    # An in-source build. RELATIVE_PATH answers empty, which is not a `..` chain,
    # so only the build-tree rule lands — and that rule is a no-op here, which is
    # correct: there is no second root to reconcile.
    "/co/src|/co/src|/co/src=."
    # A build tree whose name STARTS with `..`-ish text must not be mistaken for a
    # relative chain by a sloppier predicate.
    "/co/src/..build|/co/src|/co/src/..build=.,/co/src=.."
)

set(problems 0)
foreach(row IN LISTS Layouts)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 binaryDir)
    list(GET fields 1 sourceDir)
    list(GET fields 2 expectedRules)
    string(REPLACE "," ";" expectedRules "${expectedRules}")
    list(LENGTH expectedRules expectedRuleCount)
    set(expectedMapped OFF)
    if(expectedRuleCount GREATER 1)
        set(expectedMapped ON)
    endif()

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

    # Asked of the RESULT rather than of the table, which is what keeps it an
    # independent check: a table can be edited to expect the wrong answer, and
    # then it agrees with the code and both are wrong. This asks the property.
    # Every replacement is what ends up inside an object, so it may name no
    # directory that exists only on this machine -- `.` and `..` chains only.
    foreach(rule IN LISTS rules)
        string(REGEX REPLACE "^.*=" "" replacement "${rule}")
        if(NOT replacement MATCHES "^(\.|\.\.(/\.\.)*)$")
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
