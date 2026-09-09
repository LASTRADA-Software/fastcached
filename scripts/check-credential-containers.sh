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
'
# `clusterKey` is deliberately NOT a row, and it was one until the check refused the real
# tree over it. Nothing here is called that: the secret's file-read form is a local named
# `key`, and `clusterKeyFile` is a PATH -- which is not the secret, per the rule that a
# secret reached BY PATH is a different question from the secret. A row matching only
# `clusterKeyFile` would have made this guard report on a filename forever.
#
# A bare `key` cannot be a row either. This is a cache: `key` is the thing being cached in
# most of the tree, so that row would refuse hundreds of correct declarations, and a guard
# that has to be suppressed everywhere is one somebody deletes.

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
# reads a file path on $1
CodeLines() {
    grep -n '' "$1" 2>/dev/null | grep -v '^[0-9]*:[[:space:]]*//' | grep -v '^[0-9]*:[[:space:]]*\*'
}

# $1: root. Prints "file:line:text" for every offending declaration; sets Matched counts.
ScanRoot() {
    local root="$1"
    local files identifier reason ownedType findings totalMatches identMatches file hits
    local line text

    files=$(SourceFiles "$root")
    [ -n "$files" ] || Refuse "no source files under $root -- an empty population agrees with every rule"

    totalMatches=0
    findings=""

    while IFS='|' read -r identifier reason; do
        [ -n "$identifier" ] || continue

        identMatches=0
        while IFS= read -r file; do
            [ -n "$file" ] || continue

            # Every code line mentioning this identifier as a whole word.
            local hits
            hits=$(CodeLines "$file" | grep -E "\\b${identifier}\\b" 2>/dev/null)
            [ -n "$hits" ] || continue

            while IFS= read -r hit; do
                [ -n "$hit" ] || continue
                identMatches=$((identMatches + 1))
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
        done <<< "$files"

        if [ "$identMatches" -eq 0 ]; then
            Refuse "the identifier '${identifier}' matches nothing under ${root}.
       It is on the table because: ${reason}
       A term that has stopped matching is a guard that has stopped guarding, and it
       looks exactly like a clean tree. Either the code was renamed -- update the row --
       or the credential is gone and the row goes with it."
        fi
    done <<< "$CredentialIdentifiers"

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
