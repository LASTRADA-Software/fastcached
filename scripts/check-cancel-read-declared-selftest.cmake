# SPDX-License-Identifier: Apache-2.0
#
# `cancel-read-declared` must be SEEN to refuse, on each thing it claims and on nothing
# else. A guard nobody has watched refuse is not a guard -- and a guard nobody has
# watched ACCEPT is not known to work either, which is #1031's lesson and why case 1 is
# not decoration.
#
# Eight cases:
#
#   1. compliant   -- a transport that declares `CancelRead` PASSES, and the report says
#                     how much was looked at. A check that refuses everything is exactly
#                     as useless as one that refuses nothing, and looks like rigour.
#   2. missing     -- a transport that declares none is refused, NAMING the file and the
#                     class. This is the real defect: two of six transports inherited
#                     the default no-op and both parked (#710, #892).
#   3. commented   -- a `CancelRead` that appears only inside a `//` comment is refused.
#                     A COMMENT IS NOT A DECLARATION (#720), and the check strips
#                     comments before it measures anything.
#   4. neighbour   -- TWO transports in one header where the FIRST omits `CancelRead`
#                     and the SECOND declares it. This is the case a clean tree cannot
#                     exhibit and the reason the scan bounds each class's region: without
#                     the boundary the second's answer covers the first's silence, and
#                     the check reports green over a live violation.
#  4b. blockcommented -- the `/* */` spelling of case 3. Its own case because it was its
#                     own hole: only `//` was stripped, so a Doxygen `/** ... */` block
#                     answered for its class while case 3 went on passing.
#  4c. trailinghelper -- a `CancelRead` CALL after the LAST class in the header. The
#                     region used to run to EOF, so an ordinary trailing helper covered
#                     a transport that declared nothing -- and this needs no neighbour,
#                     which is why case 4's boundary never reached it.
#   5. noheaders   -- a tree with no `Net/*.hpp` is refused as the CHECK being broken.
#   6. nobase      -- a tree full of headers in which nothing derives from `ISocket` is
#                     refused too, and that is a DIFFERENT question from case 5: a
#                     renamed base leaves the header count healthy and the transport
#                     count at zero, and "no violations" over an empty set is the most
#                     confident wrong answer available (#492).
#
# Cases 5 and 6 are the ones a reader is most likely to think redundant. They are not:
# each is reachable without the other, and each would otherwise report that every
# transport answers, forever.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-cancel-read-declared-selftest.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR is not set.")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR is not set.")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-cancel-read-declared.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(caseCount 0)

# Build a tree holding one `Net/` header with the given contents.
# @param name     Sub-directory under the scratch root.
# @param fileName What to call the header, or "" for no header at all.
# @param contents The header text.
# @param outVar   Set to the tree root.
function(fastcached_make_tree name fileName contents outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Net")
    if(NOT fileName STREQUAL "")
        file(WRITE "${tree}/src/FastCache/Net/${fileName}" "${contents}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# @param tree        Tree to run the check against.
# @param outObjected TRUE when the check printed a CMake Error.
# @param outOutput   Everything it printed, FLATTENED.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    # Flattened before matching, because CMake WRAPS its diagnostics at ~74 columns: a
    # phrase can exist in the output and in no single LINE of it, and the phrases this
    # selftest matches on are long enough to straddle. A negative test written without
    # this reads as a refutation. The reasoning and the measurement are in
    # `.agent/rules/build-and-toolchain.md`; the sibling
    # `check-read-buffer-guard-selftest.cmake` flattens for the same reason and says so.
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

set(compliantHeader
"#pragma once

namespace FastCache
{

class ExampleSocket final: public ISocket
{
  public:
    IoAwaitable Read(std::span<std::byte> buffer) override;
    void CancelRead() noexcept override;
    void Close() noexcept override;
};

} // namespace FastCache
")

# ---------------------------------------------------------------------------
# 1. A compliant tree passes, and the report says how much it looked at.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("compliant" "ExampleSocket.hpp" "${compliantHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "compliant: a transport that declares CancelRead was refused -- the check refuses everything")
else()
    string(FIND "${output}" "1 transport(s)" counted)
    if(counted EQUAL -1)
        list(APPEND failures "compliant: the check passed but did not report the transport it read")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. A transport that declares none is refused, and named.
math(EXPR caseCount "${caseCount} + 1")
string(REPLACE "    void CancelRead() noexcept override;\n" "" missingHeader "${compliantHeader}")
fastcached_make_tree("missing" "ExampleSocket.hpp" "${missingHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "missing: a transport inheriting the default no-op was NOT refused")
else()
    string(FIND "${output}" "ExampleSocket derives from ISocket" named)
    if(named EQUAL -1)
        list(APPEND failures "missing: refused, but without naming the class -- a reader cannot act on it")
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. A declaration that exists only inside a comment is refused.
math(EXPR caseCount "${caseCount} + 1")
string(REPLACE "    void CancelRead() noexcept override;"
               "    // CancelRead is inherited on purpose, honest"
               commentedHeader "${compliantHeader}")
fastcached_make_tree("commented" "ExampleSocket.hpp" "${commentedHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "commented: a CancelRead named only in a comment satisfied the check -- comments are not declarations")
endif()

# ---------------------------------------------------------------------------
# 4. A neighbour's answer must not cover this class's silence.
math(EXPR caseCount "${caseCount} + 1")
set(neighbourHeader
"#pragma once

namespace FastCache
{

class SilentSocket final: public ISocket
{
  public:
    IoAwaitable Read(std::span<std::byte> buffer) override;
    void Close() noexcept override;
};

class AnsweringSocket final: public ISocket
{
  public:
    IoAwaitable Read(std::span<std::byte> buffer) override;
    void CancelRead() noexcept override;
    void Close() noexcept override;
};

} // namespace FastCache
")
fastcached_make_tree("neighbour" "TwoSockets.hpp" "${neighbourHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "neighbour: the SECOND transport's CancelRead covered the FIRST one's silence -- the region boundary is not holding")
else()
    string(FIND "${output}" "SilentSocket derives from ISocket" namedFirst)
    if(namedFirst EQUAL -1)
        list(APPEND failures "neighbour: refused, but did not name SilentSocket -- it may be objecting to the wrong class")
    endif()
    string(FIND "${output}" "AnsweringSocket derives from ISocket" namedSecond)
    if(NOT namedSecond EQUAL -1)
        list(APPEND failures "neighbour: refused AnsweringSocket, which declares CancelRead -- the region is over-reaching")
    endif()
endif()

# ---------------------------------------------------------------------------
# 4b. The BLOCK-comment spelling of case 3, and it is a separate case because it was a
# separate hole: stripping only `//` left `/* CancelRead */` and every `/** ... */`
# Doxygen block answering for the class that contains it. Case 3 passed throughout, so
# "a comment is not a declaration" read as enforced while one of the two spellings of a
# comment was not.
math(EXPR caseCount "${caseCount} + 1")
string(REPLACE "    void CancelRead() noexcept override;"
               "    /** CancelRead is inherited on purpose, honest */"
               blockCommentedHeader "${compliantHeader}")
fastcached_make_tree("blockcommented" "ExampleSocket.hpp" "${blockCommentedHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "blockcommented: a CancelRead named only inside a /* */ comment satisfied the check -- only the // spelling is stripped")
endif()

# ---------------------------------------------------------------------------
# 4c. The LAST class in a header is bounded by its own closing `};`, not by the end of
# the file. Its region used to run to EOF, so an ordinary trailing helper answered for
# a transport that declared nothing -- and unlike case 4 this needs no neighbour, which
# is why the `class`/`struct` boundary never covered it.
math(EXPR caseCount "${caseCount} + 1")
set(trailingHelperHeader
"#pragma once

namespace FastCache
{

class SilentSocket final: public ISocket
{
  public:
    IoAwaitable Read(std::span<std::byte> buffer) override;
    void Close() noexcept override;
};

inline void RetireBoth(ISocket& a, ISocket& b) noexcept
{
    a.CancelRead();
    b.CancelRead();
}

} // namespace FastCache
")
fastcached_make_tree("trailinghelper" "SilentSocket.hpp" "${trailingHelperHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "trailinghelper: a CancelRead CALL after the last class satisfied it -- the region runs past the closing brace")
else()
    string(FIND "${output}" "SilentSocket derives from ISocket" namedTrailing)
    if(namedTrailing EQUAL -1)
        list(APPEND failures "trailinghelper: refused, but did not name SilentSocket")
    endif()
endif()

# ---------------------------------------------------------------------------
# 5. No header at all is the CHECK being broken, not the tree being clean.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("noheaders" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "noheaders: an empty Net/ was reported as compliant, which is the check reporting on nothing")
endif()

# ---------------------------------------------------------------------------
# 6. Headers present, nothing deriving from ISocket: a different empty set.
math(EXPR caseCount "${caseCount} + 1")
set(noBaseHeader
"#pragma once

namespace FastCache
{

class ExampleSocket final: public ISomethingElse
{
  public:
    void CancelRead() noexcept override;
};

} // namespace FastCache
")
fastcached_make_tree("nobase" "ExampleSocket.hpp" "${noBaseHeader}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "nobase: a tree in which nothing derives from ISocket was reported as compliant")
else()
    string(FIND "${output}" "found no class deriving" saidWhy)
    if(saidWhy EQUAL -1)
        list(APPEND failures "nobase: refused, but not for the empty-set reason -- the two empty scans must not collapse")
    endif()
endif()

# ---------------------------------------------------------------------------
# The count is printed whether or not anything failed: a self-test that stops early
# must not look like one that judged something (#720).
list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    set(rendered "")
    foreach(failure IN LISTS failures)
        string(APPEND rendered "\n  - ${failure}")
    endforeach()
    message(FATAL_ERROR
        "cancel-read-declared-selftest: ${caseCount} case(s) ran, ${failureCount} failed:${rendered}")
endif()

message(STATUS "cancel-read-declared-selftest: ${caseCount} case(s) ran, 0 failed")
