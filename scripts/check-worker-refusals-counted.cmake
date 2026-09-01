# SPDX-License-Identifier: Apache-2.0
#
# Every refusal a `0xFC` surface answers with is routed through one of three named
# spellings, and no file reaches the raw encoder.
#
# Six sites in `WorkerProtocol.cpp` used to answer on the wire and increment nothing
# ([#327](https://github.com/LASTRADA-Software/fastcached/issues/327)). A refusal
# answered while nothing rises is how a port being probed looks, on `/metrics`,
# exactly like a port nobody is talking to -- and the six were not a coincidence but
# the default: `CompileCacheWire::EncodeErrorReply` takes a code and knows nothing
# about a sink, so counting was a thing each author had to remember.
#
# ## What this check used to be, and why that was not enough
#
# It scanned a HAND-MAINTAINED LIST of the `.cpp` files somebody had remembered. The
# list grew by hand twice -- `CompileResponder.cpp` was added after arriving with
# uncounted refusals in it, then `FrameEndpoint.cpp` after accumulating FIVE of them
# through an entire surface migration with this check green throughout (#447). It
# could not reach a header at all, which is where #447 then had to put two security
# counters.
#
# The list was exact about the files it knew and SILENT about the ones it did not,
# and silence reads identically to complete coverage
# ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492)). So the list is
# gone: this globs `src/`, headers included, and a new file answering a refusal fails
# it on the first bare call with nobody editing anything. An over-broad scan fails
# CLOSED -- a file that should not be scanned produces a visible failure somebody
# investigates, where a file that should be scanned and is not produces nothing.
#
# ## The rule
#
# **`EncodeErrorReply` is called from `Protocol/SurfaceRefusal.hpp` and nowhere else.**
#
# Two files are named below and neither is a list of surfaces: they are the two ends
# of one function. `CompileCacheWire.hpp` DEFINES the encoder, and `SurfaceRefusal.hpp`
# holds the three spellings every surface reaches it through. Every other file in
# `src/` must contain no call at all. Adding a row here is the only way to weaken this
# check, which makes weakening it a visible, argued edit rather than an omission --
# the opposite failure direction from the list this replaced.
#
# ## The three spellings, and why the third is counted rather than forbidden
#
# `Refuse` alone was not enough, and the reason is the defect that outlived #327's
# fix. Some refusals are DELIBERATELY not counted -- the in-flight byte budget says a
# surface is momentarily full, the peer retries past it, and summed into a credential
# series it makes that series unreadable. That is a considered position. But it was
# spelled as a bare `EncodeErrorReply`, which is also how "forgot" is spelled, so no
# scan could tell four considered decisions from five accumulated defects.
#
# So `RefuseWithoutCounter` states the decision and carries its reason, and
# `RefuseUntriaged` states the ABSENCE of one and carries the issue that will make it.
# The third is not a loophole precisely because this check TALLIES it and prints the
# total on every run: the backlog is visible and monotonic, a new one cannot be added
# silently, and the issues it points at get a mechanical completion test instead of a
# judgement call. Marking a site untriaged buys nothing except honesty, which is the
# property that makes it safe to have.
#
# Comments naming the function are prose, not calls, for the reason
# `check-target-file-guards` records: this file's own header describes the thing it
# checks, and a checker that failed on its own documentation would be a checker
# nobody could document.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
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

# ---------------------------------------------------------------------------
# Where the primitive lives, and where the encoder it wraps is defined.
#
# Both are asserted to exist. A moved or renamed file must fail here rather than
# quietly reduce this check to scanning a tree with nothing to find in it.
set(refusalHeader "src/FastCache/Protocol/SurfaceRefusal.hpp")
set(wireHeader "src/FastCache/Protocol/CompileCacheWire.hpp")

# The two files allowed to name the encoder, each with the reason it is allowed.
# NOT a list of surfaces -- surfaces are found by globbing. These are the definition
# and the one wrapper over it.
set(allowedFiles
    "${wireHeader}"
    "${refusalHeader}")
set(allowedReasons
    "defines EncodeErrorReply"
    "holds the three spellings every surface refuses through")

# A row is a file AND its reason, carried as two lists because CMake has no better
# shape for one. Checked rather than trusted: a reason silently attached to the wrong
# file is how an exclusion nobody can challenge gets written.
list(LENGTH allowedFiles allowedFileCount)
list(LENGTH allowedReasons allowedReasonCount)
if(NOT allowedFileCount EQUAL allowedReasonCount)
    message(FATAL_ERROR "worker refusals: ${allowedFileCount} allowed file(s) but ${allowedReasonCount} reason(s)")
endif()

# The three spellings, checked to exist. A rename that emptied this check would
# otherwise leave it reporting a clean tree it is no longer looking at.
set(spellings "Refuse" "RefuseWithoutCounter" "RefuseUntriaged")

foreach(relative IN LISTS allowedFiles)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        message(FATAL_ERROR "a file this check is built on is missing: ${relative}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Reading a file as LINES.
#
# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a
# line containing a semicolon becomes two elements and every line number after it
# drifts. C++ is made of semicolons, so this is not a corner case here.
function(fastcached_read_lines path outVar)
    file(READ "${path}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(${outVar} "${lines}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# What is scanned.
#
# Every C++ file under `src/`, headers included, because #447 put refusal rows in a
# header and the old scan could not see them.
file(GLOB_RECURSE sources RELATIVE "${FASTCACHED_SOURCE_DIR}"
     "${FASTCACHED_SOURCE_DIR}/src/*.cpp"
     "${FASTCACHED_SOURCE_DIR}/src/*.hpp")
list(SORT sources)

if(NOT sources)
    message("")
    message("  The glob over src/ matched no C++ file at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "worker refusals: the scan matched no source file and cannot conclude")
endif()

# What is NOT scanned, and why. Each row is a pattern plus its reason, because an
# exclusion nobody can read the argument for is an exclusion nobody can challenge.
#
# These are patterns rather than filenames on purpose: a new test file matches
# automatically, and a new NON-test file does not. That is the direction that fails
# closed -- the whole point of replacing the list this check used to carry.
set(excludePatterns
    "(^|/)src/tests/"
    "_test\\.cpp$"
    "TestSupport\\.hpp$"
    "TestUtils\\.hpp$"
    "(^|/)test_main\\.cpp$")
set(excludeReasons
    "shared test fixtures: a fake server encodes error replies to script one"
    "unit tests construct the exact bytes a client would see"
    "test-only helpers, same reason"
    "test-only helpers, same reason"
    "Catch2 entry points")

list(LENGTH excludePatterns excludePatternCount)
list(LENGTH excludeReasons excludeReasonCount)
if(NOT excludePatternCount EQUAL excludeReasonCount)
    message(FATAL_ERROR
            "worker refusals: ${excludePatternCount} exclusion pattern(s) but ${excludeReasonCount} reason(s)")
endif()

# ---------------------------------------------------------------------------
# Pass 1: every call to the raw encoder, and every untriaged refusal.
set(violations "")
set(allowedHits 0)
set(untriaged "")
set(issueTokens "")
set(scannedCount 0)

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

    set(allowed FALSE)
    if("${relative}" IN_LIST allowedFiles)
        set(allowed TRUE)
    endif()

    fastcached_read_lines("${FASTCACHED_SOURCE_DIR}/${relative}" lines)

    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not a call.
        if(line MATCHES "^[ \t]*(//|///|\\*)")
            continue()
        endif()

        if(line MATCHES "EncodeErrorReply[ \t]*\\(")
            if(allowed)
                math(EXPR allowedHits "${allowedHits} + 1")
            else()
                list(APPEND violations "${relative}:${lineNumber}")
            endif()
        endif()

        # The backlog. Counted everywhere except where the spelling is DEFINED.
        if(NOT allowed AND line MATCHES "RefuseUntriaged[ \t]*\\(")
            list(APPEND untriaged "${relative}:${lineNumber}")
        endif()
        if(NOT allowed AND line MATCHES "\\.issue[ \t]*=[ \t]*([A-Za-z_][A-Za-z0-9_]*|[0-9]+)")
            list(APPEND issueTokens "${relative}|${CMAKE_MATCH_1}")
        endif()

        # An issue number named once and used many times. Resolved so the tally can
        # be reported per issue, which is what gives each of them a completion test.
        if(line MATCHES "constexpr[ \t]+std::uint32_t[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*=[ \t]*([0-9]+)[ \t]*;")
            set(issueConstant_${relative}_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# Pass 2: the scan is looking at what it thinks it is.
#
# A scan that found NO call is not a clean tree -- the three spellings are built on
# one each, so zero means a renamed function, a moved file or a changed spelling.
# Reported as its own failure rather than folded into success, which is the direction
# this project keeps getting wrong.
if(allowedHits EQUAL 0)
    message("")
    message("  No `EncodeErrorReply` call was found in ${refusalHeader}.")
    message("")
    message("The three refusal spellings are each built on one, so zero means this scan")
    message("is no longer looking at what it thinks it is -- a renamed function, a moved")
    message("file, a changed spelling. It is not evidence that every refusal is routed.")
    message(FATAL_ERROR "worker refusals: the scan matched nothing and cannot conclude")
endif()

fastcached_read_lines("${FASTCACHED_SOURCE_DIR}/${refusalHeader}" refusalLines)
set(missingSpellings "")
foreach(spelling IN LISTS spellings)
    set(found FALSE)
    foreach(line IN LISTS refusalLines)
        if(line MATCHES "^[ \t]*(//|///|\\*)")
            continue()
        endif()
        if(line MATCHES "${spelling}[ \t]*\\(")
            set(found TRUE)
            break()
        endif()
    endforeach()
    if(NOT found)
        list(APPEND missingSpellings "${spelling}")
    endif()
endforeach()

if(missingSpellings)
    string(REPLACE ";" ", " missing "${missingSpellings}")
    message("")
    message("  ${refusalHeader} defines no `${missing}`.")
    message("")
    message("This check tallies `RefuseUntriaged` and requires the other two to exist. A")
    message("spelling that is gone makes both the rule and the backlog count meaningless,")
    message("so it is reported rather than passed over.")
    message(FATAL_ERROR "worker refusals: a refusal spelling is missing and the tally cannot conclude")
endif()

# ---------------------------------------------------------------------------
# Pass 3: the verdict.
if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}: calls EncodeErrorReply directly")
    endforeach()
    message("")
    message("A refusal answered while nothing rises is indistinguishable, on /metrics,")
    message("from a port nobody is talking to -- which is how six of them went unnoticed")
    message("(#327) and five more accumulated behind a green check (#447).")
    message("")
    message("Answer through one of the three spellings in ${refusalHeader}, and say")
    message("which fact you are asserting:")
    message("")
    message("    Cc::Refuse(metrics, row, detail)               a rise here means something")
    message("    Cc::RefuseWithoutCounter({ .why = \"...\" })     a rise would mean nothing, because")
    message("    Cc::RefuseUntriaged({ .issue = N })            nobody has decided yet; N will")
    message("")
    message("The third is a legitimate answer and is counted, not hidden: this check")
    message("reports the outstanding total on every run.")
    message("")
    message("The only files allowed to name the encoder directly are:")
    math(EXPR lastAllowed "${allowedFileCount} - 1")
    foreach(index RANGE ${lastAllowed})
        list(GET allowedFiles ${index} allowedFile)
        list(GET allowedReasons ${index} allowedReason)
        message("    ${allowedFile} -- ${allowedReason}")
    endforeach()
    message(FATAL_ERROR "worker refusals: ${violationCount} uncontrolled refusal site(s)")
endif()

list(LENGTH untriaged untriagedCount)
list(LENGTH issueTokens issueTokenCount)

# The backlog is read twice -- once from the calls, once from the issue each names --
# and a tally that reported one of two disagreeing readings would be worse than no
# tally. A call with no issue, or an issue outside a call, means this scan's idea of
# the construct has drifted from the construct.
if(NOT untriagedCount EQUAL issueTokenCount)
    message("")
    message("  ${untriagedCount} `RefuseUntriaged` call(s) but ${issueTokenCount} `.issue =` field(s).")
    message("")
    message("Every untriaged refusal names the issue that will decide it, so these two")
    message("counts are one fact read two ways. Disagreeing, neither can be reported as")
    message("the backlog -- and the backlog is the only reason the third spelling is safe")
    message("to have.")
    message(FATAL_ERROR "worker refusals: the triage tally read one fact two ways and they disagree")
endif()

if(untriagedCount EQUAL 0)
    message(STATUS "worker refusals: ${scannedCount} file(s) scanned, every refusal routed, none awaiting triage")
    return()
endif()

# Group the backlog by the issue that will decide it. A total alone says how much is
# undecided; the split says who decides it, and gives each issue a completion test
# that is measured rather than asserted.
set(issueList "")
foreach(token IN LISTS issueTokens)
    string(REPLACE "|" ";" parts "${token}")
    list(GET parts 0 owner)
    list(GET parts 1 name)
    if(name MATCHES "^[0-9]+$")
        set(number "${name}")
    elseif(DEFINED issueConstant_${owner}_${name})
        set(number "${issueConstant_${owner}_${name}}")
    else()
        set(number "unresolved")
    endif()
    list(APPEND issueList "${number}")
endforeach()

set(reported "")
message(STATUS "worker refusals: ${scannedCount} file(s) scanned, every refusal routed")
message(STATUS "worker refusals: ${untriagedCount} refusal site(s) awaiting triage")
foreach(number IN LISTS issueList)
    if("${number}" IN_LIST reported)
        continue()
    endif()
    list(APPEND reported "${number}")
    set(perIssue 0)
    foreach(other IN LISTS issueList)
        if("${other}" STREQUAL "${number}")
            math(EXPR perIssue "${perIssue} + 1")
        endif()
    endforeach()
    if("${number}" STREQUAL "unresolved")
        message(STATUS "worker refusals:   ${perIssue} naming an issue this scan could not resolve")
    else()
        message(STATUS "worker refusals:   ${perIssue} awaiting #${number}")
    endif()
endforeach()
