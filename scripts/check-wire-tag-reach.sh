#!/usr/bin/env bash
#
# Every Catch2 case in a wholly-wire test file carries `[wire]` (#1285).
#
# WHY A SCAN AND NOT JUST THE TAG RUN. `wire-tag-reach-count` runs
# `FastCacheTest "[wire]"` and pins how many cases RAN, which catches a tag being
# deleted, a case being removed, or the binary losing a translation unit. It cannot
# catch the thing that actually happened: a case ADDED without the tag leaves the
# pinned count untouched and the run green. That is how `[wire]` came to reach 69 of
# the 119 cases in `CompileCacheWire_test.cpp` -- the tag arrived partway through the
# file's life and was never backfilled, and every count anyone took was correct about
# a set that was not the one they meant.
#
# So the two registrations are complements and neither is redundant:
#   - the RUN pins the population downward (a tag lost, a case gone),
#   - this SCAN pins it upward (a case arriving untagged).
#
# WHAT THIS CHECKS, precisely: for each file in the table below, the number of case
# headers equals the number of `[wire]` mentions. It is a count identity rather than a
# per-case walk, and that is deliberate -- a C++ header walk in bash 3.2 is a parser
# nobody should maintain, and the identity is exactly the property. A tag string
# carries `[wire]` at most once, so the two numbers agree iff every case carries it.
#
# STATED BLIND SPOT, and the direction matters. Full-line comments are stripped, so a
# `[wire]` mentioned in a trailing comment on a line of code would be counted and could
# MASK one missing tag. That fails toward "nothing unusual here", so it is named rather
# than left for a reader to rediscover; what it cannot do is invent a violation. A
# `[wire]` inside a case NAME would do the same. Neither exists today and both would be
# odd things to write; if one lands, this check reports a count identity that holds for
# the wrong reason, and the tag RUN above is what still disagrees.
#
# WHY A TABLE AND NOT A GLOB. "Every case here is a wire case" is a judgement about a
# file's subject, not something derivable from its name or contents:
# `ClusterState_test.cpp` carries 16 wire cases among 47, and
# `SchedulerService_test.cpp` carries 1 among 57. A glob over `*Wire*` would demand the
# tag on cases that are not about the wire, and a check that refuses correct code is
# one somebody disables. Each row therefore states why the whole file qualifies.
#
# Needs no compiler, no build, no daemon, no socket and no network -- it reads source.
set -o errexit
set -o nounset
set -o pipefail

Failed=0
Ran=0

# --- the table ---------------------------------------------------------------
#
# Rows are `<path>|<reason the WHOLE file is wire subject>`.
#
# TOTAL: 4 rows. Derived from nothing -- this is a decision, so the figure is the
# table's own length and is asserted against it below rather than restated in prose.
WireFiles=(
    "src/FastCache/Protocol/CompileCacheWire_test.cpp|the 0xFC framing and its field grammars; the file is nothing but the wire"
    "src/FastCache/Consensus/RaftWire_test.cpp|the Raft peer wire: handshake, framing, tag trailer"
    "src/FastCache/Cluster/DiscoveryWire_test.cpp|the LAN beacon datagram grammar and its PSK challenge"
    "src/FastCache/Consensus/RaftTypes_test.cpp|the one case here pins a transmitted enumerator's byte"
)

Fail() {
    echo "WIRE TAG REACH FAILED: $*" >&2
    Failed=$(( Failed + 1 ))
}

# Count case headers and `[wire]` mentions in one file.
# @param 1 File to read.
# Prints "<cases> <wire>".
CountOne() {
    local file="$1" cases wire stripped
    stripped="$(grep -v '^[[:space:]]*//' "$file" || true)"
    cases="$(grep -c '^TEST_CASE(' <<< "$stripped" || true)"
    wire="$(grep -o '\[wire\]' <<< "$stripped" | grep -c . || true)"
    echo "${cases:-0} ${wire:-0}"
}

Scan() {
    local root="$1" row file reason counts cases wire
    for row in "${WireFiles[@]}"; do
        file="${row%%|*}"
        reason="${row#*|}"
        Ran=$(( Ran + 1 ))
        if [ ! -f "${root}/${file}" ]; then
            # A row naming no file is a refusal, never a skip: the table is the
            # claim, and a path that has moved makes the claim silently vacuous.
            Fail "${file} is named in the table (${reason}) and does not exist."
            continue
        fi
        counts="$(CountOne "${root}/${file}")"
        cases="${counts%% *}"
        wire="${counts##* }"
        if [ "$cases" -eq 0 ]; then
            # Zero cases is a refusal for the same reason: an empty scan agrees
            # with every claim, so it cannot be a pass.
            Fail "${file} has no \`^TEST_CASE(\` headers at all. Either the file moved, or the case macro is no longer written at column 0 -- this check counts on that and would silently report clean."
            continue
        fi
        if [ "$cases" -ne "$wire" ]; then
            Fail "${file}: ${cases} case(s), ${wire} carrying \`[wire]\`. Every case in this file is a wire case (${reason}), so a case without the tag is one nobody selecting \`[wire]\` will run. Add \`\"[wire]\"\` to the case header -- and if the new case genuinely is NOT about the wire, the file has stopped being wholly wire subject and this row needs splitting rather than the tag being forced."
            continue
        fi
        echo "ok: ${file} -- ${cases} case(s), all tagged [wire]"
    done
}

SelfTest() {
    local scratch pass fail out rc
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    # Both directions. A guard nobody has watched ACCEPT is not known to work
    # either, so the accepting arm is first and is not decoration.
    mkdir -p "${scratch}/ok" "${scratch}/bad" "${scratch}/cmt"

    pass="${scratch}/ok/W_test.cpp"
    printf 'TEST_CASE("a", "[wire]")\n{\n}\n\nTEST_CASE("b",\n          "[wire][x]")\n{\n}\n' > "$pass"

    fail="${scratch}/bad/W_test.cpp"
    printf 'TEST_CASE("a", "[wire]")\n{\n}\n\nTEST_CASE("b")\n{\n}\n' > "$fail"

    # The third arm is the stated blind spot, asserted as a KNOWN limitation
    # rather than left implicit: a `[wire]` in a full-line comment must NOT be
    # counted, or a comment could mask a missing tag outright.
    printf '// mentions [wire] in prose\nTEST_CASE("a", "[wire]")\n{\n}\n\nTEST_CASE("b")\n{\n}\n' > "${scratch}/cmt/W_test.cpp"

    local cases wire
    read -r cases wire <<< "$(CountOne "$pass")"
    Ran=$(( Ran + 1 ))
    if [ "$cases" = "2" ] && [ "$wire" = "2" ]; then
        echo "self-test 1/3 ok: a fully tagged file counts 2 == 2 (continuation line read)"
    else
        Fail "self-test 1: expected 2 2 for a fully tagged file, got ${cases} ${wire}"
    fi

    read -r cases wire <<< "$(CountOne "$fail")"
    Ran=$(( Ran + 1 ))
    if [ "$cases" = "2" ] && [ "$wire" = "1" ]; then
        echo "self-test 2/3 ok: an untagged case makes the counts disagree (2 != 1)"
    else
        Fail "self-test 2: expected 2 1 for a file with one untagged case, got ${cases} ${wire}"
    fi

    read -r cases wire <<< "$(CountOne "${scratch}/cmt/W_test.cpp")"
    Ran=$(( Ran + 1 ))
    if [ "$cases" = "2" ] && [ "$wire" = "1" ]; then
        echo "self-test 3/3 ok: a [wire] in a full-line comment does not mask the missing tag"
    else
        Fail "self-test 3: expected 2 1 with a commented [wire], got ${cases} ${wire}"
    fi

    # The table's own length, asserted rather than written in prose above.
    Ran=$(( Ran + 1 ))
    if [ "${#WireFiles[@]}" -eq 4 ]; then
        echo "self-test ok: the table has its stated 4 rows"
    else
        Fail "the table has ${#WireFiles[@]} rows; the comment above says 4. Update both or neither."
    fi

    rm -rf "$scratch"
    trap - EXIT
    echo "wire-tag-reach --self-test: ${Ran} check(s) ran, ${Failed} failed"
    [ "$Failed" -eq 0 ]
}

Root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
Mode="scan"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test) Mode="selftest"; shift ;;
        --root)      Root="$2"; shift 2 ;;
        *)           echo "usage: $0 [--root <dir>] | --self-test" >&2; exit 2 ;;
    esac
done

if [ "$Mode" = "selftest" ]; then
    SelfTest
    exit $?
fi

Scan "$Root"
echo "wire-tag-reach: ${Ran} file(s) checked, ${Failed} failed"
[ "$Failed" -eq 0 ]
