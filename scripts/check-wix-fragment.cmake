# SPDX-License-Identifier: Apache-2.0
#
# The WiX patch fragment must be well-formed XML, and the rule that breaks it is
# one nobody has in mind while writing a comment.
#
# **An XML comment may not contain a double hyphen.** Not at the end, not in the
# middle, not inside backticks: `--` is forbidden anywhere between `<!--` and
# `-->`. That collides head-on with how this repository writes comments, because
# every flag it documents is spelled with two of them -- `--seed-config`,
# `--install-service`, `--advertise` -- and an em-dash typed as `--` does it too.
#
# Measured: one comment naming `--seed-config` in packaging/windows/service-actions.xml
# took the `Package (Windows .msi)` job red with a parse error naming the LINE and
# not the reason, on a change whose subject was a config file. That job builds an
# MSI, runs only on a Windows runner, and is one of the slowest in the matrix -- so
# the feedback loop for a typo in a comment was a full CI cycle.
#
# This check needs no XML library, no compiler and no Windows: it walks the comment
# regions itself and refuses a `--` inside one. That is deliberately NARROWER than
# validating the document. Full validation would need a parser CMake does not have,
# and this is the rule that actually bit; a broader check nobody can run locally is
# the problem it replaces, not a fix for it.
#
# What it does NOT see, stated rather than left to be discovered: unbalanced tags,
# a bad attribute, an unescaped `&`. The Windows job remains the only thing that
# validates the document. This closes the one failure mode that is invisible while
# reading and cheap to make.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-wix-fragment.cmake
#
# Exit codes: 0 = every comment is legal. 1 = at least one is not.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(_dir "${FASTCACHED_SOURCE_DIR}/packaging/windows")
if(NOT IS_DIRECTORY "${_dir}")
    message(FATAL_ERROR "check-wix-fragment: ${_dir} is not a directory")
endif()

file(GLOB _fragments RELATIVE "${_dir}" "${_dir}/*.xml")

# Emptiness is its own outcome and a separate assertion, never an inference from
# "no problems found": a glob that stopped matching reports a clean run over
# nothing, which is a check announcing success for work it did not do.
list(LENGTH _fragments _count)
if(_count EQUAL 0)
    message(FATAL_ERROR
        "check-wix-fragment: no *.xml under ${_dir}, so this check proved nothing rather than passing.")
endif()

set(_offences "")
set(_comments 0)

foreach(_name IN LISTS _fragments)
    file(READ "${_dir}/${_name}" _text)

    # A `FIND`/`SUBSTRING` walk, never a list split. `file(STRINGS)` and every
    # newline-to-`;` conversion turn the text into a CMake LIST, where one `;` or
    # one unbalanced bracket merges elements and silently drops what follows --
    # which in a checker means a region it reports nothing about (#495). There is
    # no list here, so there is nothing to neutralise.
    set(_cursor 0)
    string(LENGTH "${_text}" _length)

    while(_cursor LESS _length)
        string(SUBSTRING "${_text}" ${_cursor} -1 _rest)
        string(FIND "${_rest}" "<!--" _open)
        if(_open EQUAL -1)
            break()
        endif()

        math(EXPR _bodyStart "${_cursor} + ${_open} + 4")
        string(SUBSTRING "${_text}" ${_bodyStart} -1 _afterOpen)
        string(FIND "${_afterOpen}" "-->" _close)
        if(_close EQUAL -1)
            # An unterminated comment is its own defect and is reported as one
            # rather than ending the walk quietly: a walk that simply stops has
            # nothing to say about the rest of the file, which is indistinguishable
            # from having read it and found nothing.
            list(APPEND _offences "${_name}: a comment opened and was never closed")
            break()
        endif()

        string(SUBSTRING "${_afterOpen}" 0 ${_close} _body)
        math(EXPR _comments "${_comments} + 1")

        string(FIND "${_body}" "--" _hyphens)
        if(NOT _hyphens EQUAL -1)
            # Report a slice around the offence rather than the whole comment: these
            # run to a dozen lines here, and a verdict nobody can locate is one
            # nobody acts on.
            math(EXPR _from "${_hyphens} - 30")
            if(_from LESS 0)
                set(_from 0)
            endif()
            string(LENGTH "${_body}" _bodyLength)
            math(EXPR _span "${_bodyLength} - ${_from}")
            if(_span GREATER 70)
                set(_span 70)
            endif()
            string(SUBSTRING "${_body}" ${_from} ${_span} _slice)
            string(REGEX REPLACE "[ \t\r\n]+" " " _slice "${_slice}")
            list(APPEND _offences "${_name}: ...${_slice}...")
        endif()

        math(EXPR _cursor "${_bodyStart} + ${_close} + 3")
    endwhile()
endforeach()

# The positive control, and it is not decoration: every clause above reports only on
# comments it FOUND, so a walk that found none would report nothing wrong about a
# file full of them. A fragment with no comment at all is not a state this repository
# has ever been in.
if(_comments EQUAL 0)
    message(FATAL_ERROR
        "check-wix-fragment: read ${_count} fragment(s) and found no comment in any of them. "
        "The walk matched nothing, so this check proved nothing rather than passing.")
endif()

if(_offences)
    string(REPLACE ";" "\n  " _offenceText "${_offences}")
    message(FATAL_ERROR
        "check-wix-fragment: an XML comment contains a double hyphen, which makes the fragment "
        "not well formed.\n"
        "  ${_offenceText}\n"
        "`--` is illegal anywhere inside <!-- ... -->, so a flag name cannot be spelled in one. "
        "Name the flag in prose and let the ExeCommand carry its real spelling, or reword the "
        "dash. CPack's WiX step fails on this with a parse error naming the line and not the "
        "reason, on a Windows runner, minutes into the slowest job in the matrix.")
endif()

message(STATUS
    "check-wix-fragment: ${_comments} comment(s) across ${_count} fragment(s) carry no illegal double hyphen")
