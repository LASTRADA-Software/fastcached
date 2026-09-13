#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The cluster key must live in a container that zeroes its storage.
#
# #324 was six holders of one secret, every one of them a plain `std::vector<std::byte>`
# that freed the bytes without touching them. Fixing the six is not the same as fixing
# the defect: a seventh holder is one declaration away, it looks exactly like the six
# did, and nothing about a plain vector reports that it is holding key material. This is
# the scan that makes the seventh visible.
#
# WHAT IT CANNOT SEE, stated rather than implied. It reads NAMES. A credential declared
# under a name this table does not carry is invisible to it, and no scan over identifiers
# can be otherwise -- a cache is full of variables called `key` that hold nothing secret,
# so a vocabulary wide enough to catch every credential would refuse most of the tree. So
# the table is the claim: these identifiers denote cluster-key material, and each one is
# required to still MATCH something, because a term that has quietly stopped matching is a
# guard that has quietly stopped guarding and reads identically to a clean tree (#492).
#
# Usage:
#   bash scripts/check-credential-containers.sh [--root <dir>]
#   bash scripts/check-credential-containers.sh --self-test

set -uo pipefail

Root=""
SelfTest=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root) Root="${2:-}"; shift 2 ;;
        --self-test) SelfTest=1; shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

# NOT named `fail`. That is the reserved name of the shared helper in
# `scripts/lib/e2e-common.sh`, and `check-e2e-helpers.sh` refuses a second definition of it
# -- correctly, and it caught this file. Sourcing the shared one is the wrong repair here:
# that `fail` signals the top-level pid because e2e fixtures call it from inside `$( )`, and
# it would make this check exit **143**, where a status that is neither 0 nor 1 means the
# instrument failed rather than the tree being bad. This scan deliberately does not run
# inside a command substitution -- findings and the count land in globals -- so a plain
# `exit` IS the script's exit. Every other hygiene check here draws the same distinction by
# the same means: `Fail`, `refuse`, `Abort`.
Refuse() {
    printf 'REFUSED: %s\n' "$1" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# The table. One row per identifier that denotes cluster-key material, with the
# reason it is on the list -- so a future reader can judge a row rather than
# inherit it.
# ---------------------------------------------------------------------------
CredentialIdentifiers='
signingKey|what a lease grant is MACed with; SchedulerService and SignedLeaseValidator hold it
_signingKey|the SchedulerService member holding the same
presharedKey|the cluster PSK, which MACs discovery proofs
clusterKey|the cluster key an enrolling node is handed and writes to disk; EnrollReading holds it
'
# `clusterKey` WAS excluded, and the note said "nothing here is called that" -- true when
# it was written and false now. It was excluded because a term matching nothing is a
# REFUSAL here (that is case 3, and deliberately so), not because the name is dangerous:
# matching is `\b`-anchored, so the row cannot reach `clusterKeyFile`, which is a PATH and
# not the secret, per the rule that a secret reached BY PATH is a different question.
# Measured before the row was added: `\bclusterKey\b` matches the span views and the
# `EnrollReading` member, and NO declaration of a plain owning type -- so it refuses
# nothing today and guards the enrolment path, which had no row and no way to acquire one
# while its member was called `key`.
#
# A bare `key` still cannot be a row. This is a cache: `key` is the thing being cached in
# most of the tree, so that row would refuse hundreds of correct declarations, and a guard
# that has to be suppressed everywhere is one somebody deletes. That is what forced the
# RENAME rather than a wider vocabulary -- the holder is named for what it holds.

# Types that OWN bytes. A borrowing view (`std::span`, `BytesView`, `std::string_view`)
# is deliberately absent: it owns no storage, so there is nothing for it to zero, and
# demanding a secure type of a view would refuse every correct boundary in the tree.
OwningTypes='
std::vector<std::byte>
std::vector<uint8_t>
std::vector<std::uint8_t>
std::string
'

# ---------------------------------------------------------------------------
# The scan
# ---------------------------------------------------------------------------

# Print every source file under a root. A GLOB, never a list: a list is exact about the
# files it knows and silent about the ones it does not, and silence reads identically to
# complete coverage (#492).
# $1: root directory
SourceFiles() {
    find "$1" -type f \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | sort
}

# Strip full-line comments, so a rule quoted in a comment is not a call site. A comment
# is not a declaration, and two checks in this tree have already matched their own
# headers (#723).
#
# ONE awk rather than the three greps this used to be (`grep -n ''` into two `grep -v`).
# The output is byte-identical -- `NR ":" $0` is what `grep -n` prints, and the pattern
# tests the line itself rather than the numbered form of it, which is what the sentence
# above always meant.
#
# Three processes became one, which is worth less than it sounds and is kept for the
# clarity rather than the saving: MEASURED under Git Bash, awk costs 22.9 ms to start
# against grep's 21.3, so this trades three cheap starts for one about as cheap. What
# made the difference was running it once per FILE instead of once per (file, identifier),
# and then not running it on files that cannot match at all (#1331).
# reads a file path on $1
CodeLines() {
    awk '!/^[[:space:]]*(\/\/|\*)/ { print NR ":" $0 }' "$1" 2>/dev/null
}

# $1: root. Prints "file:line:text" for every offending declaration; sets Matched counts.
#
# FILES OUTER, IDENTIFIERS INNER, and a candidate pass in front of both -- the cost fix
# rather than a tidy-up (#1331). `CodeLines` is a property of the FILE alone, so running
# it inside the identifier loop re-derived the same stripped text once per identifier,
# four times per file here and a fifth the day somebody adds a row.
#
# MEASURED at 765 files and 4 identifiers, in PROCESS CREATIONS -- tool spawns plus
# subshell forks, because a `$( )` forks and costs 12.97 ms against a grep's 21.2:
#
#     original             12,506 spawns + 3,061 forks = 15,567
#     hoist alone           4,091 spawns + 3,826 forks =  7,917
#     hoist + candidates       364 spawns +    97 forks =    461
#
# Counting tool spawns ALONE said the hoist was a 3.06x win; the CI leg said 1.51x, and
# the difference was that the hoist had increased the FORK count while cutting spawns. A
# model and a counter that share an assumption agree with each other and with nothing
# else.
#
# What this must NOT lose is the per-identifier verdict: a term matching nothing is a
# REFUSAL here, deliberately, because that is what a rename leaves behind and it looks
# exactly like a clean tree. With the loops swapped there is no longer one counter alive
# per identifier pass, so the counts are carried in a parallel array instead -- and it is
# PARALLEL ARRAYS rather than an associative one because a hygiene script `ctest` runs is
# constrained to macOS's 2007 bash 3.2, which has no `declare -A`.
ScanRoot() {
    local root="$1"
    local files identifier reason ownedType findings totalMatches file hits
    local line text code index

    files=$(SourceFiles "$root")
    [ -n "$files" ] || Refuse "no source files under $root -- an empty population agrees with every rule"

    # The vocabulary, read once. `identCounts` is what the refusal below reads.
    local identNames=() identReasons=() identCounts=() alternation=""
    while IFS='|' read -r identifier reason; do
        [ -n "$identifier" ] || continue
        identNames+=("$identifier")
        identReasons+=("$reason")
        identCounts+=(0)
        # Built with shell string ops rather than a `$( )`, because a subshell FORK
        # is the cost this whole function is now organised around: measured under
        # Git Bash, `x=$(true)` is 12.97 ms against 21.2 ms for an entire `grep`,
        # while a builtin is free.
        if [ -z "$alternation" ]; then
            alternation="$identifier"
        else
            alternation="${alternation}|${identifier}"
        fi
    done <<< "$CredentialIdentifiers"

    # ONE pass to find the files worth opening, and it is the difference between
    # this scan costing minutes and costing seconds (#1331).
    #
    # WHY IT IS SAFE, which is the only interesting thing about it: a file with no
    # whole-word mention of ANY identifier contributes zero to every per-identifier
    # count and can produce no finding, so excluding it changes no number this
    # function reports. It is a SUPERSET filter -- comments are not stripped here,
    # so a file mentioning an identifier only in a comment still gets opened and
    # still contributes zero once `CodeLines` has stripped it. The anchoring is the
    # same `\b` as below, so it cannot reach `clusterKeyFile` either.
    #
    # `find -exec ... {} +` rather than passing the file list as arguments: 765
    # paths is roughly 46 KB of command line, and Windows caps a process's at about
    # 32 KB. `find` batches to whatever the platform allows.
    local candidates
    candidates=$(find "$root" -type f \( -name '*.cpp' -o -name '*.hpp' \) \
        -exec grep -lE "\\b(${alternation})\\b" {} + 2>/dev/null | sort)

    totalMatches=0
    findings=""

    # An EMPTY candidate list is not an error here: it means no file mentions any
    # identifier, and the per-identifier refusals below are what report that -- by
    # name, one per term, which is more useful than a single "nothing matched".
    while IFS= read -r file; do
        [ -n "$file" ] || continue

        # ONCE per file, for every identifier that follows.
        code=$(CodeLines "$file")
        [ -n "$code" ] || continue

        index=0
        while [ "$index" -lt "${#identNames[@]}" ]; do
            identifier="${identNames[$index]}"

            # Every code line mentioning this identifier as a whole word. The matcher is
            # unchanged: `\b`-anchored, so a row cannot reach `clusterKeyFile`.
            hits=$(grep -E "\\b${identifier}\\b" <<< "$code" 2>/dev/null)
            if [ -z "$hits" ]; then
                index=$((index + 1))
                continue
            fi

            while IFS= read -r hit; do
                [ -n "$hit" ] || continue
                identCounts[$index]=$(( ${identCounts[$index]} + 1 ))
                totalMatches=$((totalMatches + 1))
                line="${hit%%:*}"
                text="${hit#*:}"

                # The type must IMMEDIATELY PRECEDE the identifier. Asking whether both
                # appear anywhere on the line was the first version, and it was wrong
                # twice over on its first real run:
                #
                #   [[nodiscard]] inline std::string MintLeaseToken(std::span<std::byte const> signingKey, ...)
                #
                # matched, because `std::string` is the RETURN type and `signingKey` is a
                # span -- a correct declaration reported as a defect. And `std::string`
                # matched inside `std::string_view` on the next one, because a substring
                # search has no idea where a token ends. Both are the same mistake: a
                # pattern is broader than its author reads it as.
                while IFS= read -r ownedType; do
                    [ -n "$ownedType" ] || continue
                    # <type> [const] [&|*] <identifier>, and nothing else between.
                    if grep -qE "${ownedType}[[:space:]]+(const[[:space:]]*)?[&*]*[[:space:]]*${identifier}\\b" <<< "$text"; then
                        findings="${findings}${file}:${line}: ${identifier} declared as ${ownedType}
"
                    fi
                done <<< "$OwningTypes"
            done <<< "$hits"

            index=$((index + 1))
        done
    done <<< "$candidates"

    # The per-identifier verdict, unchanged in meaning and now asked after the walk
    # rather than at the end of each identifier's own pass. Still one refusal per term,
    # still naming the term and its reason.
    index=0
    while [ "$index" -lt "${#identNames[@]}" ]; do
        if [ "${identCounts[$index]}" -eq 0 ]; then
            Refuse "the identifier '${identNames[$index]}' matches nothing under ${root}.
       It is on the table because: ${identReasons[$index]}
       A term that has stopped matching is a guard that has stopped guarding, and it
       looks exactly like a clean tree. Either the code was renamed -- update the row --
       or the credential is gone and the row goes with it."
        fi
        index=$((index + 1))
    done

    if [ "$totalMatches" -eq 0 ]; then
        Refuse "no credential identifier matched anywhere under ${root}"
    fi

    # Both results land in globals. Running this in a command substitution would put it
    # in a subshell, which eats the count AND turns every `fail` into a status the parent
    # walks straight past -- the same shape as the trap defect above, one level up.
    ScanTotalMatches="$totalMatches"
    ScanFindings="$findings"
}

# ---------------------------------------------------------------------------
# Self-test: three trees, and the negative arms are the point.
# ---------------------------------------------------------------------------
RunSelfTest() {
    local cases=0 verdict
    # NOT `local`. The EXIT trap runs where a function local is out of scope, and under
    # `set -u` that expansion is an error inside the subshell the trap fires in, which
    # the parent then carries on past -- #1031's exact shape, met in the instrument
    # written to guard against a sibling of it.
    SelfTestTmp=$(mktemp -d) || Refuse "mktemp failed"
    trap 'rm -rf "$SelfTestTmp"' EXIT
    local tmp="$SelfTestTmp"

    # Case 1: a clean tree, where every identifier is present and secure. Asserting the
    # PASSING direction, because a guard nobody has watched accept is not known to work
    # (#1031 stood for two days as a guard that refused every tree).
    mkdir -p "$tmp/clean"
    cat > "$tmp/clean/a.hpp" <<'EOF'
SecureByteBuffer signingKey;
SecureByteBuffer _signingKey;
SecureByteBuffer presharedKey;
SecureByteBuffer clusterKey;

EOF
    verdict=$(bash "$0" --root "$tmp/clean" 2>&1)
    if grep -q '^ok: every credential identifier' <<< "$verdict"; then
        printf 'ok   case 1: a clean tree passes and says so\n'
    else
        printf 'FAIL case 1: clean tree refused or reported: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    # Case 2: a planted violation. The instrument is worth nothing until it has been
    # watched refusing something.
    mkdir -p "$tmp/dirty"
    cp "$tmp/clean/a.hpp" "$tmp/dirty/a.hpp"
    printf 'std::vector<std::byte> signingKey;\n' >> "$tmp/dirty/a.hpp"
    verdict=$(bash "$0" --root "$tmp/dirty" 2>&1)
    if grep -q 'signingKey declared as std::vector<std::byte>' <<< "$verdict"; then
        printf 'ok   case 2: a plain vector holding a credential is reported\n'
    else
        printf 'FAIL case 2: violation not reported. Got: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    # Case 3: a vocabulary term that matches nothing. This is the failure mode the
    # check exists to have -- it is what a rename leaves behind, and it is silent.
    mkdir -p "$tmp/blind"
    cat > "$tmp/blind/a.hpp" <<'EOF'
SecureByteBuffer signingKey;
SecureByteBuffer _signingKey;
SecureByteBuffer clusterKey;
EOF
    verdict=$(bash "$0" --root "$tmp/blind" 2>&1)
    if grep -q "identifier 'presharedKey' matches nothing" <<< "$verdict"; then
        printf 'ok   case 3: a term that has stopped matching is a refusal, not a pass\n'
    else
        printf 'FAIL case 3: blind term not refused. Got: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    # Case 4: an empty population. Two empty lists agree perfectly.
    mkdir -p "$tmp/empty"
    verdict=$(bash "$0" --root "$tmp/empty" 2>&1)
    if grep -q 'no source files' <<< "$verdict"; then
        printf 'ok   case 4: an empty tree is refused rather than reported clean\n'
    else
        printf 'FAIL case 4: empty tree not refused. Got: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    # Case 5: a comment naming the pattern is not a call site.
    mkdir -p "$tmp/comment"
    cp "$tmp/clean/a.hpp" "$tmp/comment/a.hpp"
    printf '// std::vector<std::byte> signingKey; -- the shape this check refuses\n' >> "$tmp/comment/a.hpp"
    verdict=$(bash "$0" --root "$tmp/comment" 2>&1)
    if grep -q '^ok: every credential identifier' <<< "$verdict"; then
        printf 'ok   case 5: a commented-out declaration is not a finding\n'
    else
        printf 'FAIL case 5: comment reported as a violation: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    # Case 6: the two shapes the first real run got WRONG. A return type is not a
    # declaration, and `std::string` is a prefix of `std::string_view`. Both were false
    # POSITIVES -- the direction that gets acted on, because a finding looks like work.
    mkdir -p "$tmp/precise"
    cat > "$tmp/precise/a.hpp" <<'EOF'
SecureByteBuffer signingKey;
SecureByteBuffer _signingKey;
SecureByteBuffer presharedKey;
SecureByteBuffer clusterKey;
inline std::string MintLeaseToken(std::span<std::byte const> signingKey, int claims);
bool Authenticate(std::span<std::byte const> signingKey, std::string_view token);
EOF
    verdict=$(bash "$0" --root "$tmp/precise" 2>&1)
    if grep -q '^ok: every credential identifier' <<< "$verdict"; then
        printf 'ok   case 6: a return type and a string_view are not declarations\n'
    else
        printf 'FAIL case 6: false positive returned: %s\n' "$verdict"; return 1
    fi
    cases=$((cases + 1))

    printf '\nself-test: %d case(s) run, all passed\n' "$cases"
    return 0
}

# ---------------------------------------------------------------------------

if [ "$SelfTest" -eq 1 ]; then
    RunSelfTest
    exit $?
fi

if [ -z "$Root" ]; then
    ScriptDir=$(cd "$(dirname "$0")" && pwd)
    Root="${ScriptDir}/../src"
fi
[ -d "$Root" ] || Refuse "no such directory: $Root"

ScanTotalMatches=0
ScanFindings=""
ScanRoot "$Root"

if [ -n "$ScanFindings" ]; then
    printf 'A credential is held in a container that does not zero its storage.\n\n'
    printf '%s' "$ScanFindings"
    printf '\nUse SecureByteBuffer (FastCache/Core/SecureBytes.hpp). It is a std::vector alias,\n'
    printf 'so every span-taking interface keeps working; adopting it is a type change.\n'
    exit 1
fi

# "mention(s)", not "declaration(s)". What is counted is code lines naming a credential
# identifier, and most of them are uses rather than declarations. Calling them declarations
# would be a unit error in a verdict, which is how two numbers that should never have been
# compared end up compared.
printf 'ok: every credential identifier is held in a zeroing container (%d mention(s) inspected)\n' \
    "$ScanTotalMatches"
