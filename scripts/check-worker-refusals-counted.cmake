# SPDX-License-Identifier: Apache-2.0
#
# Every refusal the compile surface answers with must move a counter.
#
# Six sites in `WorkerProtocol.cpp` used to answer on the wire and increment nothing
# ([#327](https://github.com/LASTRADA-Software/fastcached/issues/327)). A refusal
# answered while nothing rises is how a port being probed looks, on `/metrics`,
# exactly like a port nobody is talking to -- and the six were not a coincidence but
# the default: `Wire::EncodeErrorReply` takes a code and knows nothing about a sink,
# so counting was a thing each author had to remember, and a seventh would have joined
# them by omission.
#
# `Refuse(metrics, row, detail)` is now the one way that file refuses, and it takes a
# ROW -- there is no argument to pass a bare `ErrorCode` to, so the counter cannot be
# left out. What that does NOT prevent is somebody calling `Wire::EncodeErrorReply`
# directly, because it is a free function in a header every surface includes. That is
# the hole this check closes, and it is the same shape as
# `check-target-file-guards`: the type system covers what it can, and a check covers
# the door it cannot close.
#
# ## The rule, and why it is exact rather than a heuristic
#
# **`Wire::EncodeErrorReply` appears exactly once across the surfaces, inside
# `Refuse`.**
#
# Four files are covered, and an operator reads their counters side by side:
# `WorkerProtocol.cpp` answers the verbs, `CompileResponder.cpp` answers the peer and
# capacity questions for compiles arriving on the merged `0xFC` listener,
# `CompileCapacity.cpp` holds the predicates both of them share -- admission and the
# drain decision ([#290](https://github.com/LASTRADA-Software/fastcached/issues/290))
# -- and `FrameEndpoint.cpp` is the listener itself.
# `CompileResponder.cpp` is why this list is a list rather than a pair: it was added
# with uncounted refusals in it, and the scan said nothing because it was not looking.
#
# `FrameEndpoint.cpp` is why that happened TWICE, and it is the file this check was
# most needed on. It held five refusals that answered on the wire and counted nothing
# -- the frame ceiling, the in-flight byte budget, the two credential outcomes and the
# at-capacity refusal -- and the scan could see none of them, so #326's counter stopped
# moving the moment the dedicated compile port was retired and no test failed
# ([#447](https://github.com/LASTRADA-Software/fastcached/issues/447)).
#
# It could not be added while ONE site remained, and that is a property of this check
# rather than a matter of thoroughness: the rule below is exactness, so a file with one
# uncovered refusal cannot be covered at all. Four of the five now ask the surface that
# owns the verb, through `IFrameResponder::RefusalReply` and `EndpointRefusalReply`; the
# fifth is decided at accept with no verb in hand, so the endpoint counts it itself,
# against its own `Cc::SurfaceRefusal` row.
#
# `WorkerServer.cpp` used to be the second row, answering before a verb was reached on
# the DEDICATED compile port. #290 stage 3 retired that port -- the compile verbs now
# arrive on the merged listener with everything else -- and its two shared predicates
# moved into `CompileCapacity.cpp`, which is what took its place here. The row was not
# simply dropped: a covered surface that stops existing has to be replaced by wherever
# its refusals went, or this check quietly narrows to what is left.
#
# The frame-level payload ceiling is the CHEAPEST probe there is, needing only a header
# where the envelope refusals need a whole frame read, so it is the likeliest thing to
# be pointed at a node and it counted nothing at all
# ([#326](https://github.com/LASTRADA-Software/fastcached/issues/326)).
#
# It is exact because the three refusals that ALREADY counted -- the lease refusal,
# the envelope error and the job refusal -- were routed through `Refuse` too, carrying
# their own tables' rows rather than restating the pair. Had they been left as they
# were, this check would have had to ask something like "is there an `Increment`
# within N lines", which passes for the wrong reasons: an increment belonging to a
# different branch, or the right one moved out of range by an edit. A structural fact
# about one call site cannot go approximately right.
#
# Comments naming the function are prose, not calls, for the reason
# `check-target-file-guards` records: this file's own header describes the thing it
# checks, and a checker that failed on its own documentation would be a checker
# nobody could document.
#
# Runs as `cmake -P`: it reads a file, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-worker-refusals-counted.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The surfaces, and the number of `Refuse` definitions to expect across them. One:
# the node's files call the one `WorkerProtocol` defines, because two surfaces of one
# worker must not hold two notions of what a refusal is.
set(surfaces
    "src/apps/fastcache-cc/WorkerProtocol.cpp"
    "src/apps/fastcache-compile-node/CompileCapacity.cpp"
    "src/apps/fastcache-compile-node/CompileResponder.cpp"
    "src/apps/fastcache-compile-node/FrameEndpoint.cpp")

foreach(relative IN LISTS surfaces)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        message(FATAL_ERROR "a covered surface is missing: ${relative}")
    endif()
endforeach()

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a
# line containing a semicolon becomes two elements and every line number after it
# drifts. C++ is made of semicolons, so this is not a corner case here.
set(callSites "")
foreach(relative IN LISTS surfaces)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")

    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not a call.
        if(line MATCHES "^[ \t]*(//|///|\\*)")
            continue()
        endif()
        if(line MATCHES "EncodeErrorReply[ \t]*\\(")
            list(APPEND callSites "${relative}:${lineNumber}")
        endif()
    endforeach()
endforeach()

list(LENGTH callSites siteCount)

# A scan that found NONE is not a clean file -- it is a scan that stopped working,
# because `Refuse` must contain one. Reported as its own failure rather than folded
# into success, which is the direction this project keeps getting wrong.
if(siteCount EQUAL 0)
    message("")
    message("  No `EncodeErrorReply` call was found on any covered surface.")
    message("")
    message("`Refuse` is built on one, so zero means this scan is no longer looking at")
    message("what it thinks it is -- a renamed function, a moved file, a changed")
    message("spelling. It is not evidence that every refusal is counted.")
    message(FATAL_ERROR "worker refusals: the scan matched nothing and cannot conclude")
endif()

if(NOT siteCount EQUAL 1)
    string(REPLACE ";" ", " where "${callSites}")
    message("")
    message("  EncodeErrorReply is called at ${siteCount} places: ${where}")
    message("")
    message("Exactly one is allowed, inside `Refuse`, because that is what makes every")
    message("refusal on this surface move a counter. A refusal answered while nothing")
    message("rises is indistinguishable, on /metrics, from a port nobody is talking to")
    message("-- which is how six of them went unnoticed (#327).")
    message("")
    message("Add a `SurfaceRefusal` row naming the code AND the counter, and answer")
    message("with `Refuse(_metrics, row, detail)`. If the code comes from another")
    message("table that already pairs the two -- a lease refusal, an envelope error, a")
    message("job refusal -- convert that row rather than restating the pair.")
    message(FATAL_ERROR "worker refusals: ${siteCount} uncontrolled refusal site(s)")
endif()

message(STATUS "worker refusals: one refusal path, and it counts")
