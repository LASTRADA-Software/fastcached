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
# Verified to distinguish the two: run against a binary built without the
# manifest, the same argument arrives as `67 72 FC 6E` and the match below fails.
#
# Inputs:
#   EXE  - the executable to run.
#   NAME - what to call it in a failure message.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED EXE OR NOT DEFINED NAME)
    message(FATAL_ERROR "[utf8-argv] EXE and NAME are required")
endif()

# Built from raw byte values rather than written as text, so no file's own
# encoding is what is under test -- not this script's, not CMake's reading of it.
# `g`, `r`, U+00FC as its two UTF-8 bytes, `n`.
string(ASCII 103 114 195 188 110 utf8Argument)

# An unrecognised OPTION, because every binary here answers one by echoing the
# token back: that is the shortest path from `argv` to something observable, it
# needs no daemon, no socket and no compiler, and it exits before any of them.
execute_process(
    COMMAND "${EXE}" "--${utf8Argument}"
    OUTPUT_VARIABLE stdoutText
    ERROR_VARIABLE stderrText
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
string(FIND "${stdoutText}${stderrText}" "${utf8Argument}" position)
if(position EQUAL -1)
    message(FATAL_ERROR
        "[utf8-argv] ${NAME} did not echo the argument back as the UTF-8 bytes it was given.\n"
        "An argument spelled with U+00FC reached this process as something else, so an\n"
        "operator-typed --toolchain, --advertise or --node-id cannot reach the fleet as\n"
        "text either (issue #155). On Windows this is the `activeCodePage` manifest not\n"
        "having been embedded -- see cmake/Utf8CodePage.cmake.\n"
        "  exit status: ${status}\n"
        "  stdout: ${stdoutText}\n"
        "  stderr: ${stderrText}")
endif()
