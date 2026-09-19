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

# Which files are third-party, asked of the tree being checked (#1370). Upstream scripts
# are not this project's to keep parseable by bash 3.2, and ctest runs none of them.
# shellcheck source=lib/third-party-roots.sh
. "$(dirname "$0")/lib/third-party-roots.sh"

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
    local mode scripts file parsed failed unreadable out firstParty declined
    parsed=0
    failed=0
    unreadable=0

    # The MODE is part of the output and asserted by the self-test. A guard that reports
    # the same sentence whichever path it took is one whose cheap-to-construct path
    # silently becomes the only one tested.
    scripts="$(TrackedShellScripts)"
    mode="${scripts%%$'\n'*}"
    scripts="${scripts#*$'\n'}"

    # Declined after the mode is known and before anything is parsed, so both modes
    # decline alike. A roots file that cannot be read is a refusal: read as nothing, every
    # vendored script would be parsed as a first-party one.
    if ! firstParty="$(first_party_paths "$root" "$scripts")"; then
        printf 'FAIL check-bash32-parse: the third-party roots of %s could not be read (the reader says why above),\n' "$root" >&2
        printf '     so a vendored script cannot be told from a first-party one.\n' >&2
        return 1
    fi
    declined="$(third_party_paths "$root" "$scripts")"
    scripts="$firstParty"

    printf 'check-bash32-parse: parsing with bash %s, file set from %s\n' \
        "$(BashDescription)" "$mode"
    [ -z "$declined" ] \
        || printf 'check-bash32-parse: %s\n' "$(third_party_declined_summary 'shell script(s)' "$declined")"

    while IFS= read -r file; do
        [ -n "$file" ] || continue
        # A file the walk NAMED and this cannot read is its own outcome (#1564).
        #
        # This was `|| continue`, so the file left the run in silence and the summary
        # counted it nowhere: a tracked script that is absent, unreadable, or a dangling
        # symlink was scored exactly like one that parsed. `parsed` then answers a
        # question nobody asked -- how many of the files I could open did I open -- while
        # reading as how many of the tracked scripts were checked.
        #
        # It is not a parse failure either, and folding it into `failed` would send
        # whoever met it looking for a syntax error in a file they cannot open. So it is
        # a third number, each instance is NAMED, and a nonzero count refuses.
        #
        # `-r` rather than `-f`: a file that exists and cannot be opened is the case that
        # matters on a checkout with wrong permissions, and `-f` calls that one readable.
        if [ ! -r "${root}/${file}" ]; then
            unreadable=$((unreadable + 1))
            printf 'UNREADABLE %s was named by the %s file set and cannot be read here\n' \
                "$file" "$mode" >&2
            continue
        fi
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

    printf 'check-bash32-parse: %d script(s) parsed, %d unparseable, %d unreadable\n' \
        "$parsed" "$failed" "$unreadable"
    if [ "$unreadable" -ne 0 ]; then
        printf 'FAIL check-bash32-parse: %d script(s) named by the %s file set could not be\n' "$unreadable" "$mode" >&2
        printf '     read, so they were NOT parsed and nothing here is a verdict about them.\n' >&2
        printf '     They are listed above. This is not a parse failure -- do not go looking\n' >&2
        printf '     for a syntax error in them. Either the file set names something the tree\n' >&2
        printf '     no longer has (a stale index, a dangling symlink, a checkout that did not\n' >&2
        printf '     complete), or this host cannot open it (permissions, or a worktree whose\n' >&2
        printf '     pointers this git cannot follow -- `bash scripts/repair-worktree-pointers.sh`).\n' >&2
        return 1
    fi
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

    # Every synthetic tree states its third-party roots (#1370): one that did not would
    # be REFUSED, and a case expecting a refusal would then pass for the wrong reason.
    PlantRoots() {
        mkdir -p "$1/scripts/lib"
        printf '%s\n' '# planted' 'vendor/upstream' > "$1/scripts/lib/third-party-roots.txt"
    }

    # Case 1 -- the ACCEPTING direction. A guard whose only observed behaviour is refusal
    # is indistinguishable from one that always refuses (#1031).
    mkdir -p "$tmp/a/scripts"
    PlantRoots "$tmp/a"
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
    PlantRoots "$tmp/b"
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
    PlantRoots "$tmp/c"
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
    PlantRoots "$tmp/d"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/d" 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"the walk is"*) : ;;
        *) Bad "case 6: an empty file set was not refused (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 6: refused and exited 0" "$out"
    Ok "case 6: a walk that finds no script is refused, not reported clean"

    # Cases 7 and 8 -- a third-party script is DECLINED, by name, in BOTH modes (#1370).
    # The planted one does not parse, so a check that failed to decline it refuses the
    # tree: accepting is the proof, and the name in the output is what says why.
    local mode tree
    for mode in walk git; do
        tree="$tmp/vendored-$mode"
        mkdir -p "$tree/scripts" "$tree/vendor/upstream"
        PlantRoots "$tree"
        printf '%s\n' '#!/usr/bin/env bash' 'echo hello' > "$tree/scripts/fine.sh"
        printf '%s\n' '#!/usr/bin/env bash' 'if [ 1 -eq 1 ]; then' > "$tree/vendor/upstream/broken.sh"
        if [ "$mode" = git ]; then
            ( cd "$tree" && git init -q . && git add -A ) >/dev/null 2>&1 \
                || Bad "case 8: git could not stage the planted tree, so the git mode was not exercised"
        fi
        out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tree" 2>&1) && rc=0 || rc=$?
        [ "$rc" -eq 0 ] || Bad "case $mode: a tree whose only unparseable script is third-party was refused (rc=$rc)" "$out"
        case "$out" in
            *"file set from $mode"*) : ;;
            *) Bad "case $mode: the planted tree was not enumerated by $mode" "$out" ;;
        esac
        case "$out" in
            *"declined 1 third-party shell script(s) under the roots in scripts/lib/third-party-roots.txt, first vendor/upstream/broken.sh"*) : ;;
            *) Bad "case $mode: the declined script was not named" "$out" ;;
        esac
    done
    Ok "case 7: a third-party script is declined and named when the file set is walked"
    Ok "case 8: a third-party script is declined and named when the file set comes from git"

    # Case 9 -- a roots file naming no root is a REFUSAL, never "nothing is third-party".
    mkdir -p "$tmp/noroots/scripts/lib"
    printf '%s\n' '#!/usr/bin/env bash' 'echo hello' > "$tmp/noroots/scripts/fine.sh"
    printf '%s\n' '# no root at all' > "$tmp/noroots/scripts/lib/third-party-roots.txt"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/noroots" 2>&1) && rc=0 || rc=$?
    [ "$rc" -ne 0 ] || Bad "case 9: a roots file naming no root was accepted" "$out"
    case "$out" in
        *"the third-party roots of"*"could not be read"*) : ;;
        *) Bad "case 9: refused, but not for the roots file (rc=$rc)" "$out" ;;
    esac
    Ok "case 9: a roots file naming no root is refused, not read as an empty set"

    # Case 10 -- a file the FILE SET names and this cannot read (#1564).
    #
    # Staged through git, because that is the mode where it happens: the index names
    # what was committed, and the tree in front of you is what the checkout produced.
    # A stale index, an interrupted checkout, a dangling symlink and a permissions
    # problem all land here, and every one of them used to leave the run in silence --
    # `[ -f ... ] || continue` counted the file nowhere at all.
    #
    # The accepting control is in the same tree: one script that parses, so the refusal
    # is attributable to the unreadable file rather than to "this tree is odd", and the
    # summary has a nonzero `parsed` beside the nonzero `unreadable`.
    mkdir -p "$tmp/missing/scripts"
    PlantRoots "$tmp/missing"
    printf '%s\n' '#!/usr/bin/env bash' 'echo hello' > "$tmp/missing/scripts/fine.sh"
    printf '%s\n' '#!/usr/bin/env bash' 'echo also fine' > "$tmp/missing/scripts/gone.sh"
    ( cd "$tmp/missing" && git init -q . && git add -A ) >/dev/null 2>&1 \
        || Bad "case 10: git could not stage the tree, so the index-names-it case was not built"
    rm -f "$tmp/missing/scripts/gone.sh"
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/missing" 2>&1) && rc=0 || rc=$?
    [ "$rc" -ne 0 ] || Bad "case 10: a file the index names and the tree does not have was reported clean" "$out"
    case "$out" in
        *"UNREADABLE scripts/gone.sh"*) : ;;
        *) Bad "case 10: the unreadable file was not NAMED (rc=$rc)" "$out" ;;
    esac
    # THREE numbers, and the third is the point: folded into either of the others this
    # case would still refuse, and the reader would be sent to the wrong place.
    case "$out" in
        *"1 script(s) parsed, 0 unparseable, 1 unreadable"*) : ;;
        *) Bad "case 10: the summary did not report parsed/unparseable/unreadable apart" "$out" ;;
    esac
    case "$out" in
        *"not a parse failure"*) : ;;
        *) Bad "case 10: the refusal did not say it is NOT a parse failure" "$out" ;;
    esac
    Ok "case 10: a file the file set names and cannot be read is named, counted apart, and refused"

    # Case 11 -- and the direction that keeps case 10 from being satisfied by a check
    # that always refuses: an ordinary tree reports a zero in that same third column.
    # The column has to be PRESENT on a clean run, or nobody reading a green ever learns
    # it exists and a later regression to `|| continue` is invisible again.
    out=$(FASTCACHED_PARSE_ALLOW_MODERN_BASH=1 bash "$0" --root "$tmp/a" 2>&1) && rc=0 || rc=$?
    [ "$rc" -eq 0 ] || Bad "case 11: the clean tree was refused (rc=$rc)" "$out"
    case "$out" in
        *"1 script(s) parsed, 0 unparseable, 0 unreadable"*) : ;;
        *) Bad "case 11: a clean run did not report the unreadable count" "$out" ;;
    esac
    Ok "case 11: a clean run reports the unreadable count as zero rather than omitting it"

    printf '\nself-test: %d case(s) ran, all passed (bash %s)\n' "$SelfTestCases" "$(BashDescription)"
}

if [ "$selfTest" -eq 1 ]; then
    RunSelfTest
    exit 0
fi

RequireOldBash
RunParseCheck
