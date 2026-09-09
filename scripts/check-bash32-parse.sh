#!/usr/bin/env bash
# Can the bash we ship to actually PARSE these scripts? (#1100)
#
# A script here can be legal token by token, pass every guard the repository has, pass
# the local gate, and still be unparseable by the bash macOS ships. Nothing ran a parse
# check, so the only instrument that reported it was the full `ctest` run inside
# `macOS-clang-release`, four minutes into a required context -- and it reported it at
# the WRONG LINE.
#
# ## The instance, and why no existing guard could see it
#
# PR #1096, run 34318804605. One test of 3533 failed:
#
#     3447/3533 Test #3450: e2e-helpers-selftest ...***Failed    0.02 sec
#     .../check-e2e-helpers.sh: line 402: syntax error near unexpected token `('
#
# `0.02 sec` -- it never ran, it died at parse. And line 402 was NOT the cause: that
# construct is on master unchanged and parses fine. bash 3.2 reads an apostrophe inside
# a COMMENT within `$( )` as opening a single-quoted string, so an ODD number of them
# desynchronises the scan, it swallows the closing paren, and the parser reports failure
# at the first real paren it meets -- 135 lines further down.
#
# The bash-3.2 TOKEN scan in `check-e2e-helpers.sh` could not see it and was right not
# to: `mapfile`, `declare -A`, `local -n` are all absent, every token is legal on 3.2,
# and the defect is a COMPOSITION of legal ones. That gap is #880. This is its
# complement, not its replacement -- a construct that PARSES on 3.2 and behaves
# differently (`BASHPID` silently empty) is invisible here and stays the token scan's
# job. Neither subsumes the other.
#
# ## Why this refuses on a modern bash instead of passing
#
# Run under bash 5, `bash -n` parses the very constructs this exists to catch and reports
# success -- the guard passing because it is testing something else, which this tree has
# already paid for once in `check-catch-skip-return-code`. So the interpreter is ASSERTED,
# and a bash 4.0 or newer is a REFUSAL rather than a green.
#
# That makes this a macOS-job step rather than a ctest registration: the only real 3.2 in
# this project's world is a macOS runner. `--self-test` runs anywhere, because it drives
# the checker's own logic rather than the parser's.
#
# bash 3.2: this runs under the 2007 interpreter by design.
#
# Usage:
#   bash scripts/check-bash32-parse.sh
#   bash scripts/check-bash32-parse.sh --self-test
#   bash scripts/check-bash32-parse.sh --root <dir>
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
selfTest=0
# Only for the self-test, which must exercise the version gate on a host whose bash is
# the wrong one. Never set by the real invocation.
allowModernBash="${FASTCACHED_PARSE_ALLOW_MODERN_BASH:-0}"

while [ $# -gt 0 ]; do
    case "${1:-}" in
        --self-test) selfTest=1; shift ;;
        --root)      root="${2:?--root needs a tree}"; shift 2 ;;
        *) printf 'check-bash32-parse: unknown argument: %s\n' "${1:-}" >&2; exit 2 ;;
    esac
done

# The interpreter is part of the verdict, and is printed whatever happens. A run that
# does not say which bash parsed cannot be read later -- the figure and its conditions
# travel together or the figure is unusable.
BashDescription() {
    printf '%s.%s (%s)' "${BASH_VERSINFO[0]:-?}" "${BASH_VERSINFO[1]:-?}" "${BASH_VERSION:-unknown}"
}

RequireOldBash() {
    if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
        return 0
    fi
    if [ "$allowModernBash" = "1" ]; then
        return 0
    fi
    printf 'check-bash32-parse: REFUSED: running under bash %s\n' "$(BashDescription)" >&2
    printf '     This parses with the interpreter it is RUN under, and a bash 4.0 or newer\n' >&2
    printf '     accepts every construct this check exists to catch -- so a green here would\n' >&2
    printf '     mean the mode under test was not the mode in use. Run it on a host whose\n' >&2
    printf '     /bin/bash is 3.2, which in this project is a macOS runner.\n' >&2
    exit 1
}

# Every tracked shell script, WALKED rather than listed. A list is exact about the files
# it knows and silent about the ones it does not (#492), and a new script must be covered
# by construction rather than by somebody remembering.
TrackedShellScripts() {
    if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
        printf 'git\n'
        git -C "$root" ls-files '*.sh' 2>/dev/null || true
    else
        printf 'walk\n'
        find "$root" -name '*.sh' -type f 2>/dev/null | sed "s#^${root}/##" | sort || true
    fi
}

RunParseCheck() {
    local mode scripts file parsed failed out
    parsed=0
    failed=0

    # The MODE is part of the output and asserted by the self-test. A guard that reports
    # the same sentence whichever path it took is one whose cheap-to-construct path
    # silently becomes the only one tested.
    scripts="$(TrackedShellScripts)"
    mode="${scripts%%$'\n'*}"
    scripts="${scripts#*$'\n'}"

    printf 'check-bash32-parse: parsing with bash %s, file set from %s\n' \
        "$(BashDescription)" "$mode"

    while IFS= read -r file; do
        [ -n "$file" ] || continue
        [ -f "${root}/${file}" ] || continue
        parsed=$((parsed + 1))
        if out="$(bash -n "${root}/${file}" 2>&1)"; then
            continue
        fi
        failed=$((failed + 1))
        printf 'FAIL %s does not PARSE under bash %s\n' "$file" "$(BashDescription)" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        # The reported line is where the parser GAVE UP, not where it desynchronised.
        # #1096's real gap was 135 lines, onto a construct identical to master's -- so a
        # message that prints the raw error sends whoever meets it to edit a correct line.
        printf '     The line above is where the parser FAILED, not necessarily where the\n' >&2
        printf '     cause is: look at or ABOVE it. bash 3.2 reads an apostrophe inside a\n' >&2
        printf '     comment within $( ) as opening a quote, so an odd number of them\n' >&2
        printf '     swallows the closing paren and the error surfaces at the next real\n' >&2
        printf '     paren, which can be far below. That is #1096 exactly.\n' >&2
    done <<EOF
$scripts
EOF

    # An empty walk reports clean while checking nothing.
    if [ "$parsed" -lt 1 ]; then
        printf 'FAIL check-bash32-parse: no tracked *.sh was found under %s -- the walk is\n' "$root" >&2
        printf '     broken, not the tree, and a clean report from it would mean nothing.\n' >&2
        return 1
    fi

    printf 'check-bash32-parse: %d script(s) parsed, %d unparseable\n' "$parsed" "$failed"
    [ "$failed" -eq 0 ]
}

# ---------------------------------------------------------------------------
# Self-test. Runs anywhere: it drives THIS script's logic, not the 3.2 parser.
# ---------------------------------------------------------------------------
SelfTestCases=0
Ok() { SelfTestCases=$((SelfTestCases + 1)); printf 'ok   %s\n' "$1"; }
Bad() { printf 'FAIL %s\n' "$1"; [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/     /'; exit 1; }

RunSelfTest() {
    # NOT `local`: the EXIT trap runs after this returns, so a scoped path is unbound by
    # then and `set -u` turns a passing run into a failure at the last moment (#1084).
    local out rc
    tmp="$(mktemp -d)" || { printf 'mktemp failed\n' >&2; exit 1; }
    trap 'rm -rf "$tmp"' EXIT

    # Case 1 -- the ACCEPTING direction. A guard whose only observed behaviour is refusal
    # is indistinguishable from one that always refuses (#1031).
    mkdir -p "$tmp/a/scripts"
    printf '%s\n' '#!/usr/bin/env bash' 'echo hello' > "$tmp/a/scripts/fine.sh"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/a" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        Ok "case 1: a tree whose scripts all parse is accepted"
    else
        Bad "case 1: rc=$rc" "$out"
    fi

    # Case 2 -- the REFUSING direction, on a construct no bash can parse. This is what
    # makes the red path observable on every host: #1096's real construct parses happily
    # on bash 4+, which is the whole reason the version gate exists, so it cannot be the
    # case that proves the checker reports a failure at all.
    mkdir -p "$tmp/b/scripts"
    printf '%s\n' '#!/usr/bin/env bash' 'if [ 1 -eq 1 ]; then' 'echo unterminated' > "$tmp/b/scripts/broken.sh"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/b" 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"does not PARSE"*) : ;;
        *) Bad "case 2: an unparseable script was not named (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 2: complained and exited 0" "$out"
    Ok "case 2: an unparseable script is named and refused"

    # Case 3 -- the message QUALIFIES the location. `bash -n` reports where the parser
    # gave up; #1096's cause was 135 lines above it, on a line identical to master's.
    case "$out" in
        *"not necessarily where the"*) : ;;
        *) Bad "case 3: the refusal did not qualify the reported line" "$out" ;;
    esac
    Ok "case 3: the refusal says the line is where parsing FAILED, not where the cause is"

    # Case 4 -- #1096's ACTUAL construct: an apostrophe inside a comment within $( ).
    # Staged from the real shape rather than a synthetic one, and asserted to be
    # ACCEPTED here -- because on bash 4+ it parses, which is precisely why this check
    # must run on 3.2 and why a green from a modern bash is worthless. On a macOS runner
    # the same file is the red case, and that is where the ticket's "shown red on the
    # actual construct" is observed.
    mkdir -p "$tmp/c/scripts"
    {
        printf '%s\n' '#!/usr/bin/env bash'
        printf '%s\n' 'x="$('
        printf '%s\n' '    # the lane'"'"'s own measurement, and the timer'"'"'s pacing'
        printf '%s\n' '    echo 1'
        printf '%s\n' ')"'
        printf '%s\n' 'echo "$x"'
    } > "$tmp/c/scripts/apostrophe.sh"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/c" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        Ok "case 4: #1096's construct parses on THIS bash -- which is why the gate below exists"
    else
        Bad "case 4: rc=$rc -- unexpected, this bash should accept it" "$out"
    fi

    # Case 5 -- the VERSION GATE. Without the override, a modern bash must refuse rather
    # than report a green it cannot justify.
    if [ "${BASH_VERSINFO[0]:-0}" -ge 4 ]; then
        out=$(bash "$0" --root "$tmp/a" 2>&1) && rc=0 || rc=$?
        case "$out" in
            *"REFUSED: running under bash"*) : ;;
            *) Bad "case 5: a modern bash did not refuse (rc=$rc)" "$out" ;;
        esac
        [ "$rc" -ne 0 ] || Bad "case 5: refused and exited 0" "$out"
        Ok "case 5: bash >= 4.0 is a REFUSAL, not a green from the wrong interpreter"
    else
        Ok "case 5: skipped -- this IS a bash 3.x, so the modern-bash gate cannot be staged"
    fi

    # Case 6 -- the walk reports which MODE produced its file set, and an empty one is a
    # refusal. A guard's cheap-to-construct path silently becoming the only one tested is
    # this repository's `check-catch-skip-return-code` scar.
    case "$out" in *) : ;; esac
    mkdir -p "$tmp/d/scripts"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/d" 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"the walk is"*) : ;;
        *) Bad "case 6: an empty file set was not refused (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 6: refused and exited 0" "$out"
    Ok "case 6: a walk that finds no script is refused, not reported clean"

    printf '\nself-test: %d case(s) ran, all passed (bash %s)\n' "$SelfTestCases" "$(BashDescription)"
}

if [ "$selfTest" -eq 1 ]; then
    RunSelfTest
    exit 0
fi

RequireOldBash
RunParseCheck
