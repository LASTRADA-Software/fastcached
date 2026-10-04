#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# clang-tidy's HeaderFilterRegex takes every tracked first-party header however the path to it
# is SPELLED -- forward slashes, backslashes, and the mix an MSVC build produces -- at the root
# this checkout actually lives at, and reaches no dependency and no third-party header in any
# spelling.
#
# ## Why
#
# clang-tidy matches the filter against the path it opened a header by, and on Windows that
# path is not the POSIX one: the MSVC compile database carries its include directories with
# backslashes (`-ID:\...\src`), so `#include <tests/ScratchPath.hpp>` opens
# `D:\...\src/tests/ScratchPath.hpp`. `.clang-tidy` spelled every separator `/`, and measured
# with clang-tidy 22.1.8 on Windows, a naming violation planted in a header under `src/tests/`
# was reported 0 times under it and twice under a filter accepting either separator. Every
# header finding on a Windows tidy run was discarded, and the run still looked healthy because
# the main file's findings came through.
#
# The predicate and the table of spellings are `scripts/lib/header-filter.sh`, which
# `local-gate.sh`'s coverage check -- the tidy sweep's precondition -- shares. This is the same
# question asked from `ctest`, which runs on every platform, Windows included, where the gate
# does not. It asks at the REAL root because where a checkout lives is the other dimension
# that has shipped a defect (#1040), and at synthetic roots in `--self-test`, where the answer
# must not depend on the machine.
#
# ## Modes
#
#   bash scripts/check-tidy-header-filter.sh              the configured filter, this checkout
#   bash scripts/check-tidy-header-filter.sh --self-test  the judgement, over planted filters
#
# bash 3.2: no mapfile, no declare -A. Invoke as `bash <path>`, never bare.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
# shellcheck source=lib/header-filter.sh
. scripts/lib/header-filter.sh
# shellcheck source=lib/third-party-roots.sh
. scripts/lib/third-party-roots.sh
# shellcheck source=lib/git-scrub.sh
. scripts/lib/git-scrub.sh

# Judge one filter. Prints one line per failure, or `ok <n>`; returns 1 on any failure.
# @param 1 The include regex. @param 2 The exclude regex, possibly empty. @param 3 The root.
# @param 4 First-party headers, repo-relative. @param 5 Third-party headers, possibly empty.
# @param 6 Dependency paths no spelling may reach -- `header_filter_dependency_paths`' answer.
Judge() {
    local include="$1" exclude="$2" root="$3" firstParty="$4" thirdParty="${5-}" dependencies="${6-}"
    local coverage count reach failed=0
    if [[ -z "$include" ]]; then
        echo "no-regex"
        return 1
    fi
    count="$(printf '%s\n' "$firstParty" | grep -c .)" || count=0
    if [[ "$count" -eq 0 ]]; then
        echo "no-headers"
        return 1
    fi
    coverage="$(header_filter_match "$include" "$exclude" "$root" < <(printf '%s\n' "$firstParty"))"
    if [[ "${coverage%%/*}" != "$count" ]]; then
        echo "uncovered $coverage"
        failed=1
    fi
    if [[ -z "$dependencies" ]]; then
        echo "no-dependencies"
        return 1
    fi
    reach="$(header_filter_reach "$include" "$exclude" "$root" < <(printf '%s\n' "$dependencies"))"
    if [[ "${reach%%/*}" != 0 ]]; then
        echo "dependency $reach"
        failed=1
    fi
    if [[ -n "$thirdParty" ]]; then
        reach="$(header_filter_reach "$include" "$exclude" "$root" < <(printf '%s\n' "$thirdParty"))"
        if [[ "${reach%%/*}" != 0 ]]; then
            echo "third-party $reach"
            failed=1
        fi
    fi
    [[ "$failed" -eq 0 ]] && echo "ok $count"
    return "$failed"
}

if [[ "${1:-}" == "--self-test" ]]; then
    cases=0
    failures=0
    # The wanted verdict is a WHOLE LINE of the output, so a case can assert what a line does
    # NOT carry -- a miss naming no spelling -- and not only what it starts with.
    # The dependency layouts of a PLANTED package, so these cases do not move when this
    # repository adds one; the derivation from a tree is asked on its own, below.
    planteddeps="$(header_filter_dependency_layout catch2)"
    Expect() { # name, wanted status, wanted line, include, root, first-party, third-party
        local name="$1" wantStatus="$2" wantPhrase="$3" out status=0
        cases=$((cases + 1))
        out="$(Judge "$4" "" "$5" "$6" "${7-}" "$planteddeps")" || status=$?
        if [[ "$status" == "$wantStatus" && $'\n'"$out"$'\n' == *$'\n'"$wantPhrase"$'\n'* ]]; then
            echo "   ok   $name"
        else
            echo "   FAIL $name: wanted status $wantStatus and [$wantPhrase], got status $status and [$out]"
            failures=$((failures + 1))
        fi
    }
    planted="src/FastCache/Core/Base64.hpp
src/tests/ScratchPath.hpp
src/apps/fastcache-cc/Cache.hpp
src/CowTree/Tree.hpp"
    vendored="vendor/monocypher/src/monocypher.h"
    lane=/w/fastcached-worktrees/lane-a
    configured="$(header_filter_config .clang-tidy HeaderFilterRegex)"
    Expect "the configured filter takes every spelling and nothing third-party" 0 "ok 4" \
        "$configured" "$lane" "$planted" "$vendored"
    # PLANTED: the filter as it was before this check existed. The miss is named as a
    # SPELLING, because every header is taken as a POSIX path.
    Expect "a /-only filter misses the backslash spelling" 1 \
        "uncovered 0/4 src/FastCache/Core/Base64.hpp (windows spelling)" \
        '.*/src/(CowTree|FastCache|apps|tests)/.*' "$lane" "$planted"
    # PLANTED: both separators as two separate copies, which the remedy warns against --
    # still blind to MSVC's backslashed include directory and slashed `#include` name.
    Expect "two copies of the pattern miss the MSVC mix" 1 \
        "uncovered 0/4 src/FastCache/Core/Base64.hpp (msvc spelling)" \
        '(.*/src/(CowTree|FastCache|apps|tests)/.*|.*\\src\\(CowTree|FastCache|apps|tests)\\.*)' \
        "$lane" "$planted"
    # PLANTED: #1040's pattern, anchored on a directory name. Missed in EVERY spelling, so no
    # spelling is named -- this one is about where the checkout lives.
    Expect "a layout-anchored filter misses a lane worktree, naming no spelling" 1 \
        "uncovered 0/4 src/FastCache/Core/Base64.hpp" \
        '.*/(fastcached[^/]*|worktrees/[^/]+)/src/.*' "$lane" "$planted"
    # PLANTED: the widening a separator fix tempts -- any `src` directory at all.
    Expect "a filter taking any src directory reaches a dependency" 1 \
        "dependency 2/4 out/build/gate-clang-debug/_deps/catch2-src/src/catch2/x.hpp (posix spelling)" \
        '.*[/\\]src[/\\].*' "$lane" "$planted"
    # PLANTED: a leak in ONE spelling, which an every-spelling count would read as untaken.
    Expect "a dependency taken only when backslashed is reached" 1 \
        "dependency 2/4 out/build/gate-clang-debug/_deps/catch2-src/src/catch2/x.hpp (windows spelling)" \
        "${configured}|"'.*\\_deps\\.*' "$lane" "$planted"
    # PLANTED: a filter taking `include/` directories, which reaches a package's headers in its
    # OTHER shape -- the reason the layout asks `include/<name>/` beside `src/<name>/`.
    Expect "a filter taking include directories reaches a dependency" 1 \
        "dependency 2/4 out/build/gate-clang-debug/_deps/catch2-src/include/catch2/x.hpp (posix spelling)" \
        "${configured}|.*[/\\]include[/\\].*" "$lane" "$planted"
    # PLANTED: the widening the partial-match remedy tempts, over a third-party root.
    Expect "a filter reaching a third-party root is caught" 1 \
        "third-party 1/1 vendor/monocypher/src/monocypher.h (posix spelling)" \
        "${configured}|.*/vendor/.*" "$lane" "$planted" "$vendored"
    Expect "no filter at all is refused" 1 "no-regex" "" "$lane" "$planted"
    Expect "no headers is refused, never a pass" 1 "no-headers" "$configured" "$lane" ""
    cases=$((cases + 1))
    if out="$(Judge "$configured" "" "$lane" "$planted" "" "")"; then status=0; else status=$?; fi
    if [[ "$status" == 1 && "$out" == "no-dependencies" ]]; then
        echo "   ok   no dependency paths is refused, never a pass"
    else
        echo "   FAIL no dependency paths is refused, never a pass: got status $status and [$out]"
        failures=$((failures + 1))
    fi

    # The DERIVATION: a planted tree's `CPMAddPackage(NAME ...)` calls, lowercased, one line or
    # several; a commented call and a third-party root's call are not declarations of THIS tree.
    tree="$(mktemp -d)"
    mkdir -p "$tree/cmake" "$tree/scripts/lib" "$tree/vendor/up"
    printf 'vendor/up\n' > "$tree/scripts/lib/third-party-roots.txt"
    printf 'CPMAddPackage(NAME Foo GITHUB_REPOSITORY a/foo VERSION 1.0)\n# CPMAddPackage(NAME Commented)\n' > "$tree/CMakeLists.txt"
    printf 'CPMAddPackage(\n    NAME Bar\n    VERSION 2.0\n)\n' > "$tree/cmake/Deps.cmake"
    printf 'CPMAddPackage(NAME Vendored)\n' > "$tree/vendor/up/CMakeLists.txt"
    cases=$((cases + 1))
    if ( cd "$tree" && scratch_git init -q . && scratch_git add -A ) >/dev/null 2>&1; then
        got="$(header_filter_declared_packages "$tree" 2>/dev/null)" && status=0 || status=$?
        got="$(printf '%s\n' "$got" | tr '\n' ' ')"
        if [[ "$status" == 0 && "$got" == "bar foo " ]]; then
            echo "   ok   declared packages are derived from this tree's own CPMAddPackage calls"
        else
            echo "   FAIL declared packages are derived from this tree's own CPMAddPackage calls: status $status, got [$got]"
            failures=$((failures + 1))
        fi
        # The call shapes a line scanner gets wrong, each planted alone in CMakeLists.txt beside
        # cmake/Deps.cmake's `Bar`. Two LEGITIMATE calls with a `)` before their NAME -- in a trailing
        # comment, in a quoted string -- must be derived, not refused; a LOWERCASE command must be
        # derived, not dropped (CMake command names are case-insensitive).
        # The status is CAPTURED, never left to `set -e`: a refusal here is the defect being
        # asked about, and ending the self-test on it would report no verdict at all.
        DerivedFrom() { # name, CMakeLists.txt content (printf format), wanted packages
            cases=$((cases + 1))
            printf "$2" > "$tree/CMakeLists.txt"
            got="$(header_filter_declared_packages "$tree" 2>&1)" && status=0 || status=$?
            got="$(printf '%s\n' "$got" | tr '\n' ' ')"
            if [[ "$status" == 0 && "$got" == "$3" ]]; then
                echo "   ok   $1"
            else
                echo "   FAIL $1: wanted status 0 and [$3], got status $status and [$got]"
                failures=$((failures + 1))
            fi
        }
        DerivedFrom "a ) inside a trailing comment before NAME does not close the call" \
            'CPMAddPackage(\n    GIT_TAG v1.0 # (pinned)\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a ) inside a quoted string before NAME does not close the call" \
            'CPMAddPackage(\n    OPTIONS "X=$(y)"\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a lowercase cpmaddpackage is a call, derived and never dropped" \
            'cpmaddpackage(NAME Lower VERSION 1.0)\n' "bar lower "
        DerivedFrom "a NAME inside a quoted string before the call's own NAME is not the NAME" \
            'CPMAddPackage(\n    OPTIONS "BUILD NAME Wrong"\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a quoted NAME value is read whole" \
            'CPMAddPackage(NAME "Quoted" VERSION 1.0)\n' "bar quoted "
        DerivedFrom "two calls on one line are both derived" \
            'CPMAddPackage(NAME One VERSION 1.0) CPMAddPackage(NAME Two VERSION 2.0)\n' "bar one two "
        DerivedFrom "an unquoted X(y) argument before NAME does not close the call" \
            'CPMAddPackage(\n    OPTIONS X(y)\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a NAME whose value is on the next line is derived" \
            'CPMAddPackage(\n    NAME\n        Foo\n)\n' "bar foo "
        DerivedFrom "a call inside a bracket comment is not counted" \
            '#[[\nCPMAddPackage(NAME Commented)\n]]\n' "bar "
        DerivedFrom "a call after a bracket comment closes, on its closing line, IS counted" \
            '#[[ retired\n]] CPMAddPackage(NAME After VERSION 1.0)\n' "after bar "
        DerivedFrom "a bracket comment closes only on as many = as it opened with" \
            '#[=[\n]]\nCPMAddPackage(NAME Inner)\n]=]\n' "bar "
        DerivedFrom "a NAME on the second line of a quoted argument is not the NAME" \
            'CPMAddPackage(\n    OPTIONS "A\n    NAME Wrong"\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a NAME inside a bracket argument is not the NAME" \
            'CPMAddPackage(\n    OPTIONS [==[ NAME Wrong ]==]\n    NAME Foo\n)\n' "bar foo "
        # The keyword at the END of a line inside an argument, its value on the next, BEFORE the
        # call's real NAME. Since the LAST NAME decides, the real NAME after them overwrites a
        # decoy read as a keyword, so these two no longer guard the keyword fix: they guard
        # ORDER -- a reader that let the FIRST NAME decide would derive `wrong` here. The keyword
        # fix is guarded by the two decoy-AFTER rows below.
        DerivedFrom "a quoted argument ending in NAME is not a NAME whose value is the next line" \
            'CPMAddPackage(\n    OPTIONS "A NAME\nWrong"\n    NAME Foo\n)\n' "bar foo "
        DerivedFrom "a bracket argument ending in NAME is not a NAME whose value is the next line" \
            'CPMAddPackage(\n    OPTIONS [[ NAME\nWrong ]]\n    NAME Foo\n)\n' "bar foo "
        # The same two AFTER the real NAME -- the rows that guard the KEYWORD fix: since the last
        # NAME decides, a decoy read as a keyword out of an argument would win here, and only
        # asking the keyword of `bare` refuses it.
        DerivedFrom "a quoted argument ending in NAME, after the NAME, is not the last NAME" \
            'CPMAddPackage(\n    NAME Foo\n    OPTIONS "A NAME\nWrong"\n)\n' "bar foo "
        DerivedFrom "a bracket argument ending in NAME, after the NAME, is not the last NAME" \
            'CPMAddPackage(\n    NAME Foo\n    OPTIONS [[ NAME\nWrong ]]\n)\n' "bar foo "
        DerivedFrom "the LAST NAME decides, as cmake_parse_arguments reads it" \
            'CPMAddPackage(NAME First VERSION 1.0 NAME Second)\n' "bar second "
        DerivedFrom "a value spelled NAME is a value, not the next keyword" \
            'CPMAddPackage(NAME NAME VERSION 1.0)\n' "bar name "
        # A tracked file NAMED like an assignment: awk reads a bare `x=y.cmake` operand as
        # `x = "y.cmake"` and never opens it, so its package vanished in silence. Planted beside
        # CMakeLists.txt, tracked, and asserted DERIVED.
        cases=$((cases + 1))
        printf 'CPMAddPackage(NAME Assigned VERSION 1.0)\n' > "$tree/x=y.cmake"
        printf 'CPMAddPackage(NAME Foo VERSION 1.0)\n' > "$tree/CMakeLists.txt"
        ( cd "$tree" && scratch_git add -- 'x=y.cmake' ) >/dev/null 2>&1
        got="$(header_filter_declared_packages "$tree" 2>&1)" && status=0 || status=$?
        got="$(printf '%s\n' "$got" | tr '\n' ' ')"
        ( cd "$tree" && scratch_git rm -q --cached -- 'x=y.cmake' ) >/dev/null 2>&1
        rm -f "$tree/x=y.cmake"
        if [[ "$status" == 0 && "$got" == "assigned bar foo " ]]; then
            echo "   ok   a tracked file named like an awk assignment (x=y.cmake) is read, never skipped"
        else
            echo "   FAIL a tracked file named like an awk assignment (x=y.cmake) is read, never skipped: wanted status 0 and [assigned bar foo ], got status $status and [$got]"
            failures=$((failures + 1))
        fi
        DerivedFrom "an escaped quote outside quotes opens no quoted argument" \
            'CPMAddPackage(\n    OPTIONS -DX=\\"y\\"\n    NAME Foo\n)\n' "bar foo "
        # A NAME spelled through a variable is REFUSED, saying the name cannot be derived statically
        # -- never as the shorthand, whose remedy (add a NAME) the call already follows. The third
        # shape is the one that fails OPEN: its literal prefix alone reads as a real, WRONG name.
        RefusedAsDynamic() { # name, CMakeLists.txt content (printf format), wanted file:line
            cases=$((cases + 1))
            printf "$2" > "$tree/CMakeLists.txt"
            got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
            if [[ "$status" == 2 && "$got" == *"cannot be derived statically: $3 "* && "$got" != *"shorthand"* ]]; then
                echo "   ok   $1"
            else
                echo "   FAIL $1: wanted status 2 naming [$3] as not derivable statically and no shorthand remedy, got status $status and [$got]"
                failures=$((failures + 1))
            fi
        }
        RefusedAsDynamic "a NAME from a foreach variable is refused as not derivable statically" \
            'foreach(dep a b)\n    CPMAddPackage(NAME ${dep})\nendforeach()\n' "CMakeLists.txt:2"
        RefusedAsDynamic "a NAME from the environment is refused as not derivable statically" \
            'CPMAddPackage(\n    NAME "$ENV{DEP}"\n)\n' "CMakeLists.txt:1"
        RefusedAsDynamic "a NAME with a literal prefix and a variable is refused, never read as the prefix" \
            'CPMAddPackage(NAME foo_${suffix} VERSION 1.0)\n' "CMakeLists.txt:1"
        # A NAME that runs on past the name is REFUSED naming the token -- its prefix alone
        # derived `foo`, a real-looking wrong package -- and the terminators CMake itself
        # ends an argument on are ACCEPTED, so the refusal is not simply "anything else".
        RefusedAsMalformed() { # name, CMakeLists.txt content (printf format), wanted "file:line (token)"
            cases=$((cases + 1))
            printf "$2" > "$tree/CMakeLists.txt"
            got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
            if [[ "$status" == 2 && "$got" == *"not a plain package name"*"$3"* ]]; then
                echo "   ok   $1"
            else
                echo "   FAIL $1: wanted status 2 naming [$3] as not a plain package name, got status $status and [$got]"
                failures=$((failures + 1))
            fi
        }
        RefusedAsMalformed "a NAME running on into :: is refused, never read as its prefix" \
            'CPMAddPackage(NAME foo::bar VERSION 1.0)\n' "CMakeLists.txt:1 (foo::bar)"
        RefusedAsMalformed "a NAME running on into / is refused" \
            'CPMAddPackage(NAME foo/bar VERSION 1.0)\n' "CMakeLists.txt:1 (foo/bar)"
        RefusedAsMalformed "a NAME running on into @ is refused" \
            'CPMAddPackage(NAME foo@1 VERSION 1.0)\n' "CMakeLists.txt:1 (foo@1)"
        RefusedAsMalformed "a NAME running on into an escaped ; is refused" \
            'CPMAddPackage(NAME foo\\;bar VERSION 1.0)\n' 'CMakeLists.txt:1 (foo\;bar)'
        # The selection's own grep failing is the CHECK failing, and is SAID: a grep killed by a
        # signal prints nothing, and this refusal alone used to return 2 in silence -- round 8's
        # "status 2 and []", which nobody could attribute. A grep that fails is shadowed here.
        cases=$((cases + 1))
        printf 'CPMAddPackage(NAME Foo VERSION 1.0)\n' > "$tree/CMakeLists.txt"
        got="$(grep() { return 2; }; header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"grep exited 2"*"the CHECK failing"* ]]; then
            echo "   ok   a selection grep that fails is refused naming grep, never in silence"
        else
            echo "   FAIL a selection grep that fails is refused naming grep, never in silence: status $status, [$got]"
            failures=$((failures + 1))
        fi
        # Its sibling, which failed OPEN rather than silent: a sed that fails while escaping a
        # root left that root EMPTY, the pattern `^()(/|$)` selected nothing, and every
        # vendored file read as this project's own with status 0. The tree is the same one, so
        # the only thing that can turn this red is the escape's own check.
        cases=$((cases + 1))
        got="$(sed() { return 2; }; header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"sed exited 2 escaping the root"*"the CHECK failing"* ]]; then
            echo "   ok   a root escape that fails is refused naming sed, never read as no root"
        else
            echo "   FAIL a root escape that fails is refused naming sed, never read as no root: status $status, [$got]"
            failures=$((failures + 1))
        fi
        # And the rest of the shape, audited rather than left to the next red: the filter's own
        # grep behind `|| true` read as "takes nothing", and the names' sort as "declares none".
        cases=$((cases + 1))
        got="$(grep() { return 2; }; header_filter_taken_lines '.*' '' "$tree" posix 'src/a.h' 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"grep exited 2 matching the include filter"* ]]; then
            echo "   ok   a filter grep that fails is refused naming grep, never read as taking nothing"
        else
            echo "   FAIL a filter grep that fails is refused naming grep, never read as taking nothing: status $status, [$got]"
            failures=$((failures + 1))
        fi
        cases=$((cases + 1))
        got="$(sort() { return 2; }; header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"sort exited 2 over the package names"* ]]; then
            echo "   ok   a names sort that fails is refused naming sort, never read as declaring none"
        else
            echo "   FAIL a names sort that fails is refused naming sort, never read as declaring none: status $status, [$got]"
            failures=$((failures + 1))
        fi
        cases=$((cases + 1))
        printf 'CPMAddPackage(NAME [[Foo]] VERSION 1.0)\n' > "$tree/CMakeLists.txt"
        got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"given as a bracket argument"*"CMakeLists.txt:1 "* && "$got" != *"no NAME"* && "$got" != *"not a plain package name"* ]]; then
            echo "   ok   a NAME given as a bracket argument is refused saying so, not as no NAME"
        else
            echo "   FAIL a NAME given as a bracket argument is refused saying so, not as no NAME: status $status, [$got]"
            failures=$((failures + 1))
        fi
        DerivedFrom "a NAME ending at the call's ) is accepted" \
            'CPMAddPackage(GIT_TAG v1 NAME Tight)\n' "bar tight "
        DerivedFrom "a NAME ending at an unquoted ; -- a list separator -- is accepted" \
            'CPMAddPackage(NAME Semi;x VERSION 1.0)\n' "bar semi "
        # The command TABLE: every CPM command that fetches a package is read, in any case, by
        # the form its row names -- and the one that only declares adds nothing on its own.
        DerivedFrom "a CPMFindPackage is read by its NAME" \
            'CPMFindPackage(NAME Found VERSION 1.0)\n' "bar found "
        DerivedFrom "a CPMGetPackage is read by its first argument" \
            'CPMGetPackage(Got)\n' "bar got "
        DerivedFrom "the table is matched case-insensitively" \
            'CpmFindPackage(NAME Mixed)\ncpmgetpackage(Lower2)\n' "bar lower2 mixed "
        DerivedFrom "a CPMDeclarePackage alone fetches nothing, and nothing inside it is a NAME" \
            'CPMDeclarePackage(Decl NAME Wrong VERSION 1.0)\n' "bar "
        RefusedAsDynamic "a CPMGetPackage of a variable is refused as not derivable statically" \
            'CPMGetPackage(${dep})\n' "CMakeLists.txt:1"
        cases=$((cases + 1))
        printf 'CPMGetPackage()\n' > "$tree/CMakeLists.txt"
        got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"no NAME"*"CMakeLists.txt:1 "* ]]; then
            echo "   ok   a CPMGetPackage with no argument is refused by file and line"
        else
            echo "   FAIL a CPMGetPackage with no argument is refused by file and line: status $status, [$got]"
            failures=$((failures + 1))
        fi
        # Both kinds in one tree are both named, so fixing one does not uncover the other a run later.
        cases=$((cases + 1))
        printf 'CPMAddPackage("gh:someone/src@1.0")\nCPMAddPackage(NAME ${dep})\n' > "$tree/CMakeLists.txt"
        got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"no NAME"*"CMakeLists.txt:1 "* && "$got" == *"cannot be derived statically: CMakeLists.txt:2 "* ]]; then
            echo "   ok   a tree with a nameless call and a variable NAME names both"
        else
            echo "   FAIL a tree with a nameless call and a variable NAME names both: status $status, [$got]"
            failures=$((failures + 1))
        fi
        printf 'CPMAddPackage(NAME Foo GITHUB_REPOSITORY a/foo VERSION 1.0)\n# CPMAddPackage(NAME Commented)\n' > "$tree/CMakeLists.txt"
        # PLANTED: a tracked file the reader cannot open. awk reads the others and exits non-zero,
        # so the answer `foo` is PARTIAL -- `bar` is missing -- and must be refused as the reader
        # failing, never returned as the set nor named as a tree declaring nothing.
        cases=$((cases + 1))
        mv "$tree/cmake/Deps.cmake" "$tree/cmake/Deps.cmake.kept"
        got="$(header_filter_declared_packages "$tree" 2>&1)" && status=0 || status=$?
        mv "$tree/cmake/Deps.cmake.kept" "$tree/cmake/Deps.cmake"
        # awk's OWN status, which is 2 for a file it cannot open in gawk, mawk and BWK awk
        # alike -- never the 123 xargs substituted for every failure of what it ran.
        if [[ "$status" == 2 && "$got" == *"reader failed"*"(awk exit 2;"* && "$got" != *"no CPM call naming a package"* ]]; then
            echo "   ok   a tracked CMake file the reader cannot open is refused as the reader failing, never a partial set"
        else
            echo "   FAIL a tracked CMake file the reader cannot open is refused as the reader failing, never a partial set: status $status, [$got]"
            failures=$((failures + 1))
        fi
        # PLANTED: the reviewer's shorthand call, which has no NAME. Dropped, it would still
        # report `bar foo` -- two packages for a tree that declares three -- so it is refused,
        # naming the file and line of the call.
        cases=$((cases + 1))
        printf 'CPMAddPackage("gh:someone/src@1.0")\n' >> "$tree/CMakeLists.txt"
        got="$(header_filter_declared_packages "$tree" 2>&1 >/dev/null)" && status=0 || status=$?
        if [[ "$status" == 2 && "$got" == *"no NAME"*"CMakeLists.txt:3"* ]]; then
            echo "   ok   a CPMAddPackage call with no NAME is refused by file and line, never dropped"
        else
            echo "   FAIL a CPMAddPackage call with no NAME is refused by file and line, never dropped: status $status, [$got]"
            failures=$((failures + 1))
        fi
        cases=$((cases + 1))
        printf '# no packages\n' > "$tree/CMakeLists.txt"
        printf '# none here either\n' > "$tree/cmake/Deps.cmake"
        if header_filter_declared_packages "$tree" >/dev/null 2>&1; then status=0; else status=$?; fi
        if [[ "$status" == 2 ]]; then
            echo "   ok   a tree declaring no package is refused, never read as having none"
        else
            echo "   FAIL a tree declaring no package is refused, never read as having none: status $status"
            failures=$((failures + 1))
        fi
    else
        echo "   FAIL the derivation cases need git init + git add, which failed -- not run, so not passed"
        failures=$((failures + 1))
    fi
    rm -f "$tree/CMakeLists.txt" "$tree/cmake/Deps.cmake" "$tree/vendor/up/CMakeLists.txt" "$tree/scripts/lib/third-party-roots.txt"
    echo "check-tidy-header-filter self-test: ${cases} case(s), ${failures} failure(s)"
    [[ "$cases" -gt 0 && "$failures" -eq 0 ]]
    exit $?
fi

include="$(header_filter_config .clang-tidy HeaderFilterRegex)"
exclude="$(header_filter_config .clang-tidy ExcludeHeaderFilterRegex)"
tracked="$(git ls-files -- '*.hpp' '*.h')"
firstParty="$(first_party_paths "$(pwd)" "$tracked")"
thirdParty="$(third_party_paths "$(pwd)" "$tracked")"
[[ -z "$thirdParty" ]] || echo "check-tidy-header-filter: $(third_party_declined_summary 'header(s)' "$thirdParty"), and asks that none is reached"
root="$(pwd)"
if ! dependencies="$(header_filter_dependency_paths "$root")"; then
    echo "check-tidy-header-filter: which dependency layouts the filter must keep out cannot be derived (above); refused rather than judged against none" >&2
    exit 1
fi
status=0
verdict="$(Judge "$include" "$exclude" "$root" "$firstParty" "$thirdParty" "$dependencies")" || status=$?
if [[ "$status" -ne 0 ]]; then
    echo "check-tidy-header-filter: the HeaderFilterRegex in .clang-tidy ('$include'), asked at $root, fails:" >&2
    printf '  %s\n' "$verdict" >&2
    echo "  A header the filter does not take in some spelling is a header whose findings clang-tidy discards" >&2
    echo "  on that platform, silently. Accept either separator ([/\\\\]) rather than adding a Windows copy of" >&2
    echo "  the pattern, and keep naming this repository's own roots: a pattern taking any src directory takes" >&2
    echo "  the dependencies with it. A miss naming no spelling is about where this checkout lives (#1040)." >&2
    exit 1
fi
echo "check-tidy-header-filter: '$include' takes all ${verdict#ok } tracked first-party header(s) at $root in each of: ${HeaderFilterSpellings}; and reaches none of $(printf '%s\n' "$dependencies" | grep -c .) dependency path(s) derived from $(header_filter_declared_packages "$root" | grep -c .) declared package(s), nor a third-party header, in any"
