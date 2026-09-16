# SPDX-License-Identifier: Apache-2.0
#
# No C++ source in this tree constructs from `std::istreambuf_iterator`.
#
# GCC 14 at `-O2` and above inlines that iterator's `sgetc` and then reports
# `-Werror=null-dereference` inside `<streambuf>` itself. It is a false positive, and
# one this project cannot silence: warnings are errors here and the rule is to fix
# them at the source rather than suppress them. The house spelling is a sized read --
# `FastCache::Cc::ReadFileBytes` in `src/apps/fastcache-cc/FileBytes.hpp` for bytes,
# `out << in.rdbuf()` for text -- which is also one allocation and one read instead of
# a per-character loop with geometric regrowth and then a full-size copy. The
# workaround is the better implementation regardless of the warning.
#
# ## Why a check and not a comment
#
# Because the comments were already there.
#
# Before this check, `git grep -n "istreambuf_iterator" -- 'src/'` at `ed295468` found
# **8 occurrences in 8 files: one use and SEVEN comments warning against it**. The
# pattern travels with the number deliberately -- #1029 records two people producing
# wrong counts for this in one hour, in opposite directions, neither of whom had
# miscounted: one described a worktree as though it were master, the other filtered
# comments with a `grep -v` that removed 5 of 7 and left a survivor that was still a
# comment. A census states its pattern, not only its number.
#
# `FileBytes.hpp` exists specifically to be the one spelling and carries the whole
# argument in its doc comment. That is about as loudly as a convention can be written
# down, and it still spread -- to `compile-cache-testclient/main.cpp`, and then to two
# more sites on a feature branch, one of which took `Linux-gcc-release` red at edge 495
# of 691 ([#1029](https://github.com/LASTRADA-Software/fastcached/issues/1029)).
#
# This is [#970](https://github.com/LASTRADA-Software/fastcached/issues/970)'s shape
# exactly. There, `producer | grep -q` under `pipefail` came back in eighteen sites
# across twelve scripts, five of which carried a comment explaining why they avoided
# the idiom, and the remedy was a scan rather than better prose. The tree's own
# sentence for it: *a rule stated in the files that obey it reaches no file that does
# not.*
#
# ## Why only a release build could see it
#
# `-Wnull-dereference` is in neither `-Wall` nor `-Wextra`, needs optimisation to
# fire, and `-fsyntax-only` cannot reach it. Both times, the author had run MSVC
# `cl-debug` and a clang-tidy sweep and both were green -- as was `clang-debug` with
# ASan and UBSan, and a cluster e2e running a binary built from that tree. A build
# also stops at the FIRST failure, so the second instance hid a byte-identical
# sibling behind it that was never reached; fixing only the named site would have
# relocated the failure one file along, where it presents as a regression introduced
# by the fix, with no failure history
# ([#172](https://github.com/LASTRADA-Software/fastcached/issues/172)).
#
# So this runs in the DEFAULT ctest set, where every developer meets it, rather than
# waiting for the one CI leg that compiles at `-O3`.
#
# ## A use, not a mention
#
# Seven of the eight original hits were prose. A scan that could not tell those apart
# would refuse the documentation of its own rule, with no escape hatch but rewording
# the comment -- which is a check people delete rather than obey. So comments are
# stripped before matching, line comments and block comments both, and what is left
# is code.
#
# That IS the exemption mechanism, and it is derived rather than listed. There is no
# table of excused files here, deliberately: a hand-kept list is exact about the files
# it knows and silent about the ones it does not, and silence reads identically to
# complete coverage ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492)).
# A file that both explains the rule in prose AND breaks it in code is refused, which
# is the shape a partial fix leaves behind.
#
# The blind spot, stated rather than papered over: the token inside a STRING LITERAL
# reads as code here and would be refused. Nothing in this tree does that, and the
# remedy if something ever needs to is to reword rather than to widen the check --
# CMake's regex engine has no lazy quantifier and a literal-aware C++ lexer written in
# `cmake -P` would be a bigger liability than the case it covers.
#
# ## Reading lines
#
# NOT through a list-splitting idiom, and that is a finding rather than a preference:
# writing this check's selftest showed the house reader of the day merged a line ending
# in a backslash with the next one, which its own comment claimed it prevented. The walk
# this check wrote instead now lives in `scripts/lib/CheckCommon.cmake` as
# `fastcached_scan_code_lines`, with the measurement and the consequences, because
# `check-ranges-seam.cmake` became its second caller.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-istreambuf-iterator.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The banned token. Spelled once: it is the thing being matched, the thing counted for
# the matched-nothing guard, and the thing named in the failure text.
set(bannedToken "istreambuf_iterator")

# The line walk is `fastcached_scan_code_lines` in `scripts/lib/CheckCommon.cmake`, which
# carries the measurement behind it: why it builds no CMake list, and the comment-ordering
# false green it has already had.

# Which C++ this REPOSITORY owns, asked of git rather than inferred from directory
# names. A dependency cache is untracked by construction, whatever a package manager
# calls it or wherever it puts it -- and libstdc++'s own headers are full of this
# iterator, which nobody here can edit.
# The file set this rule is about, found through the ONE enumeration (#1485). The work-tree
# probe, the git lookup, the `ls-files` call, the walk fallback and the build-tree exclusion
# list were a byte-identical copy in eight checks, and a copy is what can drift from the rule
# it enforces. The QUESTION stays here, where the rule is -- the pathspec and the glob are
# this check's own, and they are the same question twice so the two modes cover one set.
#
# The MODE travels with the answer and is stated in the status line and the refusal below. It
# has THREE values since the consolidation: a walk now says whether there was no index at all
# or whether the index named no file matching this question.

fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS "src/*.cpp" "src/*.hpp"
    GLOBS "src/*.cpp" "src/*.hpp"
    FILES_OUT sourceFiles
    MODE_OUT scanSource)

if(NOT sourceFiles)
    message("")
    message("  No C++ source was found under src/ in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "istreambuf-iterator: the scan matched no C++ sources and cannot conclude")
endif()

# Taken here rather than beside the success message, because the REFUSAL path names it
# too and a value computed after the block that reads it interpolates as empty -- the
# defect that had `local-gate.sh` refusing every tree while naming no compiler.
list(LENGTH sourceFiles fileCount)

set(violations "")
set(mentionCount 0)

foreach(relative IN LISTS sourceFiles)
    # `git ls-files` lists the INDEX, so a file deleted from the worktree and not yet
    # staged is still named here. Reading it emits `CMake Error ... failed to open for
    # reading`, which the registration's FAIL_REGULAR_EXPRESSION scores as THIS check
    # failing -- for a reason with nothing to do with the banned iterator.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()

    # A whole-file test first. 669 of this tree's 677 sources contain the token
    # nowhere, and splitting each of them into lines to discover that costs seconds on
    # every platform for nothing -- the same measurement that took the refusal scan
    # from 2.9s to 208ms.
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    string(FIND "${wholeFile}" "${bannedToken}" tokenPosition)
    if(tokenPosition EQUAL -1)
        continue()
    endif()

    fastcached_scan_code_lines("${wholeFile}" "${bannedToken}" hits)
    foreach(hit IN LISTS hits)
        if(hit MATCHES "^mention:([0-9]+)$")
            math(EXPR mentionCount "${mentionCount} + 1")
        elseif(hit MATCHES "^use:([0-9]+)$")
            list(APPEND violations "${relative}:${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

# A scan that found the token NOWHERE -- not even in the comments that document the
# rule -- is not a clean tree.
#
# This repository explains the ban in seven separate comments, including the doc
# comment on the helper header that exists to be the one spelling. Zero means a renamed
# token, a moved source root, or comment-stripping that has begun eating everything.
# Reported as its own failure rather than folded into success, for the reason
# `node-config-reference` refuses the same way: two empty results agree perfectly.
if(mentionCount EQUAL 0)
    message("")
    message("  `${bannedToken}` appears nowhere under src/, in code or in prose.")
    message("")
    message("This tree documents the ban in several comments and has a dedicated helper")
    message("header whose rationale names it. Zero occurrences means this scan is no")
    message("longer looking at what it thinks it is -- it is not evidence that no source")
    message("uses the iterator.")
    message(FATAL_ERROR "istreambuf-iterator: the scan matched nothing and cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}: constructs from `std::${bannedToken}`")
    endforeach()
    message("")
    message("GCC 14 at -O2 and above inlines that iterator's `sgetc` and then reports")
    message("`-Werror=null-dereference` inside <streambuf>. It is a false positive, and")
    message("this project cannot silence it: warnings are errors and the rule is to fix")
    message("them at the source.")
    message("")
    message("Use the house spelling instead:")
    message("")
    message("  bytes  ->  FastCache::Cc::ReadFileBytes(path)   (apps/fastcache-cc/FileBytes.hpp)")
    message("  text   ->  std::ostringstream out; out << in.rdbuf();")
    message("")
    message("Neither is only a workaround -- both are one allocation and one read,")
    message("against a per-character loop with geometric regrowth and then a copy.")
    message("")
    message("This runs in the default set because only an OPTIMISED build can see the")
    message("real failure: -Wnull-dereference is in neither -Wall nor -Wextra, and MSVC,")
    message("clang-tidy, ASan and UBSan were all green both times it shipped (#1029).")
    message("")
    # Named on the REFUSAL path too, not only on the passing one. Which enumeration
    # produced a verdict is part of the verdict: a fixture that stages synthetic trees
    # takes the directory walk while CI takes git, and a check that says so only when
    # it passes cannot be held to the mode it actually used when it failed.
    message("Enumerated ${fileCount} C++ source(s) via ${scanSource}.")
    message(FATAL_ERROR "istreambuf-iterator: ${violationCount} site(s) construct from the banned iterator")
endif()

message(STATUS
    "istreambuf-iterator: ${mentionCount} mention(s) across ${fileCount} C++ source(s) via ${scanSource}, "
    "all prose, none constructing")
