#!/usr/bin/env bash
# A script that guards one prerequisite and then uses a second unguarded one reports a
# missing tool as a failed read.
#
# That is #1038: `check-pr-closing-keywords.sh` guarded `gh` and then used `jq` bare, so
# on a machine without `jq` the check did not say "jq is missing" -- it said the pull
# request body could not be read. A statement about the subject, produced by the absence
# of a tool. The audit that closed #1038 was run BY HAND, and a hand audit is exact
# about what it looked at and silent about what it did not (#1052).
#
# ## Where each list comes from, and why the split is that way round
#
# Three sets, and which one is TYPED is the whole design:
#
#   * POPULATION -- every external command INVOKED, derived. Never derived from what the
#     tree guards: the question this check asks is "is this invocation guarded", so a
#     population built from guards cannot contain a tool guarded NOWHERE, which is
#     exactly the pre-bug state. Measured: at `ed295468` a guard-derived set holds
#     `cc cmake curl gh git ninja nm openssl perl python3 sccache ...` and no `jq`, so
#     it could not have caught #1038 prospectively while looking like it worked.
#   * OPTIONAL -- `OptionalSeed` (typed, one issue per row) unioned with every tool the
#     tree is seen to guard (derived). The seed covers "nothing guards it yet"; the
#     derived half covers the next script's new tool with nobody editing anything.
#   * PREREQUISITE -- typed, with the evidence on the row. A missing row here is a FALSE
#     POSITIVE, noticed on the first run; a missing row in the seed is a silent false
#     negative. Type the list whose omissions are loud.
#
# An inverted arrangement -- population minus a prerequisite list, everything else must
# be guarded -- was tried and measured. It needs the prerequisite list to absorb ~36
# prose fragments and `want-*` case labels, so it becomes a junk drawer of English words
# in which a missing `curl` row hides exactly as well as it would in a seed, and it
# reddens whenever somebody edits an error message. The loudness property dies in the
# noise rather than being inflated by it.
#
# ## Why the blanking pass exists
#
# `check-e2e-helpers.sh` writes a bounded-command invocation inside a `<<'CANARY'`
# heredoc -- fixture data for its own scan, not a tool it runs. An exception list would
# work today and be forgotten at the next fixture, so the text is blanked BY
# CONSTRUCTION: heredoc bodies, single-quoted regions, comments, and plain
# double-quoted TEXT.
#
# This file's own self-test fixtures name a SYNTHETIC tool (`fixturetool`) rather than a
# real one, and that is deliberate: a realistic name is fixture text to this check and a
# live invocation to the tree's other scans, which are not heredoc-aware. Using a real
# bounded-command name made three of those scans refuse this file, and the only remedy
# available would have been a whole-file exemption row in a check another lane owns. A
# name no scan polices needs no exemption from any of them.
#
# Plain quoted text is blanked while a command substitution INSIDE it is kept, because
# `"$(jq -r ...)"` runs jq and `"the cluster has no leader"` runs nothing. Without that
# split the candidate set is 321 names including `a`, `and`, `cannot`, `nothing` -- and
# `check-tidy-sweep-database.sh:347`, a quoted string DESCRIBING a configure line, reads
# as an invocation of `cmake`.
#
# LIMITATION, stated rather than special-cased: a quoted string containing a bare
# command name is indistinguishable by shape from an invocation. The `$( )` split
# narrows this and cannot close it.
#
# AND A WARNING ABOUT THAT PARAGRAPH. Within an hour of it being written, a real parser
# bug in this file produced a finding that matched it exactly -- a `cmake` reported from
# a quoted string -- and it was very nearly filed as a known limitation. The cause was
# `${got%"${got##*[![:space:]]}"}`: nested braces, a scan stopping at the first `}`, and
# the double-quote polarity inverted for every line after it, so strings FOURTEEN lines
# downstream read as code. Nothing about the symptom distinguished the two.
#
# A documented limitation creates a category a real defect can be filed into, and it
# answers "is this the instrument or the subject?" in advance -- which is the question
# that would have found the bug. The author is the likeliest victim, having written the
# category and trusting it.
#
# This is NOT an argument for documenting fewer limitations; the alternative is silence,
# which is worse. It is an argument that **a finding matching a known limitation deserves
# MORE scrutiny than one that does not**, which is the reverse of the instinct.
#
# Bash 3.2: macOS ships a 2007 /bin/bash and a default-set script runs on every platform
# CI builds. No mapfile, no declare -A, no ${var^^}, no local -n.
set -uo pipefail

self="${BASH_SOURCE[0]}"
root="$(cd "$(dirname "$self")/.." && pwd)"

# Tools whose ABSENCE has produced a defect here, and which nothing in the tree
# necessarily guards yet. One row, one reason. Kept short so that it is read.
OptionalSeed="jq gh"

# Tools this repository assumes exist. Evidence on the row, from the tree, so a row is
# a measurement rather than an opinion. A wrong row here is a false positive on the
# first run.
#   git -- invoked in 9 of 51 scripts and guarded in 1. A repository tool assumes it.
Prerequisites="git"

# --- the blanking pass ------------------------------------------------------
Blank() {
    awk '
        # Emits only what the shell would RUN. A command substitution RESTARTS quoting,
        # so `$(` is a context rather than a character to step over: inside it a `'"'"'`
        # opens a quote again even though the substitution sits inside double quotes.
        function blank(s,   i, j, n, c, out, keep, depth) {
            n = length(s); out = ""
            for (i = 1; i <= n; i++) {
                c = substr(s, i, 1)
                if (sq) { out = out " "; if (c == "'"'"'") sq = 0; continue }
                if (c == "\\") { out = out "  "; i++; continue }
                if (c == "'"'"'" && !dq) { sq = 1; out = out " "; continue }
                if (c == "\"") { dq = !dq; out = out " "; continue }
                if (c == "$" && substr(s, i + 1, 1) == "(") {
                    sub_depth++; out = out "$("; i++; continue
                }
                if (c == ")" && sub_depth > 0) { sub_depth--; out = out ")"; continue }
                # A ${...} or $name inside double quotes is an EXPANSION the shell
                # performs, not inert text, so it survives the blanking. Dropping it
                # erased `for tool in "${RequiredTools[@]}"` and with it the indirection
                # that makes `jq` visible on master -- the check then reported "0 derived
                # from guards" over a tree that guards plenty.
                if (c == "$" && dq && sub_depth == 0) {
                    j = i
                    if (substr(s, i + 1, 1) == "{") {
                        # NESTED BRACES. `${got%"${got##*[![:space:]]}"}` closes three
                        # times, and stopping at the FIRST `}` leaves the rest of the
                        # expansion parsed as ordinary text -- which flips the
                        # double-quote polarity for every line after it, so quoted
                        # strings downstream read as code. That is how a fixture line
                        # saying `"cmake --preset ..."` became an unguarded invocation
                        # of cmake, 14 lines below the real defect and looking exactly
                        # like the limitation this file documents. A stated limitation
                        # is where people stop looking, so it must not be allowed to
                        # cover a parser bug.
                        depth = 0
                        j = i + 1
                        while (j <= n) {
                            if (substr(s, j, 1) == "{") depth++
                            else if (substr(s, j, 1) == "}") { depth--; if (depth == 0) break }
                            j++
                        }
                        if (j > n) j = n
                    } else {
                        j = i + 1
                        while (j <= n && substr(s, j, 1) ~ /[A-Za-z0-9_]/) j++
                        j--
                    }
                    out = out substr(s, i, j - i + 1); i = j; continue
                }
                keep = (!dq || sub_depth > 0)
                out = out (keep ? c : " ")
            }
            return out
        }
        {
            line = $0
            if (here != "") {
                if (line ~ ("^[ \t]*" here "[ \t]*$")) here = ""
                print ""
                next
            }
            if (sq) { print blank(line); next }
            if (line ~ /^[ \t]*#/) { print ""; next }
            # Comments are dropped ABOVE and quotes are dropped BELOW, so heredoc
            # openers are read from a comment-free line with its quotes intact. Reading
            # the raw line lets a comment saying <<word open a phantom heredoc; reading
            # the blanked line destroys the delimiter of <<'"'"'EOF'"'"' so the body is
            # never skipped. Both orders are silently wrong.
            if (match(line, /<<-?[ \t]*"?'"'"'?[A-Za-z_][A-Za-z0-9_]*"?'"'"'?/)) {
                d = substr(line, RSTART, RLENGTH)
                gsub(/^<<-?[ \t]*/, "", d); gsub(/["'"'"']/, "", d)
                here = d
            }
            print blank(line)
        }
    ' "$1"
}

# Every tool guarded in one file, following ONE level of indirection.
#
# A literal `command -v jq` is the easy half. The half that matters is the shape
# #1038's own fix uses -- `RequiredTools=(gh jq)` iterated into `command -v "$tool"` --
# because reading only literal guards makes `jq` invisible on master too.
GuardedIn() {
    local blanked var arr guardKey
    guardKey="$(Blanked "$1")"; guardKey="${guardKey%.blank}.guards"
    if [ -s "$guardKey" ]; then cat "$guardKey"; return 0; fi
    _GuardedIn "$1" | sort -u > "$guardKey"
    cat "$guardKey"
}

_GuardedIn() {
    local blanked var arr
    blanked="$(cat "$(Blanked "$1")")"

    printf '%s\n' "$blanked" \
        | sed -nE 's/.*command -v[[:space:]]+([A-Za-z_][A-Za-z0-9_.-]*).*/\1/p'

    for var in $(printf '%s\n' "$blanked" \
                 | sed -nE 's/.*command -v[[:space:]]+"?\$\{?([A-Za-z_][A-Za-z0-9_]*).*/\1/p' \
                 | sort -u); do
        for arr in $(printf '%s\n' "$blanked" \
                     | sed -nE 's/.*for[[:space:]]+'"$var"'[[:space:]]+in[[:space:]].*\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}.*/\1/p' \
                     | sort -u); do
            printf '%s\n' "$blanked" \
                | sed -nE 's/^[[:space:]]*'"$arr"'=\((.*)\)[[:space:]]*$/\1/p' \
                | tr ' \t' '\n' | grep -E '^[A-Za-z_][A-Za-z0-9_.-]*$' || true
        done
        printf '%s\n' "$blanked" \
            | sed -nE 's/^[[:space:]]*for[[:space:]]+'"$var"'[[:space:]]+in[[:space:]]+([^;]*);.*/\1/p' \
            | tr ' \t' '\n' | grep -E '^[A-Za-z_][A-Za-z0-9_.-]*$' || true
    done
}

# Blanking is an awk process per call, and the naive arrangement calls it once per
# (file, tool) pair -- 53 files by 19 tools by two seds is about 1700 awk invocations and
# measured 47 SECONDS on this tree. A default-set check that costs 47s is one people move
# out of the default set. Blank each file ONCE into a cache and read that instead: 1.5s.
#
# Files rather than an associative array, because bash 3.2 has none.
_cache=""
Blanked() {
    local key="${_cache}/$(printf '%s' "$1" | tr '/.' '__')"
    [ -s "${key}.blank" ] || Blank "$1" > "${key}.blank"
    printf '%s\n' "${key}.blank"
}

# Command POSITION, not "the word appears": `git` inside a sentence is prose.
CommandPositions() {
    local key blanked
    blanked="$(Blanked "$1")"
    key="${blanked%.blank}.pos"
    if [ ! -s "$key" ]; then
        {
            grep -vE 'command -v' "$blanked" \
                | sed -nE 's/^[[:space:]]*([a-z][a-z0-9_.-]*)[[:space:]]+.*/\1/p'
            grep -vE 'command -v' "$blanked" \
                | sed -nE 's/.*(\|\||&&|[|;]|\$\()[[:space:]]*([a-z][a-z0-9_.-]*)[[:space:]]+.*/\2/p'
        } | sort -u > "$key"
    fi
    cat "$key"
}

# A HERESTRING, never a pipe. Piping a producer into a quiet matcher is a false NEGATIVE
# under `pipefail`, and it fails on the SUCCESS path: the matcher exits at its first hit,
# the producer takes SIGPIPE, and pipefail reports the producer's status -- so "the tool
# IS used" reads as "it is not" precisely because it is (#970). The idiom is not spelled
# out here either: this file is walked by the scan that bans it, and a comment is text
# the scan reads.
UsesTool() {
    local positions
    positions="$(CommandPositions "$2")"
    grep -qx -- "$1" <<< "$positions"
}

# NUMBER FIRST, then filter. Filtering `command -v` lines out before `grep -n` shifts
# every line number after them, so the check reported `jq` at 318-319 where the file has
# it at 319-320 -- a diagnostic that names the wrong line is worse than one that names
# none, because the reader trusts it and goes looking there.
LinesUsing() {
    grep -n '' "$(Blanked "$2")" \
        | grep -vE ':[[:space:]]*[^:]*command -v' \
        | grep -E ':[[:space:]]*([^:]*(\|\||&&|[|;]|\$\())?[[:space:]]*'"$1"'([[:space:]]|$)' \
        | cut -d: -f1 | tr '\n' ' '
}

# --- enumeration ------------------------------------------------------------
#
# The MODE is part of the output and an empty set is REFUSED, never reported clean. The
# hand audit this replaces once enumerated nothing -- pointed at a directory that was
# not a git repository -- and reported that as success.
#
# `cd` in a subshell rather than `git -C "$root"`: on Git Bash for Windows `$root` is a
# POSIX path the Windows git refuses as `-C`, and the failure is SILENT. This check fell
# back to `walk` on the developer host while reporting a clean run, and it was visible
# only because the mode is printed.
Enumerate() {
    local mode="" out=""
    if ( cd "$root" && git rev-parse --git-dir ) >/dev/null 2>&1; then
        mode="git"
        out="$( cd "$root" && git ls-files -- 'scripts/*.sh' 2>/dev/null )"
    fi
    if [ -z "$out" ]; then
        mode="walk"
        out="$( cd "$root" && find scripts -name '*.sh' -type f 2>/dev/null | sort )"
    fi
    printf '%s\n' "$mode"
    printf '%s\n' "$out"
}

IsPrerequisite() {
    case " $Prerequisites " in *" $1 "*) return 0 ;; esac
    return 1
}

Main() {
    local listing mode files count f t showCandidates="no"
    _cache="$(mktemp -d 2>/dev/null)" || { echo "CMake Error: cannot mktemp" >&2; return 1; }
    trap '[ -n "$_cache" ] && rm -rf "$_cache"' RETURN
    [ "${1:-}" = "--candidates" ] && showCandidates="yes"
    listing="$(Enumerate)"
    mode="$(printf '%s\n' "$listing" | head -1)"
    files="$(printf '%s\n' "$listing" | tail -n +2 | grep . || true)"
    count="$(printf '%s\n' "$files" | grep -c . || true)"

    echo "== unguarded prerequisites: enumerated ${count} script(s) by ${mode}"
    if [ "$count" -eq 0 ]; then
        echo "CMake Error: unguarded-prerequisites: the file set is EMPTY, so this check" >&2
        echo "  examined nothing. That is a refusal, not a clean tree -- an enumeration" >&2
        echo "  that came back empty reads identically to complete coverage." >&2
        return 1
    fi

    local derived optional seedCount derivedCount
    derived=""
    for f in $files; do
        derived="${derived}$(GuardedIn "${root}/${f}")
"
    done
    derived="$(printf '%s\n' "$derived" | grep . | sort -u)"
    derivedCount="$(printf '%s\n' "$derived" | grep -c . || true)"
    seedCount="$(printf '%s\n' $OptionalSeed | grep -c . || true)"

    optional="$(printf '%s\n%s\n' "$derived" "$(printf '%s\n' $OptionalSeed)" | grep . | sort -u)"

    # Printed SEPARATELY, so a tree where the seed has quietly become the whole set
    # says so rather than looking derived.
    echo "   optional tools: ${seedCount} seeded, ${derivedCount} derived from guards"

    if [ "$derivedCount" -eq 0 ]; then
        echo "CMake Error: unguarded-prerequisites: nothing in the tree guards anything, so" >&2
        echo "  the derived half is empty and every file would pass on the seed alone." >&2
        return 1
    fi

    local problems=0 guards positions
    for f in $files; do
        guards="$(GuardedIn "${root}/${f}" | sort -u)"
        [ -n "$guards" ] || continue
        # ONCE per file, not once per (file, tool): the per-pair form is 53 x 19 = about
        # a thousand calls and each one is a fork.
        #
        # MEASURED, and the conditions matter more than the number -- Git Bash on
        # Windows, 53 scripts, where a fork costs an order of magnitude more than on the
        # Linux runners this actually gates. Per-phase, one pass each: blanking 3.0s,
        # GuardedIn 6.7s, CommandPositions 5.4s. Whole-run wall time moved 47s -> 32s
        # with this change, and a repeat run under other load read 40s, so treat these as
        # a band and not a figure.
        #
        # The instructive part is that the FIRST optimisation was aimed wrong: caching the
        # blanked text bought about 2s, because blanking was never where the time went.
        # Profile before optimising, even when the expensive-looking thing is obvious.
        positions="$(CommandPositions "${root}/${f}")"
        for t in $optional; do
            IsPrerequisite "$t" && continue
            grep -qx -- "$t" <<< "$guards" && continue
            grep -qx -- "$t" <<< "$positions" || continue
            echo "CMake Error: unguarded-prerequisites: ${f} guards a prerequisite and then" >&2
            echo "  uses '${t}' unguarded, at line(s): $(LinesUsing "$t" "${root}/${f}")" >&2
            echo "  A missing '${t}' is reported as a failed read rather than a missing tool." >&2
            problems=$(( problems + 1 ))
        done
    done

    # Non-gating. The seed's omissions are silent, and this is what makes them findable:
    # a genuinely new unguarded tool appears here the day it is introduced, without
    # anybody having thought to seed it. It costs nothing to be wrong, so it needs no
    # list to keep it quiet.
    local unclassified="" known="" defined=""
    # Shell keywords and builtins are not tools. Listed rather than derived because a
    # shell's builtin set is a property of the SHELL, not of this tree.
    known="if then else elif fi for while until do done case esac in function select
time echo printf local exit return shift export set trap readonly unset eval exec read
source cd pwd test true false break continue declare typeset let alias command builtin
wait jobs kill umask ulimit getopts hash type times enable shopt"
    # Functions this tree defines are not tools either -- DERIVED, because that set
    # changes with every commit and a list of it would be stale immediately.
    for f in $files; do
        defined="${defined}$(sed -nE 's/^[[:space:]]*(function[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\(\).*/\2/p' "${root}/${f}")
"
    done
    for f in $files; do
        unclassified="${unclassified}$(CommandPositions "${root}/${f}")
"
    done
    unclassified="$(printf '%s\n' "$unclassified" | grep . | sort -u \
        | grep -vxF "$(printf '%s\n' $optional $Prerequisites $known "$defined" | grep . | sort -u)" || true)"
    local uCount
    uCount="$(printf '%s\n' "$unclassified" | grep -c . || true)"
    # Behind a flag, NOT on every run. Measured at 91 names on this tree, of which
    # roughly a third are prose fragments and `want-*` case labels that no cheap
    # extraction can tell from a command. Ten lines of that on every passing run of a
    # default-set check teaches people to skip its output, and a check whose output is
    # skipped protects nothing -- which is the same failure as the junk-drawer
    # prerequisite list, one level quieter. The count alone would move on any prose
    # edit, so it is not printed either.
    #
    # The value is real and it is a REVIEW-TIME question: run this when a script gains a
    # new tool, and the next `jq` is in the list the day it is introduced.
    if [ "$showCandidates" = "yes" ]; then
        echo "   ${uCount} unclassified invocation candidate(s) -- neither guarded, seeded, a"
        echo "   prerequisite, a shell builtin nor a function this tree defines:"
        printf '%s\n' "$unclassified" | tr '\n' ' ' | fold -sw 74 | sed 's/^/     /'
        echo ""
    fi

    if [ "$problems" -gt 0 ]; then
        echo "CMake Error: unguarded-prerequisites: ${problems} site(s)" >&2
        return 1
    fi
    echo "   no script guards one prerequisite and then uses another unguarded"
    return 0
}

# Sourcing this file must define its functions and RUN NOTHING: the self-test drives
# `Blank`, `GuardedIn` and `Main` directly, and a file that executes on `source` cannot
# be tested without its output mixed into the test's own.

# --- the self-test ----------------------------------------------------------
#
# Drives BOTH directions. A check nobody has watched refuse is a check reporting PASS
# over nothing, and every case here is arranged so that removing the behaviour it tests
# makes it fail rather than merely reporting less.
_selftest_failures=0
_selftest_ran=0

_st_stage() {
    # A synthetic tree with this check copied into it, so the check's own `$root`
    # resolves to the fixture rather than to this repository.
    local dir="$1"
    rm -rf "$dir"
    mkdir -p "${dir}/scripts"
    cp "$self" "${dir}/scripts/check-unguarded-prerequisites.sh"
}

_st_case() {
    local name="$1" dir="$2" wantStatus="$3" wantText="$4"
    local out status
    out="$( cd "$dir" && bash scripts/check-unguarded-prerequisites.sh 2>&1 )"
    status=$?
    _selftest_ran=$(( _selftest_ran + 1 ))
    if [ "$status" != "$wantStatus" ]; then
        echo "FAIL selftest/${name}: exit ${status}, expected ${wantStatus}" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        _selftest_failures=$(( _selftest_failures + 1 ))
        return
    fi
    if [ -n "$wantText" ] && ! grep -qF -- "$wantText" <<< "$out"; then
        echo "FAIL selftest/${name}: output does not carry '${wantText}'" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        _selftest_failures=$(( _selftest_failures + 1 ))
        return
    fi
    echo "ok: selftest/${name}"
}

SelfTest() {
    local tmp
    tmp="$(mktemp -d 2>/dev/null)" || { echo "CMake Error: selftest cannot mktemp" >&2; return 1; }

    # 1. THE POSITIVE CONTROL, against the real regression. Watched red, not a one-off.
    #    A guard-derived optional set cannot see `jq` here -- nothing in that tree
    #    guarded it -- which is why the seed exists.
    local pc="${tmp}/positive"
    _st_stage "$pc"
    if ( cd "$root" && git cat-file -e ed295468:scripts/check-pr-closing-keywords.sh ) 2>/dev/null; then
        ( cd "$root" && git show ed295468:scripts/check-pr-closing-keywords.sh ) \
            > "${pc}/scripts/check-pr-closing-keywords.sh"
        _st_case "positive-control-ed295468" "$pc" 1 "uses 'jq' unguarded"
    else
        # A REFUSAL, not a skip: this is the one case that proves the check can fail.
        echo "FAIL selftest/positive-control-ed295468: commit ed295468 is unreachable, so" >&2
        echo "     the only case proving this check CAN refuse did not run. That is a" >&2
        echo "     refusal, not a skip -- a shallow clone must fetch depth 0." >&2
        _selftest_ran=$(( _selftest_ran + 1 ))
        _selftest_failures=$(( _selftest_failures + 1 ))
    fi

    # 2. THE FIXED SHAPE reports nothing -- the same file with both tools guarded.
    local fx="${tmp}/fixed"
    _st_stage "$fx"
    cat > "${fx}/scripts/a.sh" <<'FIXED'
#!/usr/bin/env bash
set -uo pipefail
RequiredTools=(gh jq)
for tool in "${RequiredTools[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || exit 77
done
body="$(jq -r .body)"
gh api repos/x/y
FIXED
    _st_case "fixed-shape-is-clean" "$fx" 0 "no script guards one prerequisite"

    # 3. HEREDOC BODIES ARE DATA, and the pair is what makes it load bearing: the two
    #    trees differ ONLY by the heredoc wrapper, so a build that stopped skipping
    #    heredocs turns 3a red and one that skipped too much turns 3b green.
    #
    #    THE TOOL IS `fixturetool`, AND IT MUST NOT BE A REAL COMMAND NAME. A fixture
    #    here is data to THIS check -- it blanks heredoc bodies before looking at
    #    anything -- and live text to every other scan in the tree, none of which is
    #    heredoc-aware. Spelling a realistic bounded-command name made the tree's own
    #    `timeout` scan refuse this file for an invocation that exists only inside a
    #    quoted heredoc, and the only remedy on offer was a whole-file exemption row in
    #    a check owned by another lane.
    #
    #    So the fixture names something no scan polices. It costs nothing: the optional
    #    set is derived from the fixture tree itself, so any name works, and a synthetic
    #    one additionally cannot be mistaken for a real dependency by a reader.
    local hd="${tmp}/heredoc"
    _st_stage "$hd"
    cat > "${hd}/scripts/a.sh" <<'GUARD'
#!/usr/bin/env bash
set -uo pipefail
command -v jq >/dev/null 2>&1 || exit 77
command -v fixturetool >/dev/null 2>&1 || exit 77
GUARD
    cat > "${hd}/scripts/b.sh" <<'HEREDOC'
#!/usr/bin/env bash
set -uo pipefail
command -v jq >/dev/null 2>&1 || exit 77
cat > /tmp/fixture.sh <<'CANARY'
fixturetool 5 foo
CANARY
HEREDOC
    _st_case "heredoc-body-is-data" "$hd" 0 "no script guards one prerequisite"

    local hn="${tmp}/heredoc-negative"
    _st_stage "$hn"
    cp "${hd}/scripts/a.sh" "${hn}/scripts/a.sh"
    cat > "${hn}/scripts/b.sh" <<'BARE'
#!/usr/bin/env bash
set -uo pipefail
command -v jq >/dev/null 2>&1 || exit 77
fixturetool 5 foo
BARE
    _st_case "same-text-outside-a-heredoc-IS-flagged" "$hn" 1 "uses 'fixturetool' unguarded"

    # 4. AN EMPTY FILE SET IS REFUSED. The hand audit this replaces once enumerated
    #    nothing and reported it as success.
    local mt="${tmp}/empty"
    rm -rf "$mt"; mkdir -p "${mt}/scripts"
    cp "$self" "${mt}/elsewhere.sh"
    local out status
    out="$( cd "$mt" && bash elsewhere.sh 2>&1 )"
    status=$?
    _selftest_ran=$(( _selftest_ran + 1 ))
    if [ "$status" = "1" ] && grep -qF "the file set is EMPTY" <<< "$out"; then
        echo "ok: selftest/empty-set-is-refused"
    else
        echo "FAIL selftest/empty-set-is-refused: exit ${status}" >&2
        printf '%s\n' "$out" | sed 's/^/     | /' >&2
        _selftest_failures=$(( _selftest_failures + 1 ))
    fi

    rm -rf "$tmp"
    echo "selftest: ${_selftest_ran} case(s) run, ${_selftest_failures} failure(s)"
    [ "$_selftest_failures" -eq 0 ] || {
        echo "CMake Error: check-unguarded-prerequisites self-test failed" >&2
        return 1
    }
    return 0
}

if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    if [ "${1:-}" = "--self-test" ]; then
        SelfTest
        exit $?
    fi
    Main "$@"
fi
