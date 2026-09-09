# SPDX-License-Identifier: Apache-2.0
#
# Does a non-ASCII argument survive the trip into `main()`?
#
# Windows transcodes every narrow string crossing an OS boundary through the
# process's ACTIVE CODE PAGE, whose default is the host's legacy one -- so
# `--advertise=gruen` spelled with a real umlaut reached `argv` as a CP-1252 byte,
# the fleet refused the registration for not being UTF-8, and the node logged that
# refusal every heartbeat while never joining (issue #155). Every executable in
# this tree now declares UTF-8 as its code page; `cmake/Utf8CodePage.cmake`
# carries why that is the fix and not a conversion at one boundary.
#
# This is the end-to-end proof of it, and it is the only one there can be: the
# defect is in what the OS hands a process, so nothing inside a process can
# observe it. `Platform/NarrowText_test.cpp` asserts the code page a test binary
# ended up with; this asserts what an argument becomes on the way in.
#
# ---------------------------------------------------------------------------
# The verdict is taken in BYTES, and never through a decoder
# ---------------------------------------------------------------------------
#
# This check reads the child's streams as raw bytes (`OUTPUT_FILE`, then
# `file(READ ... HEX)`) and compares hex. That is not fussiness -- it is the
# defect this check itself shipped with.
#
# `execute_process`'s `OUTPUT_VARIABLE` DECODES the child's bytes, and its default
# `ENCODING AUTO` decodes them using the code page of the CONSOLE the process was
# launched from. So the same executable passed or failed by which shell started
# `ctest`: on the host where this was found, PowerShell runs at 65001 and Git Bash
# at 437, and CI was green throughout because its runner is UTF-8 (#1134).
#
# The mangling was OUTBOUND, in this script's own harness, and it destroyed the
# evidence before the comparison: the correct UTF-8 bytes `c3 bc` were read as
# CP437 `box-drawing` characters and re-encoded to `e2 94 9c e2 95 9d`. Measured
# both ways -- the raw stream is byte-identical under the two code pages and
# carries `67 72 c3 bc 6e`, and that mangled sequence exists ONLY in the decoded
# copy. The property under test held the whole time; a real `cmd` command line
# delivers the correct bytes under CP437 too, because a Windows command line is
# UTF-16 end to end and `chcp` governs console I/O rather than `argv`.
#
# THE EXPECTATION, stated in words here rather than encoded in a flag, so that
# saying what this check means cannot alter what it measures: an argument spelled
# `--gr<U+00FC>n` must arrive in the child's `argv` as the five UTF-8 bytes
# `67 72 c3 bc 6e`. Nothing below re-states that; the needle is derived from the
# same string the child is handed.
#
# Two `ENCODING` values also fix the verdict and neither was chosen.
#
# `ENCODING NONE` decodes nothing and is correct. It was not taken because the
# comparison would still run over a CMake string, and because the DIAGNOSTIC then
# has to print `fc` -- a byte that is not valid UTF-8 -- through the same console
# that caused this ticket. Hex is ASCII and survives any console.
#
# `ENCODING UTF8` is correct for the verdict and WRONG for the diagnostic: `fc`
# is not valid UTF-8, so it decodes to U+FFFD and the one byte worth reporting is
# replaced by a replacement character.
#
# Both share the weakness that decided it: an `ENCODING` keyword is a guard
# called ALONGSIDE the operation, which a later edit can drop with nothing to
# notice, where a hex comparison cannot silently start decoding. The bug here was
# an instrument that transformed the evidence before comparing it, so the remedy
# removes the transformation rather than swapping it.
#
# **`ENCODING ANSI` is the trap, and it INVERTS this check.** It is the
# plausible-looking alternative -- "make it use the ANSI code page" -- and it
# decodes a manifest-less binary's CP-1252 `fc` into U+00FC, re-encodes it as
# `c3 bc`, and reports a PASS for exactly the defect #155 exists to catch, with
# nothing about that green looking wrong. REFUSED on a measurement, not untried.
#
# Inputs:
#   EXE  - the executable to run.
#   NAME - what to call it in a failure message; also namespaces the scratch
#          files, since the registrations run in parallel.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED EXE OR NOT DEFINED NAME)
    message(FATAL_ERROR "[utf8-argv] EXE and NAME are required")
endif()

# Built from raw byte values rather than written as text, so no file's own
# encoding is what is under test -- not this script's, not CMake's reading of it.
# `g`, `r`, U+00FC as its two UTF-8 bytes, `n`.
string(ASCII 103 114 195 188 110 utf8Argument)

set(scratchStdout "${CMAKE_CURRENT_BINARY_DIR}/utf8-argv-${NAME}.out")
set(scratchStderr "${CMAKE_CURRENT_BINARY_DIR}/utf8-argv-${NAME}.err")

# An unrecognised OPTION, because every binary here answers one by echoing the
# token back: that is the shortest path from `argv` to something observable, it
# needs no daemon, no socket and no compiler, and it exits before any of them.
#
# To FILES, not to variables. See the header: a variable is decoded.
execute_process(
    COMMAND "${EXE}" "--${utf8Argument}"
    OUTPUT_FILE "${scratchStdout}"
    ERROR_FILE "${scratchStderr}"
    RESULT_VARIABLE status
    TIMEOUT 30
)

# A non-numeric result is `execute_process` reporting that it could not run the
# program at all, which must not read as "the bytes did not match".
if(NOT status MATCHES "^[0-9]+$")
    message(FATAL_ERROR "[utf8-argv] could not run ${NAME}: ${status}")
endif()

# Either stream: the daemon and the test client print to stderr, and nothing here
# depends on which.
file(READ "${scratchStdout}" stdoutHex HEX)
file(READ "${scratchStderr}" stderrHex HEX)

# One space after every byte, in the haystack AND in the needle. Without it a
# search over a bare hex string can match ACROSS a byte boundary -- the bytes
# `a6 72 c3 bc 6e` contain the characters `6772c3bc6e` at an odd offset -- which
# is a false PASS reachable from content this check does not control.
string(REGEX REPLACE "(..)" "\\1 " haystack "${stdoutHex}${stderrHex}")

# The needle's bytes come from the same string the child was handed, through the
# same write-and-read-back, so there is no second spelling of it to drift.
set(needleFile "${CMAKE_CURRENT_BINARY_DIR}/utf8-argv-${NAME}.needle")
file(WRITE "${needleFile}" "${utf8Argument}")
file(READ "${needleFile}" needleHex HEX)
file(REMOVE "${needleFile}")
string(REGEX REPLACE "(..)" "\\1 " needle "${needleHex}")

# Nothing at all on either stream is a THIRD outcome, and it must not be reported
# as wrong bytes: it means the binary answered an unknown option by saying
# nothing, which is a change to the thing this check leans on rather than a text
# defect. Four states, not two.
if(haystack STREQUAL "")
    message(FATAL_ERROR
        "[utf8-argv] ${NAME} produced NO output on either stream, so this check\n"
        "observed nothing to judge -- that is not the same as the bytes being wrong.\n"
        "It relies on an unrecognised option being echoed back; if that behaviour\n"
        "changed, this check needs a different observation point, not a new expectation.\n"
        "  exit status: ${status}")
endif()

string(FIND "${haystack}" "${needle}" position)
if(position EQUAL -1)
    # Report the BYTES, never a rendering. What separates the two causes is the
    # one byte between `gr` and `n`: `fc` is a CP-1252 unit and means the manifest
    # is genuinely absent, anything else means something not yet diagnosed.
    string(FIND "${haystack}" "67 72 " grPosition)
    if(grPosition EQUAL -1)
        set(observed "the argument's leading `gr` (67 72) is not in either stream at all")
    else()
        string(LENGTH "${haystack}" haystackLength)
        math(EXPR remaining "${haystackLength} - ${grPosition}")
        set(window 24)
        if(remaining LESS ${window})
            set(window ${remaining})
        endif()
        string(SUBSTRING "${haystack}" ${grPosition} ${window} observedBytes)
        set(observed "what arrived, from `gr` onward: ${observedBytes}")
    endif()

    message(FATAL_ERROR
        "[utf8-argv] ${NAME} did not echo the argument back as the UTF-8 bytes it was given.\n"
        "An argument spelled with U+00FC reached this process as something else, so an\n"
        "operator-typed --toolchain, --advertise or --node-id cannot reach the fleet as\n"
        "text either (issue #155).\n"
        "  expected: ${needle}\n"
        "  ${observed}\n"
        "\n"
        "`67 72 fc 6e` is the `activeCodePage` manifest genuinely not having been\n"
        "embedded -- see cmake/Utf8CodePage.cmake. Any OTHER byte sequence is not that\n"
        "cause and the manifest is not where to look; this message named it\n"
        "unconditionally until #1134, and sent people to a file that was fine.\n"
        "\n"
        "This verdict is taken in bytes and does not depend on the console this ran\n"
        "from, which it did until #1134. Do not `fix` a red here by changing the code\n"
        "page, and do not reach for `ENCODING ANSI`, which reports a PASS for the very\n"
        "defect this checks for -- the script header has the measurements.\n"
        "  exit status: ${status}\n"
        "  raw streams kept at: ${scratchStdout}\n"
        "                       ${scratchStderr}")
endif()

file(REMOVE "${scratchStdout}" "${scratchStderr}")

message(STATUS "[utf8-argv] ${NAME} received the argument as the UTF-8 bytes ${needle}")
