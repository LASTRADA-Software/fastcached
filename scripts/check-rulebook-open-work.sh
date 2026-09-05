#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every `## Open work` entry in `.agent/rules/` still names OPEN work.
#
# ---------------------------------------------------------------------------
# What goes wrong when nothing checks this
# ---------------------------------------------------------------------------
#
# A stale entry that says "not done yet" costs a reader a minute. The shape that
# costs more is the one #395 found: an entry asserting the field's borrow "cannot
# be tested", with a plausible short-string-optimisation argument attached, and
# `AGENT.md` routing every session to that file before they touch `Net/` or
# `Protocol/`. The rulebook was instructing the next person NOT TO ATTEMPT the
# guard, and the argument read as current rather than as obviously stale. An ASan
# trace disproved it directly: allocated, freed, read, three lines apart.
#
# `.agent/rules/` exists because every rule in it has already been a bug. An entry
# claiming something is impossible is a rule that has gone false, and nothing here
# could see it.
#
# ---------------------------------------------------------------------------
# What is an ENTRY, and why citations are deliberately out of scope
# ---------------------------------------------------------------------------
#
# An entry is the LEADING issue reference of a top-level bullet inside a section
# opened by `## Open work`. Nothing else in that section is checked, and that
# exclusion is load-bearing rather than a simplification.
#
# #619's own body measured "four stale of 26 entries" and named `#195` twice. Both
# of those are CITATIONS inside another bullet's prose -- "#200 ... since #195 that
# banner is the compiler's identity", "#201 ... #195 gave the banner the target it
# names" -- and both are correct exactly as written, because #195 is the landed
# change that CREATED the residual the bullet is about. A third, `#663` inside
# #710's bullet, the ticket did not notice.
#
# So a check built to that description invents three findings on a correct
# rulebook, somebody then edits a correct entry to satisfy it, and the check is
# disabled for noise. The ticket states that failure mode for rule PROSE and it
# applies one level down, inside an Open work bullet: **a citation carries no claim
# that the thing it names is open.** Only the bullet's subject does.
#
# That reading is safe only because of the refusal below it. A top-level bullet in
# such a section that yields NO leading reference is `unparsed-entry`, a failure
# naming the line -- so a reformatted bullet cannot quietly drop out of the scanned
# set. Narrowing the pattern without that clause would be #492 again: exact about
# what it knows, silent about what it does not.
#
# The census, stated with its pattern because a figure without one gets quoted at
# the wrong set (two audits of `scripts/check-*.cmake` differed by exactly one for
# precisely this reason). Pattern: a line matching
# `^-[[:space:]]+(\*\*)?\[#N\](https://github.com/OWNER/REPO/issues/N)` inside a
# section opened by `^## Open work$` and closed by the next `^## `. On master
# `0db96dc8` that was **39 entries in 9 sections across 11 rule files**, of which
# seven named closed issues.
#
# ---------------------------------------------------------------------------
# FOUR outcomes, not two
# ---------------------------------------------------------------------------
#
# Measured against the real API on 2026-09-05:
#
#     a real issue          exit=0  state + kind on stdout
#     PR #812               exit=0  state=open, and it is a PULL REQUEST
#     issue 999999          exit=1  stderr `gh: Not Found (HTTP 404)`
#     a repo that is not    exit=1  stderr `gh: Not Found (HTTP 404)`
#     a bad token           exit=1  stderr `gh: Bad credentials (HTTP 401)`
#     an unreachable host   exit=1  stderr `error connecting to ...`, NO status
#
# Three of those share one exit status, and two of them are faults in THIS
# script's own invocation. Reading `exit != 0` as "this entry names a dead issue"
# makes the check blame its subject for its own fault -- it invents a finding, and
# the repair is somebody editing a correct rulebook entry. So the verdict is drawn
# from the HTTP status, which separates them:
#
#     no status at all       -> could-not-run (transport)
#     404, repo resolved     -> bad-reference (the entry names nothing)
#     404, repo did not      -> bad-reference against the SLUG, once, not per entry
#     anything else          -> could-not-run (auth, rate limit, 5xx)
#
# And `gh api rate_limit` is asked first as a liveness anchor. It needs no
# repository and answers authenticated or not, so it tells a broken checker apart
# from a broken rulebook BEFORE either could be blamed. A 404 on `repos/<slug>`
# after that anchor answered is a real finding about the slug -- not forty
# findings about forty entries, which is what a missing anchor would have produced
# from a single typo.
#
# ---------------------------------------------------------------------------
# Resolving to the wrong KIND of object is a way of not having resolved it
# ---------------------------------------------------------------------------
#
# `gh issue view <n>` falls back to pull requests, and so does this REST route:
# `repos/{owner}/{repo}/issues/N` answers for a PR too, with a `pull_request` key.
# The failure is asymmetric and the asymmetry is why a `state == "open"` test is
# not enough:
#
#   * a MERGED pull request answers `closed`, fails such a test, and LOOKS LIKE
#     THE CHECK WORKING -- right answer, wrong reason, and the day it matters it
#     is an open PR;
#   * an OPEN pull request answers `open` and PASSES, while the entry names no
#     open issue at all.
#
# So the kind is asserted, not just the state, and a pull request is refused by
# name whichever state it is in.
#
# ---------------------------------------------------------------------------
# The network decision, made explicitly (it is not left to a silent skip)
# ---------------------------------------------------------------------------
#
# Live resolution, split across two registrations so that neither can report a
# verdict it did not reach:
#
#   * `--extract` is `rulebook-open-work`, in the DEFAULT ctest set. It opens no
#     socket. It is the whole grammar: the sections, the bullets, the link/URL
#     agreement, the heading-rename refusal and the non-empty assertions.
#   * `--resolve` is `rulebook-open-work-state`, labelled `smoke`. It is the only
#     part that needs `gh`, and the only part that can skip.
#
# Folding the first into the second would make a machine without `gh` skip the
# assertions that need nothing -- a skipped required thing reading as a passed
# one, which is the shape the ticket bans. A cached list refreshed by CI was the
# alternative and is worse for this subject: the cache would go stale by exactly
# the mechanism the check exists to catch, and nothing offline could tell a
# current cache from an abandoned one.
#
# What may skip is precisely bounded. A missing PREREQUISITE -- no `gh`, no
# credentials, no network -- is detected BEFORE any entry is resolved and exits
# 77, so ctest reports SKIPPED with the reason. A fault that appears MID-RUN, once
# some entries have resolved, is a FAILURE: a partial answer over a set is not an
# answer about the set, and calling it a skip would be the four-states collapse
# this repository keeps paying for.
#
# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------
#
#   check-rulebook-open-work.sh --extract      grammar only; no network
#   check-rulebook-open-work.sh --resolve      grammar, then resolve each entry
#   check-rulebook-open-work.sh --self-test    every verdict, against a stub `gh`
#
#   --rules-dir DIR   scan DIR instead of `.agent/rules` (the self-test's trees)
#
# bash 3.2: macOS ships a 2007 `/bin/bash` and both registrations are in the
# default set on every platform CI builds. No `mapfile`, no `declare -A`, no
# `local -n`, no case modification -- and no bare `"${arr[@]}"`, which is an
# unbound variable on an empty array before 4.4. Records go through FILES rather
# than arrays here, which sidesteps that last one by construction.

set -uo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rules_dir="${source_dir}/.agent/rules"
mode=""

while [ $# -gt 0 ]; do
    case "$1" in
        --extract)   mode="extract";   shift ;;
        --resolve)   mode="resolve";   shift ;;
        --self-test) mode="self-test"; shift ;;
        --rules-dir)
            [ $# -ge 2 ] || { echo "--rules-dir needs a directory" >&2; exit 2; }
            rules_dir="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$mode" ]; then
    echo "check-rulebook-open-work.sh: one of --extract, --resolve, --self-test is required" >&2
    exit 2
fi

SKIP=77

failures=0
failed_cases=""

# The ONE place the counter moves, so a failure cannot be counted without being
# named. `check-e2e-helpers.sh` records why: nineteen sites incremented by hand
# before #678, and a twentieth written the old way would report `1 failed` with no
# name at all.
#
# @param 1 the verdict name
note_failure() {
    failures=$(( failures + 1 ))
    case " ${failed_cases} " in
        *" $1 "*) ;;
        *) failed_cases="${failed_cases}${failed_cases:+ }$1" ;;
    esac
}

# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

# Emit one record per interesting line of one rule file.
#
#   SECTION <line>              a `## Open work` heading
#   RENAMED <line> <text>       a heading that mentions open work and is NOT that
#   BULLET  <line> <text>       a top-level bullet inside such a section
#
# The section closes at the next LEVEL-2 heading, so a `###` inside it stays
# inside it. A heading that says "open work" in any other spelling is refused
# rather than ignored: a renamed heading is how one file's entries stop being
# scanned while the global count stays healthy, and silence there reads exactly
# like coverage.
#
# @param 1 the file
_file_records() {
    awk '
        # A fenced block is SAMPLE text, and both directions of getting this
        # wrong were measured. A `## Open work` inside a fence opened a real
        # section and its sample bullet became a real entry, so the check
        # reported `stale` about a documentation example and told the reader to
        # delete it -- and `.agent/rules/README.md` carries exactly such a
        # sample, one heading away from being live. A fenced repro block inside a
        # REAL section went the other way and was refused as `unparsed-entry`.
        /^ ? ? ?(```|~~~)/ { fence = !fence; next }
        fence { next }

        # An ATX heading, which is `#`+ followed by a SPACE -- not merely a line
        # starting with `#`. Four lines in this rulebook begin with `#` because
        # an issue reference landed at a wrap point (`#340; the first was still
        # ...`), and under the looser test one of those containing the words
        # would be refused as a renamed heading: a refusal naming a line that is
        # not a heading, whose remedy means nothing.
        /^#+[ \t]/ {
            if ($0 ~ /^## Open work[ \t]*$/) { inw = 1; print "SECTION " NR; next }
            if (tolower($0) ~ /open[ \t]+work/) { print "RENAMED " NR " " $0 }
            # Only a LEVEL-2 heading closes the section, so a `###` inside it
            # stays inside it. One rule rather than two, because the
            # renamed-heading vocabulary is the load-bearing part and two copies
            # of it is two things to widen.
            if ($0 ~ /^## /) { inw = 0 }
            next
        }

        # Every top-level list marker, at CommonMark`s top-level indent of 0-3
        # spaces. `-` alone was the whole rule, and a `*` or `+` bullet -- or a
        # one-space-indented `-`, which is still a sibling and not a child --
        # was not a BULLET at all, so it never reached the `unparsed-entry`
        # refusal either. It simply vanished, and a closed issue under one
        # passed. That is the exact failure the refusal exists to prevent, one
        # level down in the pattern that feeds it. Four or more spaces is a
        # nested item and stays out.
        #
        # Written ` ? ? ?` rather than ` {0,3}`: interval expressions are not
        # portable across every awk this has to run on.
        inw && /^ ? ? ?[-*+][ \t]/ { print "BULLET " NR " " $0 }
    ' "$1"
}

# A bullet is an entry when it OPENS with an issue link. `[[ =~ ]]` with the
# pattern held in a variable is the bash 3.2 spelling -- quoting it inline makes
# 3.2 treat it as a literal string.
#
# Both numbers are captured. `[#64](.../issues/46)` reads correctly to a human and
# resolves to something else entirely, so the link text and the URL are required
# to agree; that is a transposition nobody proof-reads for.
entry_re='^[[:space:]]*[-*+][[:space:]]+(\*\*)?\[#([0-9]+)\]\(https://github\.com/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/issues/([0-9]+)\)'

# The HTTP status `gh` puts in its error line, or "" when there is none. ONE
# definition, because this is the single discriminator between a finding about
# the rulebook and a fault in this script's own invocation -- and a rule stated
# twice is a rule that can be half-changed.
#
# @param 1 the file gh's stderr was captured into
_http_status() {
    sed -n 's/.*(HTTP \([0-9][0-9]*\)).*/\1/p' "$1" | head -1
}

scratch="$(mktemp -d)" || { echo "could not create a scratch directory" >&2; exit 2; }
[ -n "$scratch" ] && [ -d "$scratch" ] || { echo "mktemp -d gave no directory" >&2; exit 2; }
trap 'rm -rf "$scratch"' EXIT

entries="${scratch}/entries"
: > "$entries"

files_scanned=0
sections=0
entry_count=0

# A heading with nothing under it is the other way a section leaves the scanned
# set, and it looks like tidiness rather than a defect. Two empty lists agree
# perfectly: an emptied section reports clean forever.
#
# Per SECTION and not per FILE. Counting only per file let a file with one
# populated section and one emptied one pass, which is most of the way to the
# thing being guarded against.
#
# @param 1 the file's base name
_refuse_empty_section() {
    [ "$section_line" -ne 0 ] || return 0
    [ "$section_entries" -eq 0 ] || return 0
    echo "FAIL empty-section: ${1}:${section_line} opens '## Open work' and lists no entry under it." >&2
    echo "     Delete the heading when the last entry goes, or the section silently leaves" >&2
    echo "     the scanned set and every future entry in it is unchecked." >&2
    note_failure "empty-section"
}

collect() {
    local file="" base="" kind="" line="" text=""
    local file_sections=0 section_line=0 section_entries=0

    if [ ! -d "$rules_dir" ]; then
        echo "FAIL rules-dir: ${rules_dir} is not a directory" >&2
        note_failure "rules-dir"
        return
    fi

    for file in "${rules_dir}"/*.md; do
        # A glob that matches nothing expands to itself. Counting that as a file
        # scanned is how "no rule files" becomes "every rule file clean".
        [ -f "$file" ] || continue
        base="${file##*/}"
        files_scanned=$(( files_scanned + 1 ))
        file_sections=0
        section_line=0
        section_entries=0

        # Default-IFS `read`, which splits on the space these records use and
        # puts everything left over in the LAST variable. Deliberately not
        # `IFS=$'\t' read`: tab is IFS whitespace, so an empty field collapses
        # and shifts every field after it.
        while read -r kind line text; do
            case "$kind" in
                SECTION)
                    _refuse_empty_section "$base"
                    sections=$(( sections + 1 ))
                    file_sections=$(( file_sections + 1 ))
                    section_line="$line"
                    section_entries=0
                    ;;
                RENAMED)
                    echo "FAIL heading: ${base}:${line} spells the section heading as" >&2
                    echo "     |${text}" >&2
                    echo "     The scan opens on '## Open work' exactly. A renamed heading takes this" >&2
                    echo "     file's entries out of the scanned set while the total stays healthy." >&2
                    note_failure "heading"
                    ;;
                BULLET)
                    if [[ $text =~ $entry_re ]]; then
                        if [ "${BASH_REMATCH[2]}" != "${BASH_REMATCH[5]}" ]; then
                            echo "FAIL link-text: ${base}:${line} reads '#${BASH_REMATCH[2]}' and links to issue ${BASH_REMATCH[5]}" >&2
                            echo "     A reader follows the text and the check follows the URL, so they must agree." >&2
                            note_failure "link-text"
                            continue
                        fi
                        entry_count=$(( entry_count + 1 ))
                        section_entries=$(( section_entries + 1 ))
                        printf '%s %s %s %s/%s\n' \
                            "${BASH_REMATCH[5]}" "$line" "$base" \
                            "${BASH_REMATCH[3]}" "${BASH_REMATCH[4]}" >> "$entries"
                    else
                        echo "FAIL unparsed-entry: ${base}:${line} is a bullet in an Open work section" >&2
                        echo "     that does not open with an issue link, so nothing here checks it:" >&2
                        echo "     |${text}" >&2
                        echo "     Write it as '- **[#N](https://github.com/OWNER/REPO/issues/N)** — ...'," >&2
                        echo "     or move the prose under the entry it belongs to." >&2
                        note_failure "unparsed-entry"
                    fi
                    ;;
            esac
        done < <( _file_records "$file" )

        # And the last section in the file, which has no following heading to
        # close it.
        _refuse_empty_section "$base"
    done
}

# The self-test never reads the real rulebook: it drives this same script against
# synthesised trees, and a finding about `.agent/rules/` reported from inside it
# would be a verdict about a subject the run is not about.
if [ "$mode" != "self-test" ]; then
    collect

    # The non-empty assertions, both of them, before any verdict is read. A scan
    # that found no files and a scan that found no entries both report every entry
    # sound.
    if [ "$files_scanned" -lt 1 ]; then
        echo "FAIL census: the glob matched no rule files under ${rules_dir}," >&2
        echo "     so every entry in every one of them 'passed'." >&2
        note_failure "census"
    fi
    if [ "$entry_count" -lt 1 ]; then
        echo "FAIL census: read 0 Open work entries out of ${files_scanned} rule file(s)." >&2
        echo "     With no entries there is nothing to resolve and the check reports clean." >&2
        note_failure "census"
    fi

    echo "rulebook-open-work: ${entry_count} entr(ies) in ${sections} section(s) across ${files_scanned} rule file(s)"
    echo "  pattern: a top-level bullet opening with [#N](https://github.com/OWNER/REPO/issues/N),"
    echo "           inside a section opened by '## Open work' and closed by the next '## '"
fi

if [ "$mode" = "extract" ]; then
    if [ "$failures" -gt 0 ]; then
        echo "  failed: ${failed_cases}" >&2
        exit 1
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# Resolution
# ---------------------------------------------------------------------------

if [ "$mode" = "resolve" ]; then
    # A grammar failure means the set being resolved is not the set. Resolving
    # anyway would print a confident per-entry verdict over a set the previous
    # half just said it could not read.
    if [ "$failures" -gt 0 ]; then
        echo "  failed: ${failed_cases}" >&2
        echo "  (not resolving: the entry set could not be read, so no verdict about it would mean anything)" >&2
        exit 1
    fi

    if ! command -v gh >/dev/null 2>&1; then
        echo "rulebook-open-work-state SKIPPED: gh is not on PATH, so no entry was resolved." >&2
        echo "  This is a missing prerequisite, not a verdict: ${entry_count} entries were read and none were checked." >&2
        exit "$SKIP"
    fi

    # The liveness anchor. It needs no repository and answers authenticated or
    # not, so a failure here is unambiguously THIS CHECK's problem -- and it is
    # asked before anything could be blamed on the rulebook.
    anchor_err="${scratch}/anchor.err"
    if ! gh api rate_limit >/dev/null 2>"$anchor_err"; then
        echo "rulebook-open-work-state SKIPPED: gh cannot reach the API at all." >&2
        sed 's/^/  | /' "$anchor_err" >&2
        echo "  Missing prerequisite (no credentials, or no network). ${entry_count} entries were read and none were checked." >&2
        exit "$SKIP"
    fi

    # Every distinct slug the rulebook names, resolved ONCE. A typo in a slug is
    # a 404 on every entry that carries it; reported per entry that is forty
    # findings from one mistake, and each of them says the wrong thing.
    bad_slugs="${scratch}/bad-slugs"
    : > "$bad_slugs"
    slug_err="${scratch}/slug.err"
    while IFS= read -r slug; do
        if gh api "repos/${slug}" >/dev/null 2>"$slug_err" </dev/null; then
            continue
        fi
        status="$(_http_status "$slug_err")"
        if [ "$status" = "404" ]; then
            echo "$slug" >> "$bad_slugs"
            echo "FAIL bad-reference: no repository ${slug}; every entry linking there names nothing." >&2
            note_failure "bad-reference"
        else
            # Not `could-not-run` and not red. Nothing has resolved yet, so this
            # is the prerequisite case the contract at the top of this file
            # names -- and `gh api repos/<slug>` shares a rate limit and a
            # transport with every query after it, so treating it as a finding
            # is a new way for a healthy rulebook to go red.
            echo "rulebook-open-work-state SKIPPED: could not resolve the repository ${slug}." >&2
            sed 's/^/  | /' "$slug_err" >&2
            echo "  Missing prerequisite, not a verdict: ${entry_count} entries were read and none were checked." >&2
            exit "$SKIP"
        fi
    done < <( cut -d' ' -f4 "$entries" | LC_ALL=C sort -u )

    resolved=0
    while read -r number line base slug; do
        # An entry under a slug already refused above is not ASKED again -- the
        # answer is known. Each entry is still named, because a reader has to
        # see which entries a dead slug takes with it; what is skipped is the
        # round trip, not the line.
        if grep -qxF "$slug" "$bad_slugs" 2>/dev/null; then
            echo "FAIL bad-reference: ${base}:${line} names #${number} in ${slug}, which is not a repository." >&2
            note_failure "bad-reference"
            continue
        fi

        err="${scratch}/entry.err"
        out="$( gh api "repos/${slug}/issues/${number}" \
                   --jq '[(.state), (if .pull_request then "pr" else "issue" end)] | join(" ")' \
                   2>"$err" </dev/null )"
        rc=$?

        if [ "$rc" -ne 0 ]; then
            status="$(_http_status "$err")"
            if [ "$status" = "404" ]; then
                echo "FAIL bad-reference: ${base}:${line} names #${number}, which does not exist in ${slug}." >&2
                note_failure "bad-reference"
            else
                echo "FAIL could-not-run: ${base}:${line} names #${number} and it could not be resolved." >&2
                sed 's/^/     | /' "$err" >&2
                echo "     This is a fault in THIS CHECK's invocation, not a finding about the rulebook." >&2
                echo "     ${resolved} of ${entry_count} entries had resolved when it happened, so the run is" >&2
                echo "     a partial answer about the set rather than an answer, and partial is not a skip." >&2
                note_failure "could-not-run"
            fi
            continue
        fi

        state="${out%% *}"
        kind="${out##* }"

        # An answer this script cannot read is a hard failure, never a quiet
        # retry and never a default. The self-test drives the stub through the
        # SHAPE `--jq` produces rather than through gh's parsing of the JSON, so
        # this arm is what would catch the two diverging on the live run.
        case "${state}|${kind}" in
            open\|issue|closed\|issue|open\|pr|closed\|pr) ;;
            *)
                echo "FAIL could-not-run: ${base}:${line} names #${number} and the API answered" >&2
                echo "     '${out}', which this check cannot read. Not a finding about the rulebook." >&2
                note_failure "could-not-run"
                continue
                ;;
        esac

        resolved=$(( resolved + 1 ))

        if [ "$kind" = "pr" ]; then
            # Both halves are refused, and the merged one is why the state alone
            # is not enough: a merged PR answers `closed`, so a `state == open`
            # test rejects it and looks like the check working, while an OPEN one
            # sails through naming no issue at all.
            echo "FAIL bad-reference: ${base}:${line} names #${number}, which is a PULL REQUEST (${state}), not an issue." >&2
            echo "     A pull request number in an Open work entry resolves to the wrong kind of" >&2
            echo "     object, which is a way of not having resolved it: an open one would pass a" >&2
            echo "     state test while naming no open issue." >&2
            note_failure "bad-reference"
            continue
        fi

        if [ "$state" != "open" ]; then
            echo "FAIL stale: ${base}:${line} names #${number}, which is ${state}." >&2
            echo "     The work is done and the entry says it is not. Delete the entry, or -- if the" >&2
            echo "     residual it describes is still real -- open an issue for what is left and" >&2
            echo "     point the entry at that." >&2
            note_failure "stale"
        fi
    done < "$entries"

    echo "rulebook-open-work-state: resolved ${resolved} of ${entry_count} entr(ies)"
    if [ "$failures" -gt 0 ]; then
        echo "  failed: ${failed_cases}" >&2
        exit 1
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------
#
# A suite whose only negative case is a closed issue passes under every one of
# the three bugs this script is built around: the PR fallback, the three faults
# sharing one exit status, and the checker blaming its subject for its own fault.
# So there is a case per OUTCOME, and a control that must PASS -- an arm that has
# only ever been watched on a clean tree has told you nothing.
#
# The stub is a `gh` on PATH, the shape `check-merge-group-report.sh` and
# `tsan-gate-selftest` use. What it reproduces is the SHAPE `--jq` emits, not
# gh's parsing of the JSON; the live registration is what covers the expression
# itself, and the `could-not-run` arm above is what makes a divergence loud
# rather than silent.

cases_run=0

# The `must not` half is not decoration. Every arm below fails the run, so an exit
# status alone cannot tell `stale` from `could-not-run` -- and those two are the
# pair the whole design is about: one is a finding about the rulebook and the
# other is this script's own fault wearing the same exit code. An arm that asserts
# only the presence of its own verdict passes on a build that ALSO prints the
# wrong one.
#
# @param 1 case name
# @param 2 expected exit status
# @param 3 an ERE one output LINE must match ("" for no requirement)
# @param 4 an ERE no output line may match ("" for no requirement)
# @param 5 the rules directory to scan
# @param 6 the directory holding the stub `gh`, or "" to keep PATH as it is
# @param 7 the mode to drive, default `--resolve`
_case() {
    local name="$1" want_status="$2" want_re="$3" deny_re="$4" tree="$5" stub="$6"
    local mode_flag="${7:---resolve}"
    local out="" got=0 path="$PATH"

    cases_run=$(( cases_run + 1 ))
    [ -n "$stub" ] && path="${stub}:${PATH}"

    # `bash <path>`, never a bare path. A bare invocation of a mode-644 script
    # exits 126, and inside a want-fail assertion a shell that REFUSED TO START is
    # indistinguishable from the rule firing -- eight cases passed that way in
    # #723. Naming the interpreter removes the whole class.
    out="$( PATH="$path" bash "${BASH_SOURCE[0]}" "$mode_flag" --rules-dir "$tree" 2>&1 )"
    got=$?

    if [ "$got" != "$want_status" ]; then
        _case_fail "$name" "exit ${got}, expected ${want_status}" "$out"
        return
    fi
    # `<<<` and never `producer | grep -q`: `grep -q` exits at its first match,
    # the producer dies of SIGPIPE, and `pipefail` then reports the PRODUCER's
    # status -- a false answer on the SUCCESS path. A herestring has no producer
    # process to kill, so the shape is immune rather than merely small enough.
    if [ -n "$want_re" ] && ! grep -qE "$want_re" <<< "$out"; then
        _case_fail "$name" "no output line matches /${want_re}/" "$out"
        return
    fi
    if [ -n "$deny_re" ] && grep -qE "$deny_re" <<< "$out"; then
        _case_fail "$name" "an output line matches /${deny_re}/, which it must not" "$out"
        return
    fi
}

# Through `note_failure`, so this suite obeys the rule its own header states: the
# counter moves in one place and a failure cannot be counted without being named.
# The summary then says WHICH cases failed, which a bare count cannot.
#
# @param 1 case name
# @param 2 what went wrong
# @param 3 the captured output
_case_fail() {
    echo "FAIL selftest/${1}: ${2}" >&2
    printf '%s\n' "$3" | sed 's/^/     | /' >&2
    note_failure "${1}"
}

# A rulebook of one file with one entry per number given.
#
# @param 1 the directory to create
# @param 2.. issue numbers
_tree() {
    local dir="$1"; shift
    local n=""
    mkdir -p "$dir"
    {
        echo "# A synthetic rule file"
        echo
        echo "A rule, so the file is not only a heading."
        echo
        echo "## Open work"
        echo
        for n in ${1+"$@"}; do
            echo "- **[#${n}](https://github.com/acme/widget/issues/${n})** — a residual."
        done
    } > "${dir}/synthetic.md"
}

# A stub `gh`. The table is one `number:state:kind` row per line; a number with
# no row is a 404. `rate_limit` and `repos/<slug>` answer unless told otherwise.
#
# @param 1 the directory to create the stub in
# @param 2 the row table
# @param 3 how the stub misbehaves. The `case` arms in the body below are
#          authoritative; at the time of writing they are anchor-down, slug-404,
#          auth, transport and garbage, and "" behaves. That list is a reading
#          aid and nothing checks it -- a guard over it would have to read the
#          same file the call sites live in, so it would pass with a branch
#          deleted, and a guard that cannot fail is worse than none. An
#          unrecognised value simply makes the stub behave.
_stub() {
    local dir="$1" table="$2" fault="$3"
    mkdir -p "$dir"
    {
        echo '#!/usr/bin/env bash'
        echo 'arg="${2:-}"'
        printf 'fault=%s\n' "${fault:-none}"
        cat <<'STUB'
case "$arg" in
    rate_limit)
        if [ "$fault" = "anchor-down" ]; then
            echo "error connecting to api.github.com" >&2
            echo "check your internet connection or https://githubstatus.com" >&2
            exit 1
        fi
        echo '{"rate":{"remaining":5000}}'
        exit 0
        ;;
esac
number="${arg##*/}"
case "$arg" in
    */issues/*) ;;
    *)
        # The repository probe.
        if [ "$fault" = "slug-404" ]; then
            echo "gh: Not Found (HTTP 404)" >&2
            exit 1
        fi
        if [ "$fault" = "slug-auth" ]; then
            echo "gh: Bad credentials (HTTP 401)" >&2
            exit 1
        fi
        echo '{"full_name":"acme/widget"}'
        exit 0
        ;;
esac
# `slug-404` 404s the ISSUES route too. A repository that is not there does not
# answer for issues under it, and a stub that answered them made the per-entry
# short-circuit untestable: deleting it left the suite green.
if [ "$fault" = "slug-404" ]; then
    echo "gh: Not Found (HTTP 404)" >&2
    exit 1
fi
if [ "$fault" = "forbidden" ]; then
    echo "stub-gh: reached the network" >&2
    exit 3
fi
if [ "$fault" = "auth" ]; then
    echo "gh: Bad credentials (HTTP 401)" >&2
    exit 1
fi
if [ "$fault" = "transport" ]; then
    echo "error connecting to api.github.com" >&2
    exit 1
fi
if [ "$fault" = "garbage" ]; then
    echo "probably"
    exit 0
fi
STUB
        printf 'table=%s\n' "'$table'"
        cat <<'STUB'
row=""
for r in $table; do
    case "$r" in
        "${number}:"*) row="$r" ;;
    esac
done
if [ -z "$row" ]; then
    echo '{"message":"Not Found","status":"404"}'
    echo "gh: Not Found (HTTP 404)" >&2
    exit 1
fi
rest="${row#*:}"
echo "${rest%:*} ${rest##*:}"
exit 0
STUB
    } > "${dir}/gh"
    chmod +x "${dir}/gh"
}

st="${scratch}/selftest"
mkdir -p "$st"

# --- the control: it must PASS ---------------------------------------------
#
# As necessary as any failing arm. A check that refuses everything refuses a
# correct rulebook too, and only a case that must pass can see that.
_tree "${st}/clean" 11 12
_stub "${st}/bin-clean" "11:open:issue 12:open:issue" ""
_case "control-passes" 0 "resolved 2 of 2" "FAIL" "${st}/clean" "${st}/bin-clean"

# --- stale ------------------------------------------------------------------
_stub "${st}/bin-stale" "11:open:issue 12:closed:issue" ""
_case "stale" 1 "FAIL stale: .*#12, which is closed" "could-not-run|bad-reference" \
    "${st}/clean" "${st}/bin-stale"

# --- an OPEN pull request: the case a state test passes ----------------------
_stub "${st}/bin-open-pr" "11:open:issue 12:open:pr" ""
_case "open-pull-request" 1 "FAIL bad-reference: .*#12, which is a PULL REQUEST \(open\)" "FAIL stale" \
    "${st}/clean" "${st}/bin-open-pr"

# --- a MERGED pull request: right answer for the wrong reason ----------------
#
# It answers `closed`, so a `state == open` test rejects it and looks correct.
# This asserts the REASON, which is the half that would still be wrong.
_stub "${st}/bin-merged-pr" "11:open:issue 12:closed:pr" ""
_case "merged-pull-request" 1 "FAIL bad-reference: .*#12, which is a PULL REQUEST \(closed\)" "FAIL stale" \
    "${st}/clean" "${st}/bin-merged-pr"

# --- bad reference: the number resolves to nothing ---------------------------
_stub "${st}/bin-missing" "11:open:issue" ""
_case "bad-reference" 1 "FAIL bad-reference: .*#12, which does not exist" "FAIL stale|could-not-run" \
    "${st}/clean" "${st}/bin-missing"

# --- could-not-run, auth: the fault is the checker's, mid-run ----------------
#
# The anchor answers, so the prerequisite is present; the entry query then fails
# on credentials. That must not read as stale, and must not read as a skip.
_stub "${st}/bin-auth" "11:open:issue 12:open:issue" "auth"
_case "could-not-run-auth" 1 "FAIL could-not-run: .*could not be resolved" "FAIL stale|FAIL bad-reference|SKIPPED" \
    "${st}/clean" "${st}/bin-auth"

# --- could-not-run, transport: no HTTP status at all -------------------------
_stub "${st}/bin-transport" "11:open:issue 12:open:issue" "transport"
_case "could-not-run-transport" 1 "FAIL could-not-run: .*could not be resolved" "FAIL stale|FAIL bad-reference|SKIPPED" \
    "${st}/clean" "${st}/bin-transport"

# --- an answer the script cannot read ----------------------------------------
_stub "${st}/bin-garbage" "11:open:issue 12:open:issue" "garbage"
_case "could-not-run-unreadable" 1 "which this check cannot read" "FAIL stale|FAIL bad-reference" \
    "${st}/clean" "${st}/bin-garbage"

# --- the prerequisite is missing: SKIP, and before anything is blamed --------
_stub "${st}/bin-anchor-down" "11:open:issue 12:open:issue" "anchor-down"
_case "prerequisite-missing-skips" 77 "SKIPPED: gh cannot reach the API" "FAIL" \
    "${st}/clean" "${st}/bin-anchor-down"

# --- a slug that is not a repository: ONE finding, not one per entry ---------
_stub "${st}/bin-slug404" "11:open:issue 12:open:issue" "slug-404"
_case "bad-slug" 1 "FAIL bad-reference: no repository acme/widget" "FAIL stale|could-not-run" \
    "${st}/clean" "${st}/bin-slug404"
# ...and every entry under it is still NAMED. What the short-circuit skips is
# the round trip, not the line -- a reader has to see which entries a dead slug
# takes with it.
_case "bad-slug-names-its-entries" 1 "FAIL bad-reference: .*names #12 in acme/widget, which is not a repository" "" \
    "${st}/clean" "${st}/bin-slug404"

# --- markdown shapes that were invisible, and shapes that must stay so ------
#
# Every one of these was MEASURED wrong before it was fixed, not imagined.

# A `*` bullet was not a BULLET, so it did not become an entry and did not reach
# `unparsed-entry` either. A closed issue under one passed, green.
mkdir -p "${st}/star"
printf '# A rule file\n\n## Open work\n\n* **[#12](https://github.com/acme/widget/issues/12)** — a residual.\n' \
    > "${st}/star/a.md"
_stub "${st}/bin-star" "12:closed:issue" ""
_case "star-bullet-is-an-entry" 1 "FAIL stale: .*#12" "" "${st}/star" "${st}/bin-star"

# So was a one-space-indented `-`, which CommonMark still reads as a sibling.
mkdir -p "${st}/indented"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — ok.\n - **[#12](https://github.com/acme/widget/issues/12)** — a residual.\n' \
    > "${st}/indented/a.md"
_stub "${st}/bin-indented" "11:open:issue 12:closed:issue" ""
_case "one-space-indent-is-still-an-entry" 1 "FAIL stale: .*#12" "" "${st}/indented" "${st}/bin-indented"

# A fenced sample opened a real section and its sample bullet became a real
# entry, so the check reported `stale` about a documentation example and told
# the reader to delete it. `.agent/rules/README.md` carries such a sample.
mkdir -p "${st}/fenced-sample"
printf '# A rule file\n\nAn example of the shape:\n\n```markdown\n## Open work\n\n- **[#99](https://github.com/acme/widget/issues/99)** — sample.\n```\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — real.\n' \
    > "${st}/fenced-sample/a.md"
_stub "${st}/bin-fenced" "11:open:issue 99:closed:issue" ""
_case "a-fenced-sample-is-not-an-entry" 0 "resolved 1 of 1" "FAIL" "${st}/fenced-sample" "${st}/bin-fenced"

# And the other direction: a fenced repro block inside a REAL section was
# refused as `unparsed-entry`.
mkdir -p "${st}/fenced-repro"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — a residual. Repro:\n\n```sh\n- run this\n```\n' \
    > "${st}/fenced-repro/a.md"
_stub "${st}/bin-repro" "11:open:issue" ""
_case "a-fenced-repro-is-not-a-bullet" 0 "resolved 1 of 1" "FAIL" "${st}/fenced-repro" "${st}/bin-repro"

# Four lines in the real rulebook begin with `#` because an issue reference
# landed at a wrap point. Under a `/^#/` test, one carrying the words was
# refused as a renamed heading -- a refusal naming a line that is not a heading.
mkdir -p "${st}/hash-prose"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — one, and\n#619 open work entries were audited.\n' \
    > "${st}/hash-prose/a.md"
_stub "${st}/bin-prose" "11:open:issue" ""
_case "hash-prefixed-prose-is-not-a-heading" 0 "resolved 1 of 1" "FAIL heading" \
    "${st}/hash-prose" "${st}/bin-prose"

# The emptied-section refusal counted per FILE, so a file with one populated
# section and one emptied one passed.
mkdir -p "${st}/two-sections"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — one.\n\n## Middle\n\n## Open work\n\n## End\n' \
    > "${st}/two-sections/a.md"
_stub "${st}/bin-two" "11:open:issue" ""
_case "a-second-section-emptied-is-refused" 1 "FAIL empty-section" "" "${st}/two-sections" "${st}/bin-two"

# --- the two resolution branches nothing drove -----------------------------

# A repository probe that fails on anything but 404 is the PREREQUISITE case:
# nothing has resolved, and `gh api repos/<slug>` shares a rate limit and a
# transport with every query after it. It used to go RED while all entries
# resolved cleanly, contradicting this file's own stated contract.
_stub "${st}/bin-slug-auth" "11:open:issue 12:open:issue" "slug-auth"
_case "slug-probe-fault-is-a-prerequisite" 77 "SKIPPED: could not resolve the repository" "FAIL" \
    "${st}/clean" "${st}/bin-slug-auth"

# --- the grammar arms ------------------------------------------------------
#
# These reach no network, and they are given a stub that SHOUTS if they do. Six
# of them used to run with the ambient `PATH`, where `gh` is present and
# authenticated on a developer machine; they were hermetic only because the
# grammar gate exits before `command -v gh`. Weaken that gate and the suite
# silently becomes a network client -- so the isolation is asserted rather than
# inherited, with `deny_re` on the stub's own complaint.
_stub "${st}/bin-forbidden" "" "forbidden"
FORBID="${st}/bin-forbidden"
DENY_NET="stub-gh: reached the network"


# An empty directory: the glob matches nothing.
mkdir -p "${st}/empty"
_case "no-rule-files" 1 "FAIL census: the glob matched no rule files" "$DENY_NET" "${st}/empty" "$FORBID"

# Rule files with no Open work section anywhere.
mkdir -p "${st}/no-section"
printf '# A rule file\n\nA rule.\n' > "${st}/no-section/a.md"
_case "no-entries" 1 "FAIL census: read 0 Open work entries" "$DENY_NET" "${st}/no-section" "$FORBID"

# A renamed heading. The file still has bullets and still looks tidy.
mkdir -p "${st}/renamed"
printf '# A rule file\n\n## Open Work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — a residual.\n' \
    > "${st}/renamed/a.md"
_case "renamed-heading" 1 "FAIL heading: .*spells the section heading as" "$DENY_NET" "${st}/renamed" "$FORBID"

# A bullet in the section that opens with no issue link.
mkdir -p "${st}/unparsed"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — a residual.\n- something somebody meant to finish later.\n' \
    > "${st}/unparsed/a.md"
_case "unparsed-entry" 1 "FAIL unparsed-entry: .*is a bullet in an Open work section" "$DENY_NET" "${st}/unparsed" "$FORBID"

# Link text and URL disagreeing. Reads correctly; resolves to something else.
mkdir -p "${st}/transposed"
printf '# A rule file\n\n## Open work\n\n- **[#64](https://github.com/acme/widget/issues/46)** — a residual.\n' \
    > "${st}/transposed/a.md"
_case "link-text-disagrees" 1 "FAIL link-text: .*reads '#64' and links to issue 46" "$DENY_NET" "${st}/transposed" "$FORBID"

# A heading with nothing under it.
mkdir -p "${st}/emptied"
printf '# A rule file\n\nA rule.\n\n## Open work\n' > "${st}/emptied/a.md"
_case "emptied-section" 1 "FAIL empty-section: .*lists no entry under it" "$DENY_NET" "${st}/emptied" "$FORBID"

# --- and the mode the DEFAULT ctest set actually runs -----------------------
#
# Every case above drives `--resolve`. That runs the grammar first, so the rules
# are covered -- but `--extract`'s own exit path is a second registration, and a
# registration no case drives is one nobody has watched decide anything.
_case "extract-control-passes" 0 "1 section" "FAIL|${DENY_NET}" "${st}/clean" "$FORBID" "--extract"
_case "extract-refuses-a-bad-bullet" 1 "FAIL unparsed-entry" "$DENY_NET" "${st}/unparsed" "$FORBID" "--extract"

# A citation inside a bullet's prose, naming a CLOSED issue. This is the case the
# ticket's own measurement got wrong twice, and it must PASS: the citation is the
# landed change that created the residual, and it carries no claim of being open.
mkdir -p "${st}/citation"
printf '# A rule file\n\n## Open work\n\n- **[#11](https://github.com/acme/widget/issues/11)** — a residual, and\n  since [#12](https://github.com/acme/widget/issues/12) the banner is the identity.\n' \
    > "${st}/citation/a.md"
_stub "${st}/bin-citation" "11:open:issue 12:closed:issue" ""
_case "prose-citation-of-a-closed-issue-passes" 0 "resolved 1 of 1" "FAIL" \
    "${st}/citation" "${st}/bin-citation"

# A `###` inside the section does not close it, so the entry after one is still
# scanned. Written because the obvious `^#` terminator would drop it silently.
mkdir -p "${st}/subheading"
printf '# A rule file\n\n## Open work\n\n### A group\n\n- **[#12](https://github.com/acme/widget/issues/12)** — a residual.\n\n## Something else\n\n- **[#99](https://github.com/acme/widget/issues/99)** — not an entry.\n' \
    > "${st}/subheading/a.md"
_stub "${st}/bin-subheading" "12:closed:issue" ""
_case "subheading-does-not-close-the-section" 1 "FAIL stale: .*#12" "#99" \
    "${st}/subheading" "${st}/bin-subheading"

# ---------------------------------------------------------------------------

# The count is printed whatever the outcome. A self-test that stops early must
# not look like one that judged something: `set -e` plus a generator ending in a
# `[[ ... ]] && echo` truncated a run at eight cases with no case named, and the
# run read as a pass.
echo
echo "rulebook-open-work-selftest: ${cases_run} cases ran, ${failures} failed"
if [ "$failures" -gt 0 ]; then
    echo "  failed: ${failed_cases}" >&2
    exit 1
fi
exit 0
