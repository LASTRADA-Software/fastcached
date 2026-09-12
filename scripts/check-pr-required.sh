#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# `ci-pr-required.sh` decides which required contexts block a merge. This asserts
# it is WIRED, and `--self-test` drives every verdict it can reach.
#
# The split is the one `ci-merge-group-report.sh` / `check-merge-group-report.sh`
# already uses, and for the same reason: the decision takes a record and calls no
# API, so every verdict is reachable from a synthesised file, while the wiring --
# that the tool exists and reads the SHARED required-context table rather than a
# copy of it -- is a property of the tree and is asserted here.
#
# A guard nobody has watched refuse is not a guard, so every case below states
# which direction it is testing and the negative ones assert the exact verdict
# rather than merely a non-zero exit. "It refused" and "it refused for the reason
# this case exists" are different claims, and only the second one tests anything.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

Tool=scripts/ci-pr-required.sh
Table=scripts/check-merge-queue-contexts.sh
problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--self-test" ]]; then
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT
    cases=0
    failures=0

    # A required-context table of its own, so no case depends on the repository's
    # real one. Three contexts is the smallest set that can show a mixed verdict.
    # It carries a COMMENT on purpose. The real table has an 8-line one, and every
    # text reader of it -- this tool's, `ci-merge-group-report.sh`'s and
    # `check-gated-jobs.sh`'s -- used to read each of those lines as a required
    # context name. Found by running the tool against a live pull request, not by
    # reading it: the phantoms appear only in output that names contexts one per
    # line, and the authoritative count comes from bash's own view of the array,
    # which cannot see a comment and stayed right throughout.
    cat > "$scratch/table.sh" <<'TABLE'
# required-context-table: fixture -- three SYNTHETIC names, so no case here depends on
# the repository's real table and no promotion to it moves a verdict below (#1360).
RequiredContexts=(
    "Alpha|.github/workflows/a.yml"
    "Beta|.github/workflows/a.yml"
    # A comment inside the table. It is not a context, and a reader that thinks it
    # is reports a required context no job can ever produce.
    "Gamma|.github/workflows/b.yml"
)
TABLE

    # @param 1 what this case is about
    # @param 2 the record, as `name<TAB>status<TAB>conclusion<TAB>started_at` lines
    # @param 3 expected exit: 0 mergeable, 1 blocked, 2 refused
    # @param 4 a substring the output must carry -- the VERDICT, not just the colour
    Case() {
        local what="$1" record="$2" wantExit="$3" wantText="$4" out got=0
        cases=$((cases + 1))
        # An EMPTY record is a real state -- a SHA with no check runs yet -- and
        # `printf` would write a single blank line instead, which the reader then
        # counts as one row with an empty name. That is the control firing on a
        # fixture artefact rather than on the property.
        if [[ -n "$record" ]]; then
            printf '%s\n' "$record" > "$scratch/record.tsv"
        else
            : > "$scratch/record.tsv"
        fi
        out="$(FASTCACHED_REQUIRED_CONTEXTS_FILE="$scratch/table.sh" \
               bash "$Tool" --record "$scratch/record.tsv" 2>&1)" || got=$?
        local ok=1
        [[ "$got" == "$wantExit" ]] || ok=0
        # Flattened before matching: a verdict wrapped across lines is a phrase
        # that exists in the output and in no single line of it.
        #
        # BOTH sides are squeezed. Flattening the haystack alone was the first
        # version, and it failed eight cases whose expected text was the tool's
        # own column-aligned output -- the pattern still carried the runs of
        # spaces the haystack no longer had, so every case reported "the tool did
        # not say this" about output that said exactly that.
        local haystack needle
        haystack="$(printf '%s' "$out" | tr '\n' ' ' | tr -s ' ')"
        needle="$(printf '%s' "$wantText" | tr -s ' ')"
        grep -q -- "$needle" <<< "$haystack" || ok=0
        if [[ $ok == 1 ]]; then
            echo "  ok    (exit $got) $what"
        else
            failures=$((failures + 1))
            echo "  FAIL  (exit $got, wanted $wantExit and /$wantText/) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
        fi
    }

    # Drive `ListingVerdict` directly. It is a pure function over two values the
    # API hands over, and the only alternative is a case that needs a network --
    # which is the population the check is not for.
    #
    # @param 1 what this case is about
    # @param 2 the `total_count` as text
    # @param 3 the rows read back
    # @param 4 the exact word expected
    Verdict() {
        local what="$1" total="$2" got="$3" want="$4" out=""
        cases=$((cases + 1))
        out="$(bash "$Tool" --listing-verdict "$total" "$got" 2>&1)" || true
        if [[ "$out" == "$want" ]]; then
            echo "  ok    (listing) $what"
        else
            failures=$((failures + 1))
            echo "  FAIL  (listing) $what -- said '$out', wanted '$want'" >&2
        fi
    }

    All="Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	success	2026-01-01T00:00:00Z"

    # The positive direction FIRST, and it is not decoration: every negative case
    # below is evidence only if a fully green record reports mergeable. A guard
    # nobody has watched ACCEPT is not known to work.
    Case "every required context green is MERGEABLE" "$All" 0 "every required context reports SUCCESS"

    # The four states #542 is about, each on its own, each asserted BY NAME --
    # because the whole complaint is that a count cannot tell them apart.
    Case "a required context that is absent is named ABSENT" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z" \
        1 "ABSENT     Gamma"

    Case "a required context that is skipped is named SKIPPED, not passing" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	skipped	2026-01-01T00:00:00Z" \
        1 "SKIPPED    Gamma"

    Case "a required context still running is named RUNNING, which is a wait" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	in_progress		" \
        1 "RUNNING    Gamma"

    Case "a required context that failed is named FAILED" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	failure	2026-01-01T00:00:00Z" \
        1 "FAILED     Gamma"

    # An unfinished job's conclusion arrives as "" rather than null, and reading
    # that as FAILED misclassified a context in #542's own body.
    Case "an empty conclusion is RUNNING, never FAILED" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed		2026-01-01T00:00:00Z" \
        1 "RUNNING    Gamma"

    # ---- #903, both directions -------------------------------------------
    #
    # These two records differ by ONE row, and that row is the whole ticket: a
    # cancellation with a replacement in flight is a wait, and the same
    # cancellation with nothing behind it is a blocker that will not clear.
    Case "a cancelled context with no replacement is WITHDRAWN (#903)" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	cancelled	2026-01-01T00:00:00Z" \
        1 "WITHDRAWN  Gamma"

    Case "the same cancellation with a replacement in flight is a benign wait" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	cancelled	2026-01-01T00:00:00Z
Gamma	queued		" \
        1 "RUNNING    Gamma"

    # And the case that actually happened, from the pair #903 names: a
    # cancellation SUPERSEDED by a later success. Measured on SHA a2f8359, which
    # still lists three `Require a type label` check runs -- one cancelled and
    # two successful. This must be MERGEABLE, or the tool fires on every
    # multi-label edit and is disabled within a week.
    Case "a cancellation already superseded by a later success is MERGEABLE (#903's benign pair)" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	cancelled	2026-09-06T03:42:26Z
Gamma	completed	success	2026-09-06T03:50:32Z" \
        0 "every required context reports SUCCESS"

    # ---- the sections and the refusals -----------------------------------
    Case "a non-required failure is reported and blocks NOTHING" \
        "$All
Package (macOS .pkg)	completed	failure	2026-01-01T00:00:00Z" \
        0 "non-required failures"

    Case "a conclusion nobody enumerated REFUSES rather than passing" \
        "Alpha	completed	success	2026-01-01T00:00:00Z
Beta	completed	success	2026-01-01T00:00:00Z
Gamma	completed	sideways	2026-01-01T00:00:00Z" \
        2 "state this script does not know"

    # ---- the fifth state: the instrument could not ask -------------------
    #
    # An exhausted API budget returns an ERROR, and a caller that reads the
    # RESULT of a `--jq` pipeline gets an EMPTY ANSWER -- which reads as *no
    # required context is failing*. Measured 2026-09-10 on this account: every
    # GraphQL call refusing while `gh api rate_limit` reported `graphql: 5000/5000`,
    # so a precondition check on `rate_limit` would have said the budget was
    # untouched. The tool must therefore be able to say COULD NOT ASK, and these
    # cases are what stop that arm being decoration.

    # A record whose rows are all real and none of them required. The reader and
    # the table have parted company -- which is what a mis-parsed table looks
    # like from the outside, and reporting every context ABSENT from here would
    # read as "CI has not started yet". A sibling instrument built the same
    # evening did exactly that across seven pull requests, one already red.
    Case "check runs present and NOT ONE required name matched is COULD NOT ASK, never all-absent" \
        "Package (macOS .pkg)	completed	success	2026-01-01T00:00:00Z
Some Other Job	completed	failure	2026-01-01T00:00:00Z" \
        3 "COULD NOT ASK"

    # And the case it must NOT fire on, which is the whole reason the control is
    # guarded on the RAW input rather than on the parse: a SHA with no check runs
    # at all is genuinely absent, not unread. Without this the previous case
    # could be satisfied by a control that refuses everything.
    Case "no check runs at all is ABSENT and says so, not COULD NOT ASK" \
        "" \
        1 "no check runs at all at this SHA"

    # `ListingVerdict` is a decision made during ACQUISITION. Driven directly,
    # because a decision reachable only through the network is one nobody has
    # watched in either direction -- and its empty arm shipped broken here.
    Verdict "a full page that is the whole set is ok" 30 30 "ok"
    Verdict "a listing that came back AT its cap is refused" 130 100 "capped"

    # THE LIVE HOLE. An unset total made the old guard compare 0 against 0, so a
    # response carrying no total at all passed it and every required context then
    # read ABSENT. `[[ "" -gt 0 ]]` is false, silently, and nothing about the rows
    # says the answer was about a set that is not the whole set.
    Verdict "an EMPTY total_count is unreadable, not zero" "" 0 "unreadable-total"

    # What an exhausted budget actually puts on the wire, rather than a tidy
    # empty string. `--jq` over an error body yields text, and text is not a
    # count.
    Verdict "an API error body where the total should be is unreadable" \
        "API rate limit exceeded for user ID 56763" 0 "unreadable-total"

    # An empty table is the failure this whole family has: two empty lists agree
    # perfectly, so a tool with nothing to check must refuse rather than report
    # that everything is fine.
    cp "$scratch/table.sh" "$scratch/table.full.sh"
    printf 'RequiredContexts=(\n)\n' > "$scratch/table.sh"
    Case "an empty required-context table is REFUSED, not read as all-green" "$All" 2 "vacuous"
    cp "$scratch/table.full.sh" "$scratch/table.sh"

    echo "check-pr-required: self-test ran $cases case(s), $failures failure(s)"
    [[ $failures -eq 0 ]] || exit 1
    echo "check-pr-required: self-test passed ($cases cases)"
    exit 0
fi

# ---------------------------------------------------------------------------
# The WIRING. Everything above is reachable from a staged record; these are
# properties of the tree, and each is a way the tool could exist and answer for
# nothing.
[[ -f "$Tool" ]] || Fail "$Tool does not exist, so #542's answer has no implementation"

# It must read the SHARED table. A copy would make this tool confidently wrong
# about the only question it answers, and the drift would be silent.
if grep -q 'check-merge-queue-contexts.sh' "$Tool"; then
    echo "ok: $Tool reads the required-context list from $Table"
else
    Fail "$Tool does not name $Table, so it is deciding from a second copy of the required-context list"
fi

if grep -q '^RequiredContexts=(' "$Table"; then
    echo "ok: $Table still declares RequiredContexts for it to read"
else
    Fail "$Table no longer declares \`RequiredContexts=(\`, so $Tool reads an empty list and would report every context ABSENT"
fi

# TWO mechanisms, cross-checked, because they disagreed and the disagreement was
# invisible.
#
# `check-merge-queue-contexts.sh` reports its count from `${#RequiredContexts[@]}`
# -- bash's own view of the array, which cannot see a comment. Every CONSUMER
# parses the table as text. Those two had drifted: the text readers accepted every
# line between the parentheses, so the 8-line comment above the last row became 8
# phantom required contexts. The authoritative count stayed right, which is
# exactly why nobody noticed -- and it surfaced here only because this tool is the
# first consumer that PRINTS one line per context, where a phantom shows up as a
# required context nothing produces.
#
# So this asserts the two agree, rather than asserting either alone.
byBash="$(awk '
    /^RequiredContexts=\(/ { inTable = 1; next }
    inTable && /^\)/       { inTable = 0 }
    inTable && /^[ \t]*"/  { n++ }
    END                    { print n + 0 }
' "$Table")"
byReader="$(FASTCACHED_REQUIRED_CONTEXTS_FILE="$Table" bash -c '
    source_file="$1"
    awk "
        /^RequiredContexts=\\(/ { inTable = 1; next }
        inTable && /^\\)/       { inTable = 0 }
        inTable && /^[ \\t]*\"/ { n++ }
        END                     { print n + 0 }
    " "$source_file"' _ "$Table")"

if [[ "$byBash" -eq 0 ]]; then
    Fail "the shared table names NO required contexts, so every verdict $Tool prints would be vacuous"
elif [[ "$byBash" != "$byReader" ]]; then
    Fail "the table's rows count $byBash one way and $byReader the other, so the tool and the table disagree about what a row IS"
else
    echo "ok: the shared table names $byBash required context(s), counted two ways"
fi

# And the reader must not admit a COMMENT. Asserted directly, because the count
# above passes whenever the two mechanisms are wrong together.
phantoms="$(bash "$Tool" --record /dev/null 2>&1 | grep -c '^  [A-Z]* *#' || true)"
if [[ "$phantoms" -eq 0 ]]; then
    echo "ok: no comment line in the table is read as a required context"
else
    Fail "$phantoms comment line(s) in $Table are being read as required contexts; a COMMENT is not a call site"
fi

# It must reach the API through REST ONLY.
#
# The GraphQL budget is per USER and every lane on this machine shares it.
# Measured 2026-09-10: `gh project item-list` and every other GraphQL call
# refusing with `API rate limit exceeded for user ID 56763` while REST answered
# normally -- and `gh api rate_limit` reporting `graphql: 5000/5000` throughout,
# so a precondition check on it would have said the budget was untouched.
#
# `gh pr view` is the tempting way to get a head SHA and is GraphQL. Reading it
# that way would make this tool unusable exactly when several lanes are working,
# which is exactly when somebody asks it which contexts are blocking. Nothing but
# this assertion keeps that true -- the immunity is a property of which endpoints
# are called, and the two spellings look alike at the call site.
graphqlDoors="$(grep -n -E 'gh (pr|issue|project|repo|release) ' "$Tool" | grep -v '^[0-9]*:[[:space:]]*#' || true)"
if [[ -z "$graphqlDoors" ]]; then
    echo "ok: $Tool reaches the API through REST only"
else
    Fail "$Tool calls a GraphQL-backed gh command, so it fails when the shared per-user GraphQL budget is spent: $graphqlDoors"
fi

if [[ $problems -gt 0 ]]; then
    echo "check-pr-required: $problems problem(s)" >&2
    exit 1
fi
echo "check-pr-required: wiring asserted"
