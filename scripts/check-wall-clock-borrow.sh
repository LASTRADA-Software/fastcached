#!/usr/bin/env bash
#
# A retained wall clock is a `WallClockRef`, never a raw `IWallClock` pointer or
# reference member.
#
# `WallClockRef` (Core/Clock.hpp) refuses to be built from an rvalue, so a holder that
# takes one cannot be handed a temporary -- at the parameter, which is what makes the
# refusal survive a FORWARDING constructor. A deleted rvalue overload on the storing
# type does not: inside a forwarder the parameter is a named lvalue, so it binds to the
# ordinary overload. That is measured, on gcc and clang, and it is the exact shape of
# #1028, where the temporary went to `FleetSampler` -- which stores no clock at all.
#
# The TYPE is the guard. This scan covers the one thing a type cannot: a new class
# declaring a raw `IWallClock const* _wall` and never reaching for `WallClockRef` at
# all. An opt-in set of guarded constructors reads identically to complete coverage
# (#492), so the rule is deny-by-default with one stated exception.
#
# bash 3.2: no mapfile, no declare -A, no ${var^^} -- a hygiene script runs on macOS.
# Invoke as `bash scripts/check-wall-clock-borrow.sh`, never bare: 15 of the scripts
# here are mode 644 in git and a bare call exits 126, which inside a want-fail
# assertion is indistinguishable from the rule firing (#723).
set -euo pipefail


# ---------------------------------------------------------------------------
# Self-test. A scan nobody has watched refuse is not a guard.
#
# Every case runs in BOTH file-set modes. A synthetic tree is not a git repository, so
# a self-test that only ever built one would exercise the WALK path while CI exercises
# GIT -- the mode under test not being the mode in use, which is a guard passing
# because it is testing something else. The mode is printed and asserted per case.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    ran=0
    failed=0

    # @param 1 case name, 2 tree dir, 3 expected exit (0 or 1), 4 expected substring,
    #        5 mode ("walk" or "git")
    _case()
    {
        ran=$((ran + 1))
        if [ "$5" = "git" ]; then
            ( cd "$2" && git init -q . && git add -A && git -c user.email=t@t -c user.name=t commit -qm t ) > /dev/null 2>&1
        fi
        out="$(bash "$self" "$2" 2>&1)" && rc=0 || rc=$?
        if [ "$rc" != "$3" ]; then
            echo "FAIL selftest/$1 [$5]: exit $rc, expected $3" >&2
            echo "$out" | sed "s|^|     | " >&2
            failed=$((failed + 1))
            return
        fi
        case "$out" in
            *"$4"*) ;;
            *)
                echo "FAIL selftest/$1 [$5]: output does not carry '$4'" >&2
                echo "$out" | sed "s|^|     | " >&2
                failed=$((failed + 1))
                return
                ;;
        esac
        case "$out" in
            *"mode ${5}"*) ;;
            *)
                echo "FAIL selftest/$1 [$5]: ran in the wrong file-set mode" >&2
                failed=$((failed + 1))
                ;;
        esac
    }

    # @param 1 tree dir, 2 extra content for a second header (may be empty)
    _tree()
    {
        mkdir -p "$1/src/FastCache/Core" "$1/src/FastCache/Distributed"
        cat > "$1/src/FastCache/Core/Clock.hpp" <<"HDR"
class WallClockRef
{
    IWallClock const* _wall;
};
HDR
        cat > "$1/src/FastCache/Distributed/Holder.hpp" <<"HDR"
class Holder
{
    WallClockRef _wall;
};
HDR
        if [ -n "$2" ]; then
            printf "%s" "$2" > "$1/src/FastCache/Distributed/Extra.hpp"
        fi
    }

    for mode in walk git; do
        # The control: a correct tree must PASS. A check that refuses everything refuses
        # a correct tree too, and only a case that must pass can see that.
        c="$work/clean-$mode"; mkdir -p "$c"; _tree "$c" ""
        _case "control-a-guarded-tree-passes" "$c" 0 "all 'WallClockRef'" "$mode"

        # The violation, planted: a raw retained clock outside the exempt file.
        c="$work/raw-$mode"; mkdir -p "$c"
        _tree "$c" "class Sneak
{
    IWallClock const* _wall;
};
"
        _case "a-raw-retained-clock-is-refused" "$c" 1 "Extra.hpp" "$mode"

        # The false-positive direction, and the reason the scan strips comments: the
        # sentence explaining this rule contains the pattern it bans, so a scan that
        # read prose would refuse the file that documents it.
        c="$work/comment-$mode"; mkdir -p "$c"
        _tree "$c" "/*
    IWallClock const* _wall;   // what this rule bans, quoted so a reader can see it
*/
class Fine
{
    WallClockRef _wall;
};
"
        _case "a-declaration-quoted-inside-a-block-comment-is-not-one" "$c" 0 "all 'WallClockRef'" "$mode"

        # Two empty lists agree perfectly. With the guard type gone the scan matches
        # nothing, and a clean verdict would then describe a tree it never read.
        c="$work/empty-$mode"; mkdir -p "$c"
        mkdir -p "$c/src/FastCache/Core"
        printf "class Nothing {};
" > "$c/src/FastCache/Core/Clock.hpp"
        _case "no-guarded-member-anywhere-is-refused" "$c" 1 "stopped matching" "$mode"
    done

    echo "wall-clock-borrow-selftest: ${ran} cases ran, ${failed} failed"
    [ "$failed" -eq 0 ] || exit 1
    exit 0
fi

root="${1:-.}"
cd "$root"

refuse()
{
    echo "FAIL wall-clock-borrow: $*" >&2
    # A plain exit, deliberately: this script calls `fail` from no subshell, so the
    # `kill -s TERM $$` idiom the e2e fixtures need would only buy an exit status of
    # 143 -- and anything that is neither 0 nor 1 reads as "something killed this"
    # rather than "the tree is bad", which is the opposite of what a verdict should say.
    exit 1
}

# The one file allowed to hold the raw pointer: `WallClockRef` itself is what stores it.
exempt="src/FastCache/Core/Clock.hpp"

# File set, with the MODE stated in the output and asserted on both sides -- a
# synthetic tree is not a git repository, so a check that silently falls back tests
# the cheap-to-construct path and never the one CI runs (#685 family).
if git ls-files "src/*.hpp" "src/*.cpp" > /dev/null 2>&1 && [ -n "$(git ls-files "src/*.hpp" "src/*.cpp")" ]; then
    mode="git"
    files="$(git ls-files "src/*.hpp" "src/*.cpp")"
else
    mode="walk"
    files="$(find src -type f -name "*.hpp" -o -type f -name "*.cpp" | sort)"
fi
[ -n "$files" ] || refuse "no C++ sources found under src/ (mode ${mode}); the file set is empty, which agrees with everything"

echo "wall-clock-borrow: reading $(echo "$files" | wc -l | tr -d " ") source(s), mode ${mode}"

# A COMMENT is not a declaration. Two checks in one branch matched their own headers
# for want of this, and the sentence explaining this rule contains the pattern it bans.
#
# BLOCK comments are why this is not a one-line `sed`. The match below is anchored at
# the start of a line, so a `//` comment could never have matched it and stripping one
# bought nothing -- but a declaration commented out inside a `/* ... */` sits at the
# start of its own line and matches perfectly. The first version of this script stripped
# only `//`, and its self-test case passed with the stripping REMOVED: a fixture that
# could not fail, found by neutering rather than by reading.
strip_comments()
{
    awk '
    {
        line = $0
        out = ""
        while (length(line) > 0) {
            if (inBlock) {
                p = index(line, "*/")
                if (p == 0) { line = ""; break }
                line = substr(line, p + 2)
                inBlock = 0
                continue
            }
            b = index(line, "/*")
            l = index(line, "//")
            if (l > 0 && (b == 0 || l < b)) { out = out substr(line, 1, l - 1); line = ""; break }
            if (b > 0) { out = out substr(line, 1, b - 1); line = substr(line, b + 2); inBlock = 1; continue }
            out = out line
            line = ""
        }
        print out
    }' "$1"
}

raw_members=""
guarded=0
for f in $files; do
    text="$(strip_comments "$f")"
    hits="$(echo "$text" | grep -nE "^[[:space:]]*IWallClock[[:space:]]*(const[[:space:]]*)?[*&][[:space:]]*_[A-Za-z]" || true)"
    if [ -n "$hits" ] && [ "$f" != "$exempt" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && raw_members="${raw_members}${f}:${line}
"
        done <<< "$hits"
    fi
    n="$(echo "$text" | grep -cE "^[[:space:]]*WallClockRef[[:space:]]+_[A-Za-z]" || true)"
    guarded=$((guarded + n))
done

# Two empty lists agree perfectly: a scan finding no guarded member at all has stopped
# matching, and would then pass over a tree with the guard entirely removed.
[ "$guarded" -gt 0 ] || refuse "found no 'WallClockRef' member anywhere; the pattern has stopped matching, so a clean result would mean nothing"

if [ -n "$raw_members" ]; then
    echo "$raw_members" | sed "/^$/d" | sed "s|^|  |" >&2
    refuse "a retained wall clock must be a 'WallClockRef', not a raw IWallClock pointer or reference. It is the PARAMETER that has to refuse a temporary: a deleted rvalue overload on the storing type accepts one that arrived through a forwarding constructor, which is #1028."
fi

echo "wall-clock-borrow: ${guarded} retained wall clock(s), all 'WallClockRef'; no raw retained IWallClock outside ${exempt}"
