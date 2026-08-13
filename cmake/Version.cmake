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
##        string when git is absent or the command failed.
## @param ARGN The git arguments.
# Written once because every call below needs exactly this. The previous version
# of this file grew four copy-pasted execute_process blocks, two of which ran in
# CMAKE_CURRENT_SOURCE_DIR while the others used CMAKE_SOURCE_DIR.
function(FastCachedRunGit OutputVar)
    if(NOT GIT_EXECUTABLE)
        set(${OutputVar} "" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE commandOutput
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE commandResult
    )
    if(NOT commandResult STREQUAL "0")
        set(commandOutput "")
    endif()
    set(${OutputVar} "${commandOutput}" PARENT_SCOPE)
endfunction()

## Suffix marking a work tree with uncommitted changes to tracked files.
## @param OutputVar Name of the variable to receive "-dirty" or "".
# Untracked files deliberately do not count: out/, .cache/ and a local
# version.txt are all legitimate and none of them changes what the source says.
# Written once because both git resolvers append the same marker.
function(FastCachedGitDirtyMarker OutputVar)
    FastCachedRunGit(pendingChanges status --porcelain --untracked-files=no)
    if(pendingChanges STREQUAL "")
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
    FastCachedRunGit(nearestTag describe --tags --abbrev=0 --match "v[0-9]*")
    if(nearestTag MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(triple "${CMAKE_MATCH_1}")
    else()
        return()
    endif()

    # The same describe without --abbrev=0 appends the distance from the tag and
    # the abbreviated commit, so a build between releases is identifiable:
    # 0.1.0-12-gdeadbee. On the tagged commit it degrades to the tag name alone,
    # so a release build's string is exactly its triple.
    FastCachedRunGit(described describe --tags --match "v[0-9]*")
    if(described STREQUAL "")
        set(described "${nearestTag}")
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

    FastCachedRunGit(insideWorkTree rev-parse --is-inside-work-tree)
    if(NOT insideWorkTree STREQUAL "true")
        return()
    endif()

    # rev-parse and not `describe --always`: describe would happily return a tag
    # name here, and this row is only ever reached because no tag was usable — a
    # v1.2 tag that failed the triple pattern would otherwise end up spliced into
    # the string as 0.0.0-0-gv1.2-3-gdeadbee.
    FastCachedRunGit(shortCommit rev-parse --short HEAD)
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
