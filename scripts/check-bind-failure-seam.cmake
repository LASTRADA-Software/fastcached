# SPDX-License-Identifier: Apache-2.0
#
# Every surface the compile node opens reaches its bind-failure verdict through
# `JudgeBindFailure`, and no opener decides one itself.
#
# `NodeSurfaceTable()` carries a `BindFailurePolicy` column and a sentence saying why
# ([#352](https://github.com/LASTRADA-Software/fastcached/issues/352)), and a
# `static_assert` refuses a row that omits either. That guard is exact about the
# TABLE and says nothing about the CODE: a fifth opener can be written tomorrow that
# binds a port and answers
#
#     return std::unexpected { std::format("cannot bind {}: {}", where, why) };
#
# which compiles, links, passes every test, and puts the verdict back in the place
# the ticket was filed about. The row would still be there, still stating a policy,
# and nothing would read it. That is the same shape as the counter defect
# `check-worker-refusals-counted.cmake` exists for -- a rule the type system enforces
# on one side of a seam and not on the other -- so this is deliberately built the
# same way, down to the whole-file prefilter and the reason each exclusion carries.
#
# ## What is scanned, and what is not
#
# `src/apps/fastcache-compile-node/` only. The daemon binds too --
# `src/apps/fastcached/main.cpp` and `src/FastCache/Server/ReactorServerLoop.cpp`
# each decide a bind failure inline -- and they are deliberately NOT covered here:
# `BindFailurePolicy` lives in the node's own `NodeSurfaces.hpp`, which the library
# and the daemon cannot include. Widening this scan without first moving the seam
# would report violations nobody can fix, so the boundary is stated rather than
# discovered.
#
# ## Why a per-FILE requirement rather than a per-SITE one
#
# A bind and the verdict it produces are usually several statements apart, often in
# a different function of the same file -- `NodeFrameSurface::Bind` binds and
# `StartNodeSurfaceOrExplain` judges. Pairing them mechanically would need to
# understand control flow, and a checker that guesses at that reports confident
# nonsense on the first opener written in a shape it did not anticipate.
#
# So the requirement is the one that can be checked exactly: a file that calls a
# bind primitive must also name `JudgeBindFailure`. That cannot prove a particular
# failure path goes through the seam, and it does not claim to. What it does close
# is the case this ticket is about -- a whole opener written without the seam ever
# being reached -- which is the only way the four current ones went wrong.
#
# ## The two ways this check can be wrong about itself
#
# Both are failures rather than silences, because a scan that finds nothing looks
# exactly like a tree with nothing to find (#492).
#
#   - **The scan matched nothing.** No files, no `JudgeBindFailure` anywhere, or a
#     bind primitive that appears in no file at all -- the last being what a rename
#     looks like, after which this check goes on passing while guarding nothing.
#   - **A file could not be read.** Distinguished from a violation and escalated to
#     a failure, never counted as compliant, for `ci-scope.sh`'s reason: every way of
#     not knowing resolves to the answer that fails closed.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-bind-failure-seam.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

# A `cmake -P` script has no project, so every policy starts unset and CMP0057
# (`if(... IN_LIST ...)`) is one of them -- unset, the operator is an unknown
# argument and the script errors out rather than answering. Stated as a minimum
# version so the whole set moves together with the project's own.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(seam "JudgeBindFailure")
set(nodeDirectory "src/apps/fastcache-compile-node")

# ---------------------------------------------------------------------------
# What counts as opening a listening surface.
#
# Each row is a primitive plus the reason it belongs, because a needle nobody can
# read the argument for is a needle nobody can challenge. Every one of these is
# asserted to appear SOMEWHERE below: a primitive that matches nothing has been
# renamed, and a scan looking for a name nobody calls passes forever.
set(bindPrimitives
    "FrameEndpoint::Start"
    "FrameEndpoint::StartAdopted"
    "PlatformListener::Bind"
    "BlockingListener::Bind"
    "OpenSharedPortUdpSocket")
set(bindReasons
    "the 0xFC surface, bound on the reactor"
    "the same surface under socket activation"
    "the reactor TCP listener consensus uses"
    "the blocking TCP listener the admin surface uses"
    "discovery's UDP pair, the one non-TCP surface")

list(LENGTH bindPrimitives primitiveCount)
list(LENGTH bindReasons reasonCount)
if(NOT primitiveCount EQUAL reasonCount)
    message(FATAL_ERROR "bind-failure seam: ${primitiveCount} primitive(s) but ${reasonCount} reason(s)")
endif()

# Files that bind and deliberately do NOT judge, each with the reason.
#
# One row, and it is not an opener: `FrameEndpoint` is the MECHANISM a surface is
# opened with. It returns its failure to the caller, which is where the row is
# known and where the verdict is reached. A mechanism that judged would be deciding
# policy for every surface built on it, which is the defect inverted.
set(allowedFiles "${nodeDirectory}/FrameEndpoint.cpp")
set(allowedReasons "hands its bind failure to the caller, which owns the row")

list(LENGTH allowedFiles allowedFileCount)
list(LENGTH allowedReasons allowedReasonCount)
if(NOT allowedFileCount EQUAL allowedReasonCount)
    message(FATAL_ERROR "bind-failure seam: ${allowedFileCount} allowed file(s) but ${allowedReasonCount} reason(s)")
endif()

# ---------------------------------------------------------------------------
# What is scanned. ONE traversal; two patterns would mean two walks of a tree that
# costs 2.09 s on DrvFs (#502).
file(GLOB_RECURSE sources RELATIVE "${FASTCACHED_SOURCE_DIR}"
     "${FASTCACHED_SOURCE_DIR}/${nodeDirectory}/*")
list(FILTER sources INCLUDE REGEX "\\.(cpp|hpp)$")
list(SORT sources)

if(NOT sources)
    message("")
    message("  The glob over ${nodeDirectory} matched no C++ file at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "bind-failure seam: the scan matched no source file and cannot conclude")
endif()

# What is NOT scanned, and why. Patterns rather than filenames, so a new test file
# matches automatically and a new NON-test file does not -- the direction that fails
# closed.
set(excludePatterns
    "_test\\.cpp$"           # unit tests construct synthetic rows and call the seam directly
    "TestSupport\\.hpp$"     # test-only helpers, same reason
    "TestUtils\\.hpp$")      # test-only helpers, same reason

set(violations "")
set(unreadable "")
set(seamFiles "")
set(scannedCount 0)
foreach(primitive IN LISTS bindPrimitives)
    set(primitiveHits_${primitive} 0)
endforeach()

foreach(relative IN LISTS sources)
    set(skip FALSE)
    foreach(pattern IN LISTS excludePatterns)
        if(relative MATCHES "${pattern}")
            set(skip TRUE)
            break()
        endif()
    endforeach()
    if(skip)
        continue()
    endif()
    math(EXPR scannedCount "${scannedCount} + 1")

    # A file listed by the glob that comes back empty while having bytes on disk was
    # not read. That is NOT a file with no bind site in it, and the difference is the
    # whole of clause two: counting it as compliant is how a scan reports a clean
    # tree it never looked at.
    file(SIZE "${FASTCACHED_SOURCE_DIR}/${relative}" sizeOnDisk)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)
    string(LENGTH "${content}" contentLength)
    if(sizeOnDisk GREATER 0 AND contentLength EQUAL 0)
        list(APPEND unreadable "${relative}")
        continue()
    endif()

    set(allowed FALSE)
    if("${relative}" IN_LIST allowedFiles)
        set(allowed TRUE)
    endif()

    # Whole-file first, and this is not an approximation: each needle is a strict
    # PREFIX of the pattern matched per line below, so a file containing none can
    # contain no site either. It can open a file for nothing, which costs only time;
    # it cannot skip one.
    set(interesting FALSE)
    foreach(needle IN LISTS bindPrimitives ITEMS "${seam}")
        string(FIND "${content}" "${needle}" found)
        if(NOT found EQUAL -1)
            set(interesting TRUE)
            break()
        endif()
    endforeach()
    if(NOT interesting)
        continue()
    endif()

    # Per LINE from here, because a name in PROSE is not a call.
    #
    # Both of this check's first two findings were doc comments: `AdminEndpoint.hpp`
    # explains a design decision "for the reason `FrameEndpoint::Start` is", and
    # `main.cpp` names `FrameEndpoint::StartAdopted` while explaining what it does NOT
    # build. Whole-file matching called both openers that decide their own verdict --
    # a checker that fails on documentation of the thing it checks, which is the
    # failure `check-target-file-guards` records and which was two lines of prose away
    # from being shipped here too.
    #
    # Walked over newlines rather than split into a CMake list: an unbalanced `[` or
    # `]` makes the list parser swallow every following element into one, and this
    # file matches text containing `[[nodiscard]]`.
    set(scanRest "${content}")
    set(lineNumber 0)
    set(binds FALSE)
    set(callsSeam FALSE)
    while(NOT scanRest STREQUAL "")
        math(EXPR lineNumber "${lineNumber} + 1")

        string(FIND "${scanRest}" "\n" scanNewline)
        if(scanNewline EQUAL -1)
            set(line "${scanRest}")
            set(scanRest "")
        else()
            string(SUBSTRING "${scanRest}" 0 ${scanNewline} line)
            math(EXPR scanNewline "${scanNewline} + 1")
            string(SUBSTRING "${scanRest}" ${scanNewline} -1 scanRest)
        endif()

        # Prose, not a call.
        if(line MATCHES "^[ \t]*(//|///|\\*)")
            continue()
        endif()

        foreach(primitive IN LISTS bindPrimitives)
            if(line MATCHES "${primitive}[ \t]*\\(")
                set(binds TRUE)
                math(EXPR primitiveHits_${primitive} "${primitiveHits_${primitive}} + 1")
            endif()
        endforeach()

        # A CALL, not the declaration in `NodeSurfaces.hpp` and not a mention in a
        # comment. A file that merely names the seam has not routed anything through
        # it, and accepting a mention would let a violation be waved away with a
        # sentence.
        if(line MATCHES "${seam}[ \t]*\\(" AND NOT line MATCHES "${seam}[ \t]*\\(SurfaceRow")
            set(callsSeam TRUE)
        endif()
    endwhile()

    if(callsSeam)
        list(APPEND seamFiles "${relative}")
    endif()

    if(binds AND NOT allowed AND NOT callsSeam)
        list(APPEND violations "${relative}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The three ways this concludes nothing, each reported as a failure.
if(unreadable)
    message("")
    message("  These files were listed by the scan and could not be read:")
    message("")
    foreach(relative IN LISTS unreadable)
        message("    ${relative}")
    endforeach()
    message("")
    message("A file that could not be read is not a file with no bind site in it.")
    message("Treating the two the same is how a scan reports a clean tree it never")
    message("looked at, so this is a failure rather than a silence.")
    message(FATAL_ERROR "bind-failure seam: ${nodeDirectory} contains files this check could not read")
endif()

if(NOT seamFiles)
    message("")
    message("  No file under ${nodeDirectory} names ${seam} at all.")
    message("")
    message("Either the seam was renamed -- in which case this check is looking for")
    message("something nobody calls and would pass forever -- or every opener has")
    message("stopped using it, which is the defect this check exists to report.")
    message(FATAL_ERROR "bind-failure seam: the scan found no use of ${seam} and cannot conclude")
endif()

set(missingPrimitives "")
set(index 0)
foreach(primitive IN LISTS bindPrimitives)
    if(primitiveHits_${primitive} EQUAL 0)
        list(GET bindReasons ${index} reason)
        list(APPEND missingPrimitives "${primitive} (${reason})")
    endif()
    math(EXPR index "${index} + 1")
endforeach()

if(missingPrimitives)
    message("")
    message("  These bind primitives appear in no scanned file:")
    message("")
    foreach(entry IN LISTS missingPrimitives)
        message("    ${entry}")
    endforeach()
    message("")
    message("A needle nobody calls is a needle that cannot catch anything. Either the")
    message("primitive was renamed -- update the table above, with its reason -- or the")
    message("surface it opened is gone, in which case remove the row and say so.")
    message(FATAL_ERROR "bind-failure seam: ${primitiveCount} primitive(s) in the table, some matching nothing")
endif()

# ---------------------------------------------------------------------------
# The verdict.
if(violations)
    message("")
    message("  These files open a listening surface and never reach ${seam}:")
    message("")
    foreach(relative IN LISTS violations)
        message("    ${relative}")
    endforeach()
    message("")
    message("What a bind failure DOES is a column of NodeSurfaceTable(), not a decision")
    message("an opener makes -- that is #352, and the static_assert beside the table")
    message("only forces the ROW to answer. An opener that refuses on its own compiles,")
    message("links and passes every test while putting the verdict back in the place")
    message("the ticket was filed about.")
    message("")
    message("Route the failure through ${seam}(RowFor(<surface>), <message>, logger).")
    message("If this file binds a socket that is not a node surface, add it to")
    message("allowedFiles above WITH THE REASON -- an exclusion nobody can read the")
    message("argument for is an exclusion nobody can challenge.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "bind-failure seam: ${violationCount} opener(s) decide a bind failure without the row")
endif()

list(LENGTH seamFiles seamFileCount)
message("bind-failure seam: ${scannedCount} file(s) scanned, ${seamFileCount} reaching ${seam}, no opener deciding its own verdict")
