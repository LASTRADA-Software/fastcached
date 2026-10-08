# SPDX-License-Identifier: Apache-2.0
#
# Version resolution: the git tag is the single source of truth. Cutting a
# release is `git tag -a v1.2.3 -m ...` followed by a push; nothing in the tree
# restates that number.
#
# There used to be a committed version.txt, and because it outranked every other
# source it was the *real* source of truth: a second version carrier that every
# release had to remember to bump in lock-step with the tag, and that pinned
# every build, every wire banner and every package to 0.0.1 for as long as it
# existed. It is gone, and `ctest -R repository-hygiene` fails if it is ever
# tracked again — see scripts/check-repository-hygiene.cmake. A build with no
# tag to describe (an exported source tarball, say) states its version with
# -DFASTCACHED_VERSION=1.2.3 instead.
#
# Two values come out of this and they are NOT interchangeable:
#
#   triple  strictly numeric MAJOR.MINOR.PATCH, no suffix, ever. It feeds
#           project(VERSION), which rejects anything else, and
#           CPACK_PACKAGE_VERSION, and from there the MSI ProductVersion
#           (major/minor < 256, patch < 65536), the RPM `Version:` field (where
#           a '-' is illegal) and the Debian version (where a '-' starts the
#           package revision).
#   string  human-facing, and carries whatever the winning source knows. A build
#           twelve commits past v0.1.0 reads 0.1.0-12-gdeadbee, with -dirty
#           appended when the tree had uncommitted changes as cmake ran. It is
#           only ever substituted into text — the memcached `version` reply, the
#           RESP INFO banner, `--version` — never into a package field.
#
# Nothing here re-runs on a new commit: the version is a configure-time
# snapshot, deliberately. A CMAKE_CONFIGURE_DEPENDS on .git/HEAD would
# reconfigure the project after every commit and, because the commit distance is
# part of the string, rewrite Core/Version.hpp and recompile every translation
# unit that includes it. Release builds configure from scratch, so a shipped
# number is always current; a developer's stale banner costs nothing. Re-run
# cmake to refresh it.

# ---------------------------------------------------------------------------
# Declared constants and located tools.

# The triple a build reports when nothing available can say what it is: no tag,
# no -DFASTCACHED_VERSION. 0.0.0 and not the 0.0.1 the deleted version.txt used
# to claim, because 0.0.0 is a number no release will ever carry: it reads as
# "this build does not know", sorts below every real release, and is legal in
# every field the triple reaches. Deliberately not a cache variable — a knob
# here would be one more place a version could come from.
set(FastCachedFallbackVersionTriple "0.0.0")

# Escalate "could not resolve a version from a tag" from a warning to a hard
# error. OFF everywhere except the CI jobs that publish artifacts: shipping
# fastcached-0.0.0-Linux-x86_64.deb because a tag fetch silently broke is worse
# than failing the job that would have built it. See .github/workflows/build.yml.
option(FASTCACHED_REQUIRE_EXACT_VERSION
    "Fail configuration when the version cannot be resolved from a git tag" OFF)

# How long one git query may take, in seconds. A PARAMETER rather than a literal
# so the seam test can drive a timeout without waiting 20 s per case; it is not a
# cache variable, so nothing typed at configure time raises it. Do not raise it to
# accommodate a slow work tree: a query that does not answer is a supported,
# reported outcome, and a larger budget only hides the cost. The usual cost is a
# work tree that Windows git and WSL git both use, measured in
# .agent/rules/build-and-toolchain.md.
if(NOT DEFINED FastCachedGitTimeoutSeconds)
    set(FastCachedGitTimeoutSeconds 20)
endif()

# The ways a git query can end other than with an answer, one row each, tried IN ORDER:
#
#   <kind> | <outcome> | <match> | <what happened, for the query's own warning> | <what to do>
#
# <match> is a regular expression over execute_process's RESULT_VARIABLE: an exit status
# when git ran to completion, and CMake's own words for how it ended otherwise. Those words,
# measured on CMake 4.2 (Linux) and 4.3.1 (Windows), and on 3.28.3 (Linux) where it differs:
#
#   a timeout          "Process terminated due to timeout", on both
#   git did not start  "no such file or directory" and "permission denied" (libuv's spawn
#                      errors, on both); "unknown error" (Windows, a file that is no image).
#                      CMake 3.28.3 capitalises the first letter: "No such file or
#                      directory" (a missing file) and "Permission denied" (a file that
#                      is not executable, or a directory). MATCHES is case-sensitive, so
#                      `unrunnable` takes that letter in either case (#1642). `[Uu]` is
#                      by analogy only: "unknown error" is a Windows word, and no Windows
#                      CMake 3.28 was measured
#   git died, Linux    SIGKILL "Subprocess killed", SIGSEGV "Segmentation fault",
#                      SIGTERM "Subprocess terminated", SIGABRT "Subprocess aborted"
#   git died, Windows  0xC0000005 "Access violation", 0xC000013A "User interrupt",
#                      0xC00000FD "Stack overflow", 0xC000001D "Illegal instruction",
#                      0xC0000409 "Exit code 0xc0000409" (a code CMake has no name for)
#
# Those are every death measured, not every death there is: `died` catches the rest.
#
# `died` matches ANYTHING and is the last row, so a way of ending no row names lands there
# rather than in the nearest neighbour: before it, every non-timeout was "could not run",
# which sent the operator to GIT_EXECUTABLE for a git that had started and crashed. Its
# words quote CMake's, and a start failure it turns out to catch belongs in `unrunnable`'s
# <match>. <outcome> is what FastCachedRunGit reports: `failed` is git answering "no",
# `unanswered` is no answer at all.
#
# `@QUERY@`, `@RESULT@`, `@BOUND@` and `@GIT@` are filled in per query. A kind is a row
# rather than a branch because each needs its OWN words: a git that never started is not
# a timeout, and blaming the Windows/WSL index re-hash for a missing binary sends an
# operator to the wrong machine. Fields are separated by " | ", so a <match>'s own
# alternation is written without spaces; no row may contain a ';' (these are CMake lists).
set(FastCachedGitNonAnswers
    "failed | failed | ^[0-9]+$ | failed `@QUERY@` (exit @RESULT@) | run the quoted git command in the source tree to see why it failed"
    "timeout | unanswered | due to timeout$ | did not answer `@QUERY@` within @BOUND@ s (@RESULT@) | re-run cmake once git answers. A work tree that Windows git and WSL git both use re-hashes every tracked file on each switch, see .agent/rules/build-and-toolchain.md"
    "unrunnable | unanswered | ^([Nn]o such file or directory|[Pp]ermission denied|[Uu]nknown error)$ | could not run `@QUERY@` (@RESULT@, GIT_EXECUTABLE is @GIT@) | check GIT_EXECUTABLE: a cached path to a git that has since moved or gone is the usual cause"
    "died | unanswered | . | ended `@QUERY@` without an answer (@RESULT@) | run the quoted git command in the source tree. The words in parentheses are CMake's for how it ended: a crash or a signal, or a way of failing to start that the unrunnable row does not name yet"
)

## Split one FastCachedGitNonAnswers row into its fields.
## @param Row The row.
## @param Prefix Receives <Prefix>Kind, <Prefix>Outcome, <Prefix>Match, <Prefix>Happened and
##        <Prefix>Advice in the caller's scope.
# Written once because the classifier and the final report both read the table, and a row
# with a field too many or too few is refused here rather than read shifted by one.
function(FastCachedGitNonAnswerFields Row Prefix)
    string(REPLACE " | " ";" fields "${Row}")
    list(LENGTH fields count)
    if(NOT count EQUAL 5)
        message(FATAL_ERROR "FastCachedGitNonAnswers: a row has 5 fields separated by ' | ', this one has ${count}: ${Row}")
    endif()
    set(index 0)
    foreach(name IN ITEMS Kind Outcome Match Happened Advice)
        list(GET fields ${index} value)
        set(${Prefix}${name} "${value}" PARENT_SCOPE)
        math(EXPR index "${index} + 1")
    endforeach()
endfunction()

## The FastCachedGitNonAnswers row a RESULT_VARIABLE other than "0" belongs to.
## @param Result The RESULT_VARIABLE.
## @param Prefix Receives the winning row's fields, named as FastCachedGitNonAnswerFields names
##        them, in the caller's scope.
# The first row whose <match> the result satisfies wins: an exit status is git saying no, and
# anything else is CMake's words for a git that did not finish, told apart because their
# remedies are on different machines. A function of its own so a check can hand it the words
# of a CMake it is not running on (scripts/check-version-git-unanswered.cmake).
function(FastCachedGitNonAnswerOf Result Prefix)
    foreach(row IN LISTS FastCachedGitNonAnswers)
        FastCachedGitNonAnswerFields("${row}" row)
        if(Result MATCHES "${rowMatch}")
            foreach(name IN ITEMS Kind Outcome Match Happened Advice)
                set(${Prefix}${name} "${row${name}}" PARENT_SCOPE)
            endforeach()
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "FastCachedGitNonAnswers has no row for `${Result}`: its last row must match anything")
endfunction()

# git, located once. find_program and not find_package(Git): this module is
# included before project(), where find_package's toolchain-dependent machinery
# has nothing to stand on, while a PATH search for a program has no such
# dependency. The cache entry uses the name FindGit would have used, so a later
# find_package(Git) reuses it, and src/tests/CMakeLists.txt hands this very
# binary to the repository-hygiene test rather than looking git up a second time.
find_program(GIT_EXECUTABLE NAMES git DOC "git command line client")

# ---------------------------------------------------------------------------
# Version sources, in precedence order. One row per source:
#
#   <label>|<resolver>|<authority>|<advice>
#
#   label      printed as the version source
#   resolver   function(RequestedTripleName RequestedStringName
#                       TripleOutVar StringOutVar DetailOutVar)
#              Fills the three out-variables in the caller's scope, or leaves the
#              triple empty when it cannot answer, in which case the loop below
#              tries the next row. The first two arguments are the variable names
#              the caller asked to have filled; only the override resolver reads
#              them, and every resolver accepts them so the dispatch stays one
#              uniform call instead of a special case per row.
#   authority  Exact       the value names a release, or the caller stated it
#              Provisional the value is a stand-in, and is reported as such
#   advice     printed with the report; for a Provisional row, the remedy
#
# A fifth source is a row plus its resolver. The ordering, the reporting, the
# severity and the validation are each written once, in GetVersionInformation.
#
# No row may contain a ';' — these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedVersionSources
    "explicit override|FastCachedVersionFromCache|Exact|The version was stated on the cmake command line, so nothing in the tree was consulted."
    "git tag|FastCachedVersionFromGitTag|Exact|The tag is the single source of truth for the version."
    "git commit without a matching tag|FastCachedVersionFromGitCommit|Provisional|Create the first release tag with 'git tag -a v0.1.0 -m fastcached-0.1.0' and push it. In CI, check out with fetch-depth 0 so both the tags and enough history to reach them arrive on the runner."
    "declared fallback|FastCachedVersionFallback|Provisional|Build from a git work tree that has tags, or state the version explicitly with -DFASTCACHED_VERSION=1.2.3 — an exported source tarball has no other way to know."
)

# Field limits the triple has to satisfy. One row per limit:
#
#   <field>|<index in the triple>|<maximum>|<what breaks above it>
#
# These are hard constraints of a packaging format rather than opinions, and they
# are checked here so a bad tag is caught at configure time on every platform
# instead of by WiX, at release time, on Windows only.
set(FastCachedVersionFieldLimits
    "major|0|255|the MSI ProductVersion major field is 8 bits wide"
    "minor|1|255|the MSI ProductVersion minor field is 8 bits wide"
    "patch|2|65535|the MSI ProductVersion build field is 16 bits wide"
)

# ---------------------------------------------------------------------------
# Helpers.

## Run git in the source tree and return its stripped standard output.
## @param OutputVar Name of the variable to receive the output. Set to the empty
##        string unless git answered.
## @param OutcomeVar Name of the variable to receive HOW the query went:
##        `answered` (git exited 0), `failed` (git ran and exited non-zero, or is
##        absent), or `unanswered` (git did not finish -- a timeout, a git that
##        could not start, or one that died). The table FastCachedGitNonAnswers decides.
## @param LOSS What this caller loses when the query does not answer, as a
##        sentence. A caller names its OWN loss, because only some of these
##        queries make the version fall back.
## @param ANSWER_ON_FAILURE Given when a non-zero exit is an ordinary answer (no
##        tag, not a work tree). Without it, `failed` is reported like `unanswered`.
## @param ARGS The git arguments.
# Written once because every call below needs exactly this. The previous version
# of this file grew four copy-pasted execute_process blocks, two of which ran in
# CMAKE_CURRENT_SOURCE_DIR while the others used CMAKE_SOURCE_DIR.
#
# A query that did not answer is recorded in the global property
# FastCachedVersionUnanswered, which GetVersionInformation reads, so a version
# built from a partial answer is never reported as Exact. Reading an unanswered
# query as its NEGATIVE -- "no distance", "clean" -- is what let a timed-out dirty
# check publish a string claiming a clean tree under the Exact severity.
function(FastCachedRunGit OutputVar OutcomeVar)
    cmake_parse_arguments(PARSE_ARGV 2 git "ANSWER_ON_FAILURE" "LOSS" "ARGS")
    set(${OutputVar} "" PARENT_SCOPE)
    if(NOT GIT_EXECUTABLE)
        set(${OutcomeVar} "failed" PARENT_SCOPE)
        return()
    endif()

    # TIMEOUT, because this is the only unbounded subprocess left in the
    # configure path and a configure that blocks here produces NO output at all:
    # no compile starts, nothing is written, and the build looks hung rather than
    # slow. git is not a pure computation on a local directory -- it takes
    # repository locks, and `gc --auto` can be repacking underneath it -- so
    # "it is only `git describe`" is not a bound.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${git_ARGS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE commandOutput
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        TIMEOUT ${FastCachedGitTimeoutSeconds}
        RESULT_VARIABLE commandResult
    )
    if(commandResult STREQUAL "0")
        set(${OutputVar} "${commandOutput}" PARENT_SCOPE)
        set(${OutcomeVar} "answered" PARENT_SCOPE)
        return()
    endif()

    FastCachedGitNonAnswerOf("${commandResult}" row)
    set(kind "${rowKind}")
    set(outcome "${rowOutcome}")
    set(happened "${rowHappened}")
    set(${OutcomeVar} "${outcome}" PARENT_SCOPE)
    if(outcome STREQUAL "failed" AND git_ANSWER_ON_FAILURE)
        return()
    endif()

    list(JOIN git_ARGS " " query)
    string(REPLACE "@QUERY@" "git ${query}" happened "${happened}")
    string(REPLACE "@RESULT@" "${commandResult}" happened "${happened}")
    string(REPLACE "@BOUND@" "${FastCachedGitTimeoutSeconds}" happened "${happened}")
    string(REPLACE "@GIT@" "${GIT_EXECUTABLE}" happened "${happened}")
    set_property(GLOBAL APPEND PROPERTY FastCachedVersionUnanswered "git ${query}")
    set_property(GLOBAL APPEND PROPERTY FastCachedVersionUnansweredKinds "${kind}")
    message(WARNING "fastcached: git ${happened}: ${git_LOSS}")
endfunction()

## Suffix marking a work tree with uncommitted changes to tracked files.
## @param OutputVar Name of the variable to receive "-dirty", "" or, when git did
##        not answer, "-dirty-unknown" -- never "" for a question nobody answered.
# Untracked files deliberately do not count: out/, .cache/ and a local
# version.txt are all legitimate and none of them changes what the source says.
# Written once because both git resolvers append the same marker.
function(FastCachedGitDirtyMarker OutputVar)
    FastCachedRunGit(pendingChanges outcome
        LOSS "whether the tree has uncommitted changes is unknown, so the version string says -dirty-unknown"
        ARGS status --porcelain --untracked-files=no)
    if(NOT outcome STREQUAL "answered")
        set(${OutputVar} "-dirty-unknown" PARENT_SCOPE)
    elseif(pendingChanges STREQUAL "")
        set(${OutputVar} "" PARENT_SCOPE)
    else()
        set(${OutputVar} "-dirty" PARENT_SCOPE)
    endif()
endfunction()

## Refuse a triple that project(VERSION) or a packaging format cannot carry.
## @param Triple The resolved MAJOR.MINOR.PATCH candidate.
## @param Origin How it was obtained, for the diagnostic.
function(FastCachedRequireVersionTriple Triple Origin)
    if(NOT Triple MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
            "Version triple '${Triple}' (from ${Origin}) is not a bare numeric "
            "MAJOR.MINOR.PATCH. project(VERSION) rejects anything else, the RPM "
            "Version field may not contain a '-', and Debian reads a '-' as the "
            "start of the package revision. Suffixes belong in the version "
            "string, never in the triple.")
    endif()

    # Above the MSI limits WiX fails at release time with a diagnostic that names
    # neither the tag nor this file, so on Windows this is fatal. Elsewhere it is
    # a warning: the constraint is real but it is not theirs, and refusing to
    # build a fork that tags v2024.1.0 on Linux would be gratuitous. One message
    # either way — the severity is the only thing that varies.
    if(WIN32)
        set(severity FATAL_ERROR)
    else()
        set(severity WARNING)
    endif()

    string(REPLACE "." ";" fields "${Triple}")
    foreach(limitRow IN LISTS FastCachedVersionFieldLimits)
        string(REPLACE "|" ";" limit "${limitRow}")
        list(GET limit 0 fieldName)
        list(GET limit 1 fieldIndex)
        list(GET limit 2 fieldMaximum)
        list(GET limit 3 fieldReason)
        list(GET fields "${fieldIndex}" fieldValue)

        if(fieldValue GREATER fieldMaximum)
            message(${severity}
                "Version triple '${Triple}' (from ${Origin}) has "
                "${fieldName}=${fieldValue}, above the maximum of "
                "${fieldMaximum}: ${fieldReason}. No Windows MSI can be built "
                "from this version.")
        endif()
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Resolvers. Each honours the contract documented on FastCachedVersionSources.

## Resolver: the version stated on the cmake command line, e.g.
##   cmake -DFASTCACHED_VERSION=1.2.3 [-DFASTCACHED_VERSION_STRING=1.2.3-vendor1]
## @param RequestedTripleName Cache entry holding the triple override.
## @param RequestedStringName Cache entry holding the string override.
## @param TripleOutVar Name of the variable to receive the triple.
## @param StringOutVar Name of the variable to receive the version string.
## @param DetailOutVar Name of the variable to receive the source detail.
# The names are not spelled out here: the override for a value is the very
# variable the caller asked to have filled, so there is no second name to keep in
# sync with the call in the top-level CMakeLists.txt. A distro packager building
# an exported tarball has neither a tag nor a .git and needs exactly this.
function(FastCachedVersionFromCache RequestedTripleName RequestedStringName
                                    TripleOutVar StringOutVar DetailOutVar)
    set(${TripleOutVar} "" PARENT_SCOPE)
    set(${StringOutVar} "" PARENT_SCOPE)
    set(${DetailOutVar} "" PARENT_SCOPE)

    # Double dereference rather than get_property(... CACHE ...): it reads a
    # normal variable and a cache entry alike, so -D works in both project and
    # script mode, and a parent listfile could set the value directly. It also
    # avoids the trap that sank the first attempt here — get_property leaves its
    # output variable *undefined* when the entry does not exist, and
    # `if(undefinedVar STREQUAL "")` compares the literal name "undefinedVar"
    # against "" and is therefore false, so every empty-check silently inverted.
    # Seeding both with "" keeps the checks below value comparisons.
    set(overrideTriple "")
    set(overrideString "")
    if(DEFINED ${RequestedTripleName})
        set(overrideTriple "${${RequestedTripleName}}")
    endif()
    if(DEFINED ${RequestedStringName})
        set(overrideString "${${RequestedStringName}}")
    endif()

    if(overrideTriple STREQUAL "")
        if(NOT overrideString STREQUAL "")
            message(FATAL_ERROR
                "-D${RequestedStringName} was given without "
                "-D${RequestedTripleName}. The string alone cannot be used: the "
                "numeric triple is what project(VERSION) and every packaging "
                "format need, and it cannot be derived from an arbitrary "
                "string. Pass both.")
        endif()
        return()
    endif()

    if(overrideString STREQUAL "")
        set(overrideString "${overrideTriple}")
    endif()

    set(${TripleOutVar} "${overrideTriple}" PARENT_SCOPE)
    set(${StringOutVar} "${overrideString}" PARENT_SCOPE)
    set(${DetailOutVar} "-D${RequestedTripleName}=${overrideTriple}" PARENT_SCOPE)
endfunction()

## Resolver: the nearest git tag reachable from HEAD.
## @param RequestedTripleName Unused by this resolver.
## @param RequestedStringName Unused by this resolver.
## @param TripleOutVar Name of the variable to receive the triple.
## @param StringOutVar Name of the variable to receive the version string.
## @param DetailOutVar Name of the variable to receive the source detail.
function(FastCachedVersionFromGitTag RequestedTripleName RequestedStringName
                                     TripleOutVar StringOutVar DetailOutVar)
    set(${TripleOutVar} "" PARENT_SCOPE)
    set(${StringOutVar} "" PARENT_SCOPE)
    set(${DetailOutVar} "" PARENT_SCOPE)

    # --tags so a lightweight `git tag v1.2.3` counts too — git describe
    #   considers only annotated tags without it.
    # --match v[0-9]* and not v*: with v*, a tag such as `vendor-drop` becomes
    #   the nearest tag, fails the pattern below, and drops the build to the
    #   fallback while a perfectly good release tag sits one commit further back.
    # --abbrev=0 yields the tag name alone, which is where the triple comes from.
    FastCachedRunGit(nearestTag outcome ANSWER_ON_FAILURE
        LOSS "the release tag is unknown, so the version falls back past the git-tag row"
        ARGS describe --tags --abbrev=0 --match "v[0-9]*")
    if(nearestTag MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(triple "${CMAKE_MATCH_1}")
    else()
        return()
    endif()

    # The same describe without --abbrev=0 appends the distance from the tag and
    # the abbreviated commit, so a build between releases is identifiable:
    # 0.1.0-12-gdeadbee. On the tagged commit it degrades to the tag name alone,
    # so a release build's string is exactly its triple.
    #
    # When it does not answer, this row does not answer either. Substituting the
    # bare tag would make a build fourteen commits past v0.3.0 claim to BE v0.3.0,
    # under the Exact severity; the commit row below says only what it knows.
    FastCachedRunGit(described outcome
        LOSS "the distance from the tag is unknown, so the git-tag row cannot name this build and the version falls back past it"
        ARGS describe --tags --match "v[0-9]*")
    if(NOT outcome STREQUAL "answered" OR described STREQUAL "")
        return()
    endif()

    # A dirty marker is a configure-time observation, so it is slightly stale by
    # construction. It is still worth having: it reaches only the banner, never a
    # package field, and a binary claiming to be a commit it is not is the more
    # expensive mistake. CI release builds are always clean, so no shipped
    # artifact ever carries it.
    FastCachedGitDirtyMarker(dirtyMarker)

    string(REGEX REPLACE "^v" "" versionString "${described}")

    set(${TripleOutVar} "${triple}" PARENT_SCOPE)
    set(${StringOutVar} "${versionString}${dirtyMarker}" PARENT_SCOPE)
    set(${DetailOutVar} "${nearestTag}" PARENT_SCOPE)
endfunction()

## Resolver: a git work tree in which no tag describes HEAD.
## @param RequestedTripleName Unused by this resolver.
## @param RequestedStringName Unused by this resolver.
## @param TripleOutVar Name of the variable to receive the triple.
## @param StringOutVar Name of the variable to receive the version string.
## @param DetailOutVar Name of the variable to receive the source detail.
# That is the state of a fresh fork, of a shallow CI checkout whose tags were not
# fetched, and of this repository before its first release tag. It repairs what
# the old third branch of this file was reaching for: that branch computed the
# branch name and the short commit and then discarded both without ever setting a
# version, so it could not have worked, and it was unreachable whenever the git
# binary was found at all.
#
# The triple is the declared fallback — nothing here knows a release number — and
# the string carries the commit so the build stays identifiable. Provisional, so
# the caller is warned and told the remedy.
function(FastCachedVersionFromGitCommit RequestedTripleName RequestedStringName
                                        TripleOutVar StringOutVar DetailOutVar)
    set(${TripleOutVar} "" PARENT_SCOPE)
    set(${StringOutVar} "" PARENT_SCOPE)
    set(${DetailOutVar} "" PARENT_SCOPE)

    FastCachedRunGit(insideWorkTree outcome ANSWER_ON_FAILURE
        LOSS "whether this is a git work tree is unknown, so the version falls back past the commit row"
        ARGS rev-parse --is-inside-work-tree)
    if(NOT insideWorkTree STREQUAL "true")
        return()
    endif()

    # rev-parse and not `describe --always`: describe would happily return a tag
    # name here, and this row is only ever reached because no tag was usable — a
    # v1.2 tag that failed the triple pattern would otherwise end up spliced into
    # the string as 0.0.0-0-gv1.2-3-gdeadbee.
    FastCachedRunGit(shortCommit outcome ANSWER_ON_FAILURE
        LOSS "the commit is unknown, so the version falls back past the commit row"
        ARGS rev-parse --short HEAD)
    if(shortCommit STREQUAL "")
        return()  # a work tree whose HEAD has no commit yet
    endif()

    FastCachedGitDirtyMarker(dirtyMarker)

    set(${TripleOutVar} "${FastCachedFallbackVersionTriple}" PARENT_SCOPE)
    set(${StringOutVar}
        "${FastCachedFallbackVersionTriple}-0-g${shortCommit}${dirtyMarker}"
        PARENT_SCOPE)
    set(${DetailOutVar} "commit ${shortCommit}${dirtyMarker}" PARENT_SCOPE)
endfunction()

## Resolver: the last row, which always answers so the loop cannot fall through.
## @param RequestedTripleName Named in the diagnostic as the way to fix this.
## @param RequestedStringName Unused by this resolver.
## @param TripleOutVar Name of the variable to receive the triple.
## @param StringOutVar Name of the variable to receive the version string.
## @param DetailOutVar Name of the variable to receive the source detail.
# The exported-tarball case: no .git, no -D flag, and therefore nothing that
# could possibly know the version. The string says `unknown` in as many words: a
# bug report quoting fastcached-0.0.0-unknown has already answered the first
# question we would otherwise have to ask.
function(FastCachedVersionFallback RequestedTripleName RequestedStringName
                                   TripleOutVar StringOutVar DetailOutVar)
    set(${TripleOutVar} "${FastCachedFallbackVersionTriple}" PARENT_SCOPE)
    set(${StringOutVar} "${FastCachedFallbackVersionTriple}-unknown" PARENT_SCOPE)
    set(${DetailOutVar} "no git tag and no -D${RequestedTripleName}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------

## Resolve the project version from the first source that can answer.
## @param VersionTripleVar Name of the variable to receive the numeric
##        MAJOR.MINOR.PATCH triple. Setting this same name on the cmake command
##        line (-DFASTCACHED_VERSION=1.2.3, given the call in the top-level
##        CMakeLists.txt) overrides every source below it.
## @param VersionStringVar Name of the variable to receive the full, possibly
##        suffixed version string. Overridable the same way, but only together
##        with the triple.
function(GetVersionInformation VersionTripleVar VersionStringVar)
    set(resolvedTriple "")
    set(resolvedString "")
    set(resolvedDetail "")
    set_property(GLOBAL PROPERTY FastCachedVersionUnanswered "")
    set_property(GLOBAL PROPERTY FastCachedVersionUnansweredKinds "")

    foreach(sourceRow IN LISTS FastCachedVersionSources)
        string(REPLACE "|" ";" sourceFields "${sourceRow}")
        list(GET sourceFields 0 sourceLabel)
        list(GET sourceFields 1 sourceResolver)
        list(GET sourceFields 2 sourceAuthority)
        list(GET sourceFields 3 sourceAdvice)

        cmake_language(CALL "${sourceResolver}"
            "${VersionTripleVar}" "${VersionStringVar}"
            resolvedTriple resolvedString resolvedDetail)

        if(NOT resolvedTriple STREQUAL "")
            break()
        endif()
    endforeach()

    # Unreachable while the last row is the unconditional fallback. Kept because
    # the alternative to a diagnostic here is project(VERSION "") and a CPack run
    # that produces fastcached--Linux-x86_64.deb.
    if(resolvedTriple STREQUAL "")
        message(FATAL_ERROR
            "No version source produced a version. The last row of "
            "FastCachedVersionSources is supposed to answer unconditionally, so "
            "a resolver has been added or changed and now returns nothing.")
    endif()

    FastCachedRequireVersionTriple("${resolvedTriple}" "${sourceLabel}")

    set(sourceDetail "")
    if(NOT resolvedDetail STREQUAL "")
        set(sourceDetail " (${resolvedDetail})")
    endif()

    message(STATUS "[Version] triple: ${resolvedTriple}")
    message(STATUS "[Version] string: ${resolvedString}")

    # A query that did not answer makes the result Provisional whichever row won:
    # the row answered from what it could see, and what it could not see is exactly
    # what an Exact version promises. So a publishing job refuses it, and every
    # other build is told which queries went unanswered.
    # The advice is the union of what the recorded KINDS call for, each once: the
    # index re-hash only when something timed out, GIT_EXECUTABLE only when git could
    # not be run.
    get_property(unanswered GLOBAL PROPERTY FastCachedVersionUnanswered)
    get_property(unansweredKinds GLOBAL PROPERTY FastCachedVersionUnansweredKinds)
    if(unanswered)
        list(JOIN unanswered "`, `" unansweredText)
        set(sourceAuthority "Provisional")
        string(APPEND sourceAdvice
            " git gave no answer to `${unansweredText}`, so this is not necessarily the "
            "version the work tree would report.")
        foreach(row IN LISTS FastCachedGitNonAnswers)
            FastCachedGitNonAnswerFields("${row}" row)
            if(rowKind IN_LIST unansweredKinds)
                string(APPEND sourceAdvice " To fix it: ${rowAdvice}.")
            endif()
        endforeach()
    endif()

    # One report, three severities, all of it taken from the winning row: an
    # exact source is a STATUS line, a provisional one warns and prints its
    # remedy, and the jobs that publish artifacts pass
    # -DFASTCACHED_REQUIRE_EXACT_VERSION=ON so a provisional version can never
    # reach a package name.
    if(sourceAuthority STREQUAL "Exact")
        set(severity STATUS)
    elseif(FASTCACHED_REQUIRE_EXACT_VERSION)
        set(severity FATAL_ERROR)
    else()
        set(severity WARNING)
    endif()
    message(${severity}
        "[Version] ${resolvedString} from ${sourceLabel}${sourceDetail}. "
        "${sourceAdvice}")

    # Write resulting version triple and version string to parent scope's variables.
    set(${VersionTripleVar} "${resolvedTriple}" PARENT_SCOPE)
    set(${VersionStringVar} "${resolvedString}" PARENT_SCOPE)
endfunction()
