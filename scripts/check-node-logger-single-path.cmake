# SPDX-License-Identifier: Apache-2.0
#
# The compile node builds its console logger in exactly ONE place.
#
# `fastcache-compile-node` shipped for months with
# ([#485](https://github.com/LASTRADA-Software/fastcached/issues/485)):
#
#     ConsoleLogger consoleLogger { std::cerr, cfg.logLevel };
#
# against a constructor whose third parameter is `bool timestamps = false`. The
# argument was not wrong, it was ABSENT -- so the node could not emit a time, no flag
# existed to ask for one, and `ConsoleLogger` was perfectly correct throughout. A
# `cluster-e2e` failure then could not be diagnosed from a completed run at all: a
# leader elected at second 59 and one elected at second 3 that never affirms are the
# same bytes without times.
#
# ## Why a check and not a test
#
# `MakeNodeConsoleLogger` closes it, and a unit test drives configuration in and reads
# stamped bytes out. What that test CANNOT see is `main.cpp` keeping a second
# construction beside it. The seam would then be correct, tested, and not what
# production runs -- which is this repository's `PurgeExpired` failure exactly: a
# reclaimer that was correct, was tested, and had no production caller at all. A green
# test over an unused function is worse than no test, because it reads as coverage.
#
# So the load-bearing property is not "the factory is right", it is "the factory is the
# ONLY path". That is a fact about call sites, which no unit test can assert and a
# string scan can.
#
# ## The rule
#
# **`ConsoleLogger` is constructed exactly once across the node's sources, in
# `NodeLogging.cpp`.**
#
# `main.cpp` is covered because that is where the defect was and where a second one
# would go. Comments naming the type are prose, not construction -- the same carve-out
# `check-worker-refusals-counted` documents, and for the same reason: this file's own
# header describes what it checks.
#
# A scan matching NOTHING is its own failure rather than a pass. `NodeLogging.cpp`
# contains one construction by definition, so zero means the scan has stopped looking
# at what it thinks it is -- a renamed factory, a moved file, a changed spelling.
#
# Runs as `cmake -P`: it reads two files, compares strings and reports. The verdict is
# the presence of `CMake Error` in the OUTPUT, never the exit code -- `message(FATAL_ERROR)`
# exits 0 on CMake 3.28, this project's declared minimum. See
# `check-script-check-signals.cmake`.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-node-logger-single-path.cmake
#
# Exit codes: 0 always.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(sources
    "src/apps/fastcache-compile-node/NodeLogging.cpp"
    "src/apps/fastcache-compile-node/main.cpp")

foreach(relative IN LISTS sources)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        message(FATAL_ERROR "a covered source is missing: ${relative}")
    endif()
endforeach()

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a line
# containing a semicolon becomes two elements and every line number after it drifts.
# C++ is made of semicolons, so this is not a corner case here.
set(constructions "")
foreach(relative IN LISTS sources)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")

    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not code.
        if(line MATCHES "^[ \t]*(//|///|\\*)")
            continue()
        endif()
        # A construction, by either spelling: `ConsoleLogger name {`, `ConsoleLogger(`,
        # or `make_unique<ConsoleLogger>`. A declaration of a REFERENCE or pointer to
        # one is not a construction and is deliberately not matched.
        if(line MATCHES "ConsoleLogger>[ \t]*\\(" OR line MATCHES "ConsoleLogger[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*[{(]")
            list(APPEND constructions "${relative}:${lineNumber}")
        endif()
    endforeach()
endforeach()

list(LENGTH constructions siteCount)

if(siteCount EQUAL 0)
    message("")
    message("  No `ConsoleLogger` construction was found in the node's sources.")
    message("")
    message("`NodeLogging.cpp` holds one by definition, so zero means this scan is no")
    message("longer looking at what it thinks it is -- a renamed factory, a moved file,")
    message("a changed spelling. It is NOT evidence that the node builds one logger.")
    message(FATAL_ERROR "node logger: the scan matched nothing and cannot conclude")
endif()

if(NOT siteCount EQUAL 1)
    string(REPLACE ";" ", " where "${constructions}")
    message("")
    message("  ConsoleLogger is constructed at ${siteCount} places: ${where}")
    message("")
    message("Exactly one is allowed, inside `MakeNodeConsoleLogger`. A second one is how")
    message("#485 happened: a call site spelling out the arguments itself, with the")
    message("third simply absent, against a constructor that defaults it. The logger was")
    message("correct; the argument was never passed, and no test of the logger could")
    message("see that.")
    message("")
    message("Call `MakeNodeConsoleLogger(sink, cfg)`. If a second sink genuinely needs")
    message("one, give the factory the parameter rather than building a logger beside it")
    message("-- otherwise `NodeLogging_test` covers a path production no longer takes.")
    message(FATAL_ERROR "node logger: ${siteCount} construction site(s)")
endif()

if(NOT constructions MATCHES "NodeLogging\\.cpp")
    message("")
    message("  The one ConsoleLogger construction is at ${constructions}, not in NodeLogging.cpp.")
    message("")
    message("The factory is meant to be the only construction path. One built elsewhere")
    message("leaves `MakeNodeConsoleLogger` a function nothing calls, and its test green")
    message("over code production does not run.")
    message(FATAL_ERROR "node logger: the construction is outside NodeLogging.cpp")
endif()

message(STATUS "node logger: one construction path, and it is the factory")
