#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every `##` section of a rulebook file says whether it has a tripwire in
# `AGENT.md`, and names the phrase (#876).
#
# `AGENT.md` states the rule this enforces, from both sides:
#
#   "a rule landed in `.agent/rules/` without a bullet here fires in no session
#    that does not open its file -- which is the population the rule was written
#    for. It has shipped that way (#355), caught by a review rather than by the
#    author, and the reason it is easy is that writing the rule feels like the
#    work."
#
# ## Why the marker, and why not the thing the ticket asked for
#
# #876 asks for a check that a rulebook ENTRY has an `AGENT.md` tripwire, and
# its acceptance clause is refuted by `.agent/rules/README.md`, which worked the
# design out and recorded it rather than building it. Three findings stand, and
# each one kills a cheaper design:
#
#   * **The correspondence cannot be heading text.** README worked example is a
#     heading and a tripwire that state the same fact with no matchable text
#     between them. Any fuzzy ruler refuses correct entries and misses incorrect
#     ones -- which is the ticket own objection to a threaded-source census,
#     turned on the ticket.
#   * **A tripwire may live under ANOTHER file link, and may be a nested
#     bullet.** `compile-cache.md` "Two servers on one wire are two VERSIONS on
#     one wire" is tripwired as a sub-bullet under **`distributed-compilation.md`**,
#     correctly, because the rule spans both. So "a bullet under that file link"
#     would refuse a correct tree, and any reader keyed on a top-level `^- `
#     misses it silently. **That is why the phrase is searched over the WHOLE of
#     `AGENT.md`** rather than inside the file own section.
#   * **The rulebook own rule is CONDITIONAL.** README "Adding a rule" says to
#     add a tripwire *if the rule is one a reader could plausibly break without
#     noticing*. A check written to #876 acceptance clause would refuse entries
#     the rulebook says need no tripwire.
#
# So the claim travels in a MARKER the author writes, and the check verifies the
# claim rather than inventing the correspondence. That makes *yes* a statement
# somebody can be WRONG about instead of a box ticked: the phrase either is in
# `AGENT.md` or it is not.
#
# ## The marker is MANDATORY, and `none` is how a section opts out
#
# Not opt-in. An opt-in marker is exact about the sections it knows and silent
# about the ones it does not, and silence reads identically to complete coverage
# (#492). This is `check-table-totals.sh` `table-total: none` idiom one file
# over, and the `Refuse` / `RefuseWithoutCounter` idiom from
# `.agent/rules/metrics-and-observability.md` arriving in markdown: *deliberately
# untripwired must not be spelled like forgot*. A section with no tripwire says
# so and says WHY, which costs one line and buys the property that a section
# ARRIVING cannot join the tree unchecked.
#
#   <!-- agent-tripwire: a phrase that appears in AGENT.md -->
#   <!-- agent-tripwire: none: why this section needs no tripwire -->
#   <!-- agent-tripwire: untriaged: #123 nobody has decided about this one yet -->
#
# THREE spellings, three claims, because two cannot carry this: a rise in the
# first means the tripwire moved, `none` says *a rise would mean nothing, and
# why*, and `untriaged` says *nobody has decided, and which issue will*. That is
# the `Refuse` / `RefuseWithoutCounter` / `RefuseUntriaged` split from
# `.agent/rules/metrics-and-observability.md`, and the reason it is safe to have
# a third at all is the same reason it is safe there: **the check TALLIES the
# untriaged ones and prints the total per issue on every run**, so a placeholder
# cannot spell *forgot* in the vocabulary of *decided*. An `untriaged` marker
# must name an issue, or it is a backlog row with no owner.
#
# This spelling exists because the first seeding of these markers found sections
# with no `AGENT.md` tripwire at all -- #876 premise, confirmed on its first run.
# Spelling those `none:` would have recorded *this section needs no tripwire* for
# rules that need one. The COUNT is not written here: the check prints it, per
# issue, on every run, which is the one place it cannot drift. (It was written
# here, as "eight", and disagreed with the ctest registration's "seven" and with
# the live run before this sentence replaced it.)
#
# The marker carries no line number and no AGENT.md coordinate, deliberately:
# `AGENT.md` is several hundred bullets that several sessions edit at once, and
# a coordinate would be stale on arrival. A PHRASE survives everything except
# the tripwire being reworded or deleted, which are the two events worth hearing
# about.
#
# `AGENT.md` is only ever READ. That is the property that makes this affordable
# at all -- nothing here edits the file every session loads first.
#
# ## What this check does NOT cover, stated because a reader will over-apply it
#
#   * It does not decide whether the phrase is the RIGHT tripwire for the
#     section. It checks that the phrase the author nominated is present. A
#     marker quoting a real sentence about an unrelated rule passes.
#   * It does not check the reverse direction -- an `AGENT.md` bullet with no
#     rules-file section behind it is not refused. AGENT.md carries tripwires for
#     rules that live in source comments and in `.agent/guides/`, so that
#     direction has no denominator.
#   * It does not read `## Open work` or `## Accepted trade-offs` as rules. They
#     are sections of a rulebook file like any other and need a marker like any
#     other, and the honest answer for them is `none:`.
#   * It says nothing about a rule that is in neither file.
#
# ## Matching
#
# The haystack is `AGENT.md` flattened to one whitespace-normalised line with
# `*`, backtick and `_` removed; the needle is normalised the same way. Both
# halves matter and both have already been a bug in this repository:
#
#   * **Flattening**, because `AGENT.md` is hard-wrapped and a phrase WRAPPED
#     ACROSS A LINE reading as absent is a row of the table `check-table-totals`
#     exists to protect, and `.agent/rules/build-and-toolchain.md` carries the
#     same finding against CMake own wrapped diagnostics.
#   * **Markup stripping**, because these bullets are dense with `**bold**` and
#     backticks and somebody bolding half a phrase must not break a correct
#     marker. `check-table-totals.sh` strips exactly this set for exactly this
#     reason.
#
# A phrase shorter than MinPhraseWords words is REFUSED. Without a floor the
# marker degrades into the box-tick this design exists to avoid: "the" appears
# in `AGENT.md` and proves nothing.
#
# Scope is `.agent/rules/*.md`, README included. Excluding README by name would
# be an exclusion list -- a bet on the directory layout -- where five `none:`
# markers state the same thing and cannot go stale. Nothing under `.agent/` is
# vendored, so `third-party-roots` has no question to answer here;
# `check-table-totals.sh` sets that precedent one file over.
#
# Usage:
#   bash scripts/check-rulebook-tripwires.sh [<repo-root>]
#   bash scripts/check-rulebook-tripwires.sh --self-test
#
# Exit: 0 clean, 1 a rule was broken, 2 usage.

set -uo pipefail

# The floor on a nominated phrase, in whitespace-separated words after
# normalisation. Five, because four admits "A count that OVERSTATES" -- a
# fragment short enough to collide by accident across several hundred bullets --
# and a floor that admits an accidental match is not a floor.
MinPhraseWords=5

# How many lines after a `## ` heading the marker may sit on. 4, so that the
# conventional heading / blank / marker layout fits with room for a wrapped
# marker, and no further: a marker twenty lines down is under a PARAGRAPH, not
# under the heading, and which heading it belongs to stops being obvious.
MarkerWindow=4

# The awk program is the whole check, one pass per rules file, no shell loop over
# lines: these files are dense with `[#876](...)` and `|`, and every shell
# splitting idiom in this repository has been bitten by one or the other.
#
# NOTE: no apostrophes anywhere in this block -- it is a single-quoted shell
# string, and one ends it.
CheckAwk='
BEGIN {
    # The haystack: AGENT.md, already flattened and stripped by the caller, read
    # as one line. Read here rather than passed with -v, because a 200 KB -v
    # assignment is not portable across the awks this runs on.
    hay = ""
    if ((getline hay < HayFile) <= 0) {
        printf "FATAL could not read the normalised AGENT.md at %s\n", HayFile
        exit 1
    }
}
function refuse(line, msg) {
    printf "%s:%d: %s\n", FILENAME, line, msg
}
function normalise(s) {
    gsub(/[*`_]/, "", s)
    gsub(/[ \t]+/, " ", s)
    sub(/^ /, "", s); sub(/ $/, "", s)
    return s
}
# Does this file `## Open work` section name this issue? An `untriaged:` marker
# is deferred work, and deferred work in this rulebook lives as an `## Open work`
# entry -- which `rulebook-open-work-state` RESOLVES, refusing one whose issue has
# closed. Requiring the pairing is what puts these markers under that resolver:
# without it, the issue closes, the entry is forced out, and the markers go on
# printing a live-looking tally of a dead issue -- inside the guard whose whole
# safety argument IS that tally.
function inOpenWork(issue,   k, seen) {
    seen = 0
    for (k = 1; k <= NR; k++) {
        if (lines[k] ~ /^## /) seen = (lines[k] == "## Open work")
        else if (seen && lines[k] ~ ("#" issue "([^0-9]|$)")) return 1
    }
    return 0
}
{ lines[NR] = $0 }
END {
    fence = 0
    for (i = 1; i <= NR; i++) {
        if (lines[i] ~ /^[ \t]*(```|~~~)/) { fence = !fence; continue }
        if (fence) continue
        if (lines[i] !~ /^## /) continue
        headings++
        heading = substr(lines[i], 4)

        # The marker, within MarkerWindow lines below the heading and never past
        # the next heading.
        marker = ""; markerLine = 0
        for (j = i + 1; j <= NR && j <= i + MarkerWindow; j++) {
            if (lines[j] ~ /^#/) break
            if (lines[j] ~ /<!--[ ]*agent-tripwire:/) { marker = lines[j]; markerLine = j; break }
        }
        if (marker == "") {
            refuse(i, "section `" heading "` carries no `<!-- agent-tripwire: ... -->` marker. Every `##` section of a rulebook file must say whether AGENT.md tripwires it, because an opt-in marker is silent about a section that never opted in (#492). Name a phrase from the AGENT.md bullet, or say `<!-- agent-tripwire: none: <reason> -->`. It must sit within " MarkerWindow " lines below the heading.")
            continue
        }
        markers++
        sub(/^.*<!--[ ]*agent-tripwire:[ ]*/, "", marker)
        sub(/[ ]*-->.*$/, "", marker)
        marker = normalise(marker)

        if (marker == "") {
            refuse(markerLine, "section `" heading "` has an EMPTY `agent-tripwire` marker. A blank marker is not an answer -- it reads as a decision from a distance and is a forgot. Name a phrase, or say `none: <reason>`.")
            continue
        }

        # `none:` and a reason. The reason is required and is the forcing
        # function: a bare `none` would spell *deliberately untripwired* exactly
        # like *forgot*, which is the one distinction this marker exists to make.
        if (marker == "none" || marker ~ /^none:/) {
            reason = marker
            sub(/^none:?[ ]*/, "", reason)
            if (reason == "") {
                refuse(markerLine, "section `" heading "` says `none` with no reason. `none` is a CLAIM that this section needs no AGENT.md tripwire, and a claim with no reason beside it cannot be told from a box ticked. Write `<!-- agent-tripwire: none: <why> -->`.")
                continue
            }
            noneMarkers++
            continue
        }

        # `untriaged:` and an ISSUE. Safe only because the caller tallies these
        # and prints the total per issue on every run -- a placeholder nobody
        # counts is a permanent to-do wearing the word *decided*.
        if (marker == "untriaged" || marker ~ /^untriaged:/) {
            reason = marker
            sub(/^untriaged:?[ ]*/, "", reason)
            if (reason !~ /#[0-9]+/) {
                refuse(markerLine, "section `" heading "` says `untriaged` without naming an issue. `untriaged` is a backlog row, and a backlog row with no owner is a permanently false to-do. Write `<!-- agent-tripwire: untriaged: #<issue> <what is undecided> -->`.")
                continue
            }
            issue = reason
            sub(/^.*#/, "", issue)
            sub(/[^0-9].*$/, "", issue)
            if (!inOpenWork(issue)) {
                refuse(markerLine, "section `" heading "` is `untriaged` against #" issue ", but this file `## Open work` section does not name that issue. Deferred work in this rulebook lives as an `## Open work` entry, which `ctest -R rulebook-open-work-state` resolves and refuses once the issue closes. Without the entry these markers outlive the issue and go on printing a live-looking tally of a dead one -- which is the tally this spelling is only safe because of. Add the entry, or decide the section with a phrase or `none:`.")
                continue
            }
            printf "UNTRIAGED #%s\n", issue
            continue
        }

        # A nominated phrase. Long enough to mean something, then present.
        words = split(marker, w, " ")
        if (words < MinPhraseWords) {
            refuse(markerLine, "section `" heading "` nominates the phrase `" marker "`, which is " words " word(s). A phrase under " MinPhraseWords " words can match AGENT.md by accident, and a marker that cannot be wrong is a box ticked. Quote more of the bullet.")
            continue
        }
        if (index(hay, marker) == 0) {
            refuse(markerLine, "section `" heading "` nominates the phrase `" marker "`, which does NOT appear in AGENT.md. Either the tripwire was reworded or deleted -- in which case restore it, because a rule with no tripwire fires in no session that does not open this file -- or the marker is stale and should quote the bullet as it now reads. (AGENT.md is searched WHOLE and line-wrapping and `**`/backticks are ignored, so this is not a wrapping or an emphasis miss.)")
            continue
        }
        checked++
    }
    printf "SUMMARY %d %d %d %d\n", headings, markers, noneMarkers, checked
}
'

# What this check is blind to, printed with every refusal. A reader who meets a
# refusal is the reader most likely to over-apply the rule, and a guard stated
# only in its own header reaches nobody who did not open it.
NotCovered='  This check verifies the phrase the author nominated is PRESENT in AGENT.md.
  It does NOT decide whether that phrase is the right tripwire for the section,
  it does NOT refuse an AGENT.md bullet with no rules-file section behind it
  (AGENT.md also tripwires rules that live in source comments and .agent/guides/),
  and it says nothing about a rule that is in neither file.
  Its UNIT is the `##` heading, and a rule arrives as a BULLET under existing
  prose -- so a new rule in an already-marked section is invisible to it, and the
  files with the fewest headings are the ones it watches least. Read the counts
  above as sections covered, never as rules covered. That gap is measured and
  tracked as #1572.'

# The body. Wrapped by `RunCheck` below so the haystack temp file has exactly one
# removal site. Not a `trap ... RETURN`: a RETURN trap set inside a function also
# fires when its CALLER returns, so it would need clearing inside its own handler
# -- and this runs on a 2007 `/bin/bash`, where that behaviour is untested here.
# (An earlier version of this comment blamed `set -T`, which governs a different
# thing: whether a trap set in a caller is INHERITED. The decision is unchanged;
# the reason was wrong, and a confidently wrong reason is worse than a narrow
# one.)
RunCheckImpl() {
    local root="$1" hay="$2" f out line files fileCount=0
    local headings=0 markers=0 none=0 checked=0 failures=0 untriaged=0 issues=""

    files="$(find "$root/.agent/rules" -maxdepth 1 -name '*.md' -type f 2>/dev/null | sort)"
    if [[ -z "$files" ]]; then
        echo "check-rulebook-tripwires: no markdown under $root/.agent/rules -- refusing rather than reporting clean. A census that finds nothing has not passed, it has failed to look." >&2
        return 1
    fi

    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        fileCount=$((fileCount + 1))
        # The status is CHECKED. An awk that refused to parse writes nothing, and
        # the `headings == 0` guard below would then name the wrong cause -- a
        # reader would go hunting for a missing heading.
        local awkrc=0
        out="$(awk -v HayFile="$hay" -v MinPhraseWords="$MinPhraseWords" \
                   -v MarkerWindow="$MarkerWindow" "$CheckAwk" "$f" 2>&1)" || awkrc=$?
        if [[ "$awkrc" -ne 0 ]]; then
            echo "check-rulebook-tripwires: awk failed on $f (status $awkrc): $out" >&2
            return 1
        fi
        while IFS= read -r line; do
            case "$line" in
                SUMMARY\ *)
                    set -- $line
                    headings=$((headings + $2)); markers=$((markers + $3))
                    none=$((none + $4)); checked=$((checked + $5))
                    ;;
                # The untriaged TALLY is derived from these, not carried
                # alongside them: a total stated beside a list is derived from
                # it or it is a second claim, and the two could disagree.
                UNTRIAGED\ *)
                    issues="$issues ${line#UNTRIAGED }"
                    untriaged=$((untriaged + 1))
                    ;;
                # No `FATAL` arm: the awk program `exit 1`s after printing it,
                # so the `awkrc` guard above has already returned -- and it
                # echoes the text. An arm here would be unreachable.
                "") ;;
                *) echo "  $line" >&2; failures=$((failures + 1)) ;;
            esac
        done <<< "$out"
    done <<< "$files"

    # Zero is not a verdict, it is the absence of one. Each of these reads
    # exactly like a clean tree, and each means the check failed to LOOK.
    if [[ "$headings" -eq 0 ]]; then
        echo "check-rulebook-tripwires: found no \`## \` heading under $root/.agent/rules. A census that finds nothing has not passed, it has failed to look." >&2
        return 1
    fi
    if [[ "$markers" -eq 0 ]]; then
        echo "check-rulebook-tripwires: found $headings section(s) and not one marker. The marker syntax has stopped matching, which reads exactly like a tree with nothing to check." >&2
        return 1
    fi

    # `fileCount` from the loop, not a second `wc -l` over `$files`: the loop
    # skips blank entries that a `wc` would count, so the two mechanisms could
    # disagree about the same list.
    echo "rulebook tripwires: $headings section(s) across $fileCount rulebook file(s), $markers marked ($none stating none, $untriaged untriaged), $checked phrase(s) found in AGENT.md"

    # The untriaged tally, per issue, on EVERY run. This is the whole reason a
    # third spelling is safe: a placeholder nobody counts is a permanent to-do
    # wearing the word *decided*, and a tally that simply stops appearing reads
    # the same as one that stopped being computed -- so the `none` branch prints
    # too.
    if [[ "$untriaged" -eq 0 ]]; then
        echo "  untriaged: none"
    else
        printf '%s\n' $issues | sort | uniq -c | while read -r n issue; do
            echo "  untriaged: $n section(s) awaiting $issue"
        done
    fi

    if [[ "$failures" -ne 0 ]]; then
        printf '%s\n' "$NotCovered" >&2
        return 1
    fi
    return 0
}

RunCheck() {
    local root="$1" hay rc=0

    if [[ ! -f "$root/AGENT.md" ]]; then
        echo "check-rulebook-tripwires: no AGENT.md at $root -- refusing rather than reporting clean. The haystack being absent and every phrase being present are not the same green." >&2
        return 1
    fi

    # Flatten and strip ONCE here rather than per file: the design spawns one awk
    # per rules file, so an in-awk flatten would run eleven times.
    #
    # (This comment used to quote AGENT.md at "2400 lines / 200 KB" and blame BWK
    # awk's string concatenation being quadratic. The figures were falsified by
    # the very next commit on the branch that wrote them -- in a change that also
    # adds a check refusing AGENT.md to quote its own size BECAUSE such figures
    # go stale -- and the quadratic clause was an unmeasured amplifier reading as
    # the reason. The per-file repetition is the narrow true reason, so it is the
    # one kept.)
    hay="$(mktemp)" || { echo "check-rulebook-tripwires: mktemp failed" >&2; return 2; }
    sed -e 's/[*`_]//g' "$root/AGENT.md" | tr '\n\t' '  ' | tr -s ' ' > "$hay"
    # Whitespace only is the same failure as empty, and `-s` cannot tell them
    # apart: a one-line file of blanks is "not empty". `grep -q` short-circuits
    # at the first non-blank byte, and nothing pipes INTO it, so `pipefail` has
    # nothing to misreport.
    if ! grep -q '[^ ]' "$hay"; then
        rm -f "$hay"
        echo "check-rulebook-tripwires: AGENT.md at $root normalised to nothing. The check cannot look, which is not the same as finding nothing wrong." >&2
        return 1
    fi

    RunCheckImpl "$root" "$hay" || rc=$?
    rm -f "$hay"
    return "$rc"
}

# ---------------------------------------------------------------------------
# The self-test. Synthetic trees, one per verdict, driven in BOTH directions --
# a guard nobody has watched ACCEPT is not known to work either, and a suite
# whose only case is a refusal passes under a check that refuses everything.
#
# It prints how many cases ran. Without that, a run that died half way through
# is indistinguishable from one where everything passed, and "no failures
# printed" reads as "the guard did not fire".
SelfTest() {
    local scratch ran=0 failures=0
    scratch="$(mktemp -d)" || { echo "check-rulebook-tripwires: mktemp -d failed" >&2; exit 2; }
    trap 'rm -rf "$scratch"' EXIT

    # @param 1 case name
    # @param 2 expected outcome: `clean` or `refused`
    # @param 3 body of AGENT.md, or the sentinel `@@no-agent@@` to write none
    # @param 4 body of .agent/rules/t.md
    # @param 5.. text the output must contain; a leading `!` means must NOT
    Case() {
        local name="$1" want="$2" agent="$3" rules="$4"; shift 4
        local dir out rc=0 pattern got
        ran=$((ran + 1))
        dir="$scratch/case-$ran"
        mkdir -p "$dir/.agent/rules"
        [[ "$agent" == "@@no-agent@@" ]] || printf '%s\n' "$agent" > "$dir/AGENT.md"
        printf '%s\n' "$rules" > "$dir/.agent/rules/t.md"
        out="$(RunCheck "$dir" 2>&1)" || rc=$?
        got="clean"; [[ "$rc" -eq 0 ]] || got="refused"
        if [[ "$got" != "$want" ]]; then
            echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$((failures + 1))
            return
        fi
        # `${1+"$@"}`: before bash 4.4 an empty `$@` is an unbound expansion
        # under `set -u`, and this runs on macOS 3.2.
        for pattern in ${1+"$@"}; do
            if [[ "${pattern:0:1}" == "!" ]]; then
                if [[ "$out" == *"${pattern:1}"* ]]; then
                    echo "  FAIL ${name}: output contains '${pattern:1}' and must not" >&2
                    printf '%s\n' "$out" | sed 's/^/       | /' >&2
                    failures=$((failures + 1)); return
                fi
            elif [[ "$out" != *"$pattern"* ]]; then
                echo "  FAIL ${name}: output lacks '${pattern}'" >&2
                printf '%s\n' "$out" | sed 's/^/       | /' >&2
                failures=$((failures + 1)); return
            fi
        done
        echo "  ok   ${name}"
    }

    local _agent='# AGENT.md

## The rulebook

**[`.agent/rules/t.md`](.agent/rules/t.md)** — a made-up file.
- A socket has ONE read operation and `Read` and `WaitReadable` share it, so
  arming either while the other is parked drops the parked coroutine.
- **A wall clock is not a duration.**'

    # The ACCEPT direction, first and deliberately: a guard nobody has watched
    # accept is not known to work.
    Case "a section naming a phrase that is in AGENT.md is clean" clean \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: A socket has ONE read operation -->

Prose.' \
        "1 phrase(s) found" "1 section(s)"

    # The #492 hole this design closes: a section that never opted in. A MARKED
    # section sits beside it deliberately -- with only the unmarked one the tree
    # trips the `not one marker` guard instead, which is a different diagnosis
    # and does not carry the blind-spot footer. That footer rides on every
    # per-section refusal, so it is asserted here.
    Case "a section with no marker at all is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: A socket has ONE read operation -->

Prose.

## Framing

Prose.' \
        "carries no" "agent-tripwire" "does NOT decide whether that phrase is the right tripwire"

    # ... and the counterpart that makes a mandatory marker bearable.
    Case "a section stating none with a reason is clean" clean \
"$_agent" \
'# t

## Open work

<!-- agent-tripwire: none: deferred work, not a rule; the issues carry it -->

Prose.' \
        "1 stating none" "0 phrase(s) found"

    # `none` with no reason spells *decided* in the vocabulary of *forgot*.
    Case "a bare none with no reason is refused" refused \
"$_agent" \
'# t

## Open work

<!-- agent-tripwire: none -->

Prose.' \
        "with no reason"

    # The THIRD spelling, and the tally that is the only reason it is safe. A
    # placeholder nobody counts is a permanent to-do wearing the word *decided*,
    # so the per-issue total is asserted here and not merely the acceptance.
    Case "an untriaged section naming an issue is clean, and is tallied" clean \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: untriaged: #876 nobody has decided whether this needs one -->

Prose.

## Open work

<!-- agent-tripwire: none: deferred work, tracked as GitHub issues -->

- **[#876](https://github.com/LASTRADA-Software/fastcached/issues/876)** -- what is left.' \
        "1 untriaged" "untriaged: 1 section(s) awaiting #876"

    # ... and the pairing is REQUIRED, or the marker outlives the issue that was
    # meant to retire it.
    Case "an untriaged issue with no Open work entry is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: untriaged: #876 nobody has decided whether this needs one -->

Prose.' \
        "does not name that issue"

    # A neighbouring issue number must not satisfy it -- `#8761` is not `#876`.
    Case "a different issue in Open work does not satisfy the pairing" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: untriaged: #876 nobody has decided whether this needs one -->

Prose.

## Open work

<!-- agent-tripwire: none: deferred work, tracked as GitHub issues -->

- **[#8761](https://github.com/LASTRADA-Software/fastcached/issues/8761)** -- other.' \
        "does not name that issue" "!carries no"

    # ... and it must name an OWNER. Without the issue it is a backlog row
    # nobody will ever close.
    Case "an untriaged section naming no issue is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: untriaged: somebody should look at this -->

Prose.' \
        "without naming an issue"

    # The tally must print on the ZERO branch too: a total that simply stops
    # appearing reads exactly like one that stopped being computed.
    Case "a tree with nothing untriaged still prints the tally" clean \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: A socket has ONE read operation -->

Prose.' \
        "untriaged: none"

    # The failure the ticket is about: the rule is in the rules file and the
    # tripwire is gone from AGENT.md.
    Case "a phrase that is not in AGENT.md is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: a reclaim is reported before the call that caused it -->

Prose.' \
        "does NOT appear in AGENT.md"

    # A blank marker must not read as an answer.
    Case "an empty marker is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: -->

Prose.' \
        "EMPTY"

    # The phrase WRAPPED ACROSS A LINE in AGENT.md. Reading a present phrase as
    # absent is the defect `check-table-totals` carries a case for, and CMake
    # wrapped diagnostics are the same finding one rules file over.
    Case "a phrase wrapped across a line in AGENT.md is still found" clean \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: share it, so arming either while the other is parked -->

Prose.' \
        "1 phrase(s) found"

    # ... and one whose emphasis differs. Somebody bolding half a sentence in
    # AGENT.md must not break a correct marker.
    Case "a phrase differing only in markdown emphasis is found" clean \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: A **socket** has ONE `read` operation -->

Prose.' \
        "1 phrase(s) found"

    # A floor, or the marker degrades into the box-tick this design avoids.
    Case "a phrase too short to be distinctive is refused" refused \
"$_agent" \
'# t

## Sockets

<!-- agent-tripwire: a socket has -->

Prose.' \
        "word(s)" "box ticked"

    # A marker twenty lines down is under a paragraph, not under the heading.
    Case "a marker outside the window below its heading is refused" refused \
"$_agent" \
'# t

## Sockets

Prose.

More prose.

Yet more prose.

<!-- agent-tripwire: A socket has ONE read operation -->' \
        "carries no"

    # A `## ` inside a fenced block is not a heading, and demanding a marker
    # inside one is a refusal a reader cannot act on.
    Case "a heading inside a fenced code block is not a section" refused \
"$_agent" \
'# t

```
## Sockets
```' \
        "failed to look"

    # Both ways this check can fail to LOOK, each watched refusing, because both
    # read exactly like a clean tree.
    Case "a tree with no AGENT.md is refused, not reported clean" refused \
"@@no-agent@@" \
'# t

## Sockets

<!-- agent-tripwire: A socket has ONE read operation -->' \
        "no AGENT.md at"

    Case "an AGENT.md that is only whitespace is refused" refused \
"   " \
'# t

## Sockets

<!-- agent-tripwire: A socket has ONE read operation -->' \
        "normalised to nothing"

    Case "a tree where no section is marked is refused" refused \
"$_agent" \
'# t

## Sockets

Prose.

## Framing

Prose.' \
        "not one marker"

    echo "check-rulebook-tripwires --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

case "${1:-}" in
    --self-test) SelfTest ;;
    -h|--help)   echo "usage: $0 [<repo-root>] | --self-test"; exit 0 ;;
esac

Root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
RunCheck "$Root"
