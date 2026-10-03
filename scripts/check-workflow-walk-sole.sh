#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# No script under `scripts/` keeps a private awk walk over workflow YAML (#1456).
#
# ## Why this exists rather than the migrations being enough
#
# Five checks each carried their own model of a GitHub Actions workflow, and
# `scripts/lib/workflow-walk.awk` now serves all five. What that does NOT stop is
# the sixth: somebody writing a new check reaches for `/^  [A-Za-z0-9_-]+:/`
# because it is four characters and works on the file in front of them, and
# nothing anywhere says otherwise. Every one of the five was written that way, by
# somebody who was right about the file and wrong about YAML.
#
# ## What it looks for, stated as a pattern
#
# An awk regex literal anchored on a FIXED indentation -- `/^` followed by two or
# more literal spaces, or `/^[ ]{N}` with N above one -- in a first-party script
# or awk program that also names `workflows`. That is the shape of a column count
# standing in for a depth: `/^    if:/` is a job condition in `build.yml` and
# nothing at all in a file whose jobs sit deeper, which is legal YAML.
#
# A depth-RELATIVE pattern is not flagged and must not be: `/^[ \t]*$/` and
# `/^-?[ \t]*/` say nothing about how deep anything sits, which is the property
# the shared walk is built on.
#
# ## What it does NOT cover, said plainly
#
# A fixed indent built at RUN TIME escapes it -- `awk -v job="  $2:"` was one of
# the spellings this ticket removed, and no textual scan can see it. So can a
# walk written with `substr($0, 1, 4)` or with `index($0, "    if:")`. The scan is
# the common shape rather than the class, and a check silent about a case reads
# exactly like one that cleared it.
#
# ## Exemptions carry a reason, and a stale row is refused
#
# A fixture GENERATOR legitimately spells the shape it stages -- it is writing a
# workflow fragment, not reading one -- so it is exempt by row rather than by a
# broadened pattern. A row that no longer matches anything is refused as stale,
# because an exemption kept past its subject silently excuses the next site that
# happens to look like it.
set -uo pipefail

# Both overridable, and only so that `--self-test` can stage a whole tree -- a git
# repository, real `git ls-files`, real third-party roots -- rather than driving the
# predicate alone. The two halves this tree has been bitten by are the ENUMERATION
# answering nothing and the PATTERN being broader than its author reads it as, and only
# one of those is a predicate.
FastCachedRoot="${FASTCACHED_WALK_SOLE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FastCachedExemptions="${FASTCACHED_WALK_SOLE_EXEMPTIONS:-${FastCachedRoot}/scripts/check-workflow-walk-sole-exemptions.txt}"
# shellcheck source=scripts/lib/third-party-roots.sh
. "${FastCachedRoot}/scripts/lib/third-party-roots.sh"

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    selfTestCases=0
    selfTestStatus=0
    me="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    # A scratch REPOSITORY, because the enumeration is `git ls-files` and a
    # directory walk would be a different instrument. `git -c` rather than a
    # global config, so a machine with none does not abort the run.
    Stage() {
        rm -rf "$scratch/tree"
        mkdir -p "$scratch/tree/scripts/lib"
        # This script is NOT staged, and that is the point rather than an economy.
        # Its own fixtures below spell the very shape it refuses, as string
        # literals, so a staged copy would be flagged by construction -- the
        # rulebook's *a file that matches its own scan exempts a REGION, never
        # itself*, met here by not putting it in scope at all. The REAL script is
        # run against the staged tree through the root override.
        cp "${me%/*}/lib/third-party-roots.sh" "$scratch/tree/scripts/lib/"
        cp "${me%/*}/lib/third-party-roots.txt" "$scratch/tree/scripts/lib/"
        : > "$scratch/tree/exemptions.txt"
        git -C "$scratch/tree" init -q 2>/dev/null
    }

    Track() { git -C "$scratch/tree" add -A -f >/dev/null 2>&1; }

    # @param 1 what is being staged  @param 2 want-pass|want-fail|want-refuse
    Case() {
        local what="$1" want="$2" out got=0
        selfTestCases=$((selfTestCases + 1))
        Track
        out="$(FASTCACHED_WALK_SOLE_ROOT="$scratch/tree" \
               FASTCACHED_WALK_SOLE_EXEMPTIONS="$scratch/tree/exemptions.txt" \
               bash "$me" 2>&1)" || got=$?
        case "$want" in
            want-pass)   [ "$got" -eq 0 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-fail)   [ "$got" -eq 1 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-refuse) [ "$got" -eq 2 ] && { echo "  ok    ($want) $what"; return; } ;;
        esac
        echo "  FAIL  ($want, exit $got) $what" >&2
        printf '%s\n' "$out" | sed 's/^/        /' >&2
        selfTestStatus=1
    }

    # The baseline. Every refusing case below is evidence only if a tree with one
    # ordinary workflow reader passes -- and the reader has to NAME a workflow, or
    # the scan refuses for having nothing to scan, which is a different verdict.
    Stage
    printf '%s\n' '# reads .github/workflows through the shared walk' \
        'awk -f scripts/lib/workflow-walk.awk -f mine.awk "$1" "$1"' > "$scratch/tree/scripts/ordinary.sh"
    Case "a reader that names a workflow and keeps no private walk passes" want-pass

    # walk-sole-fixtures: begin -- the lines below PLANT the shape this scan refuses, so
    # they are a region this file exempts from its own scan. A row exempting the whole file
    # would turn the scan off for the one file whose fixtures are its evidence.
    # The arm this scan exists for.
    Stage
    printf '%s\n' '# walks .github/workflows itself' \
        "awk '/^  [A-Za-z0-9_-]+:/ { job = \$0 } /^    if:/ { print job }' \"\$1\"" \
        > "$scratch/tree/scripts/private.sh"
    Case "a private awk walk anchored on a fixed indent is REFUSED" want-fail

    # And the exemption mechanism, over the same planted site.
    printf 'scripts/private.sh\tstaged by the self-test\n' > "$scratch/tree/exemptions.txt"
    Case "the same site passes once a row exempts it, so the mechanism works" want-pass

    # A row that matches nothing is refused as stale.
    Stage
    printf '%s\n' '# reads .github/workflows through the shared walk' \
        'awk -f scripts/lib/workflow-walk.awk -f mine.awk "$1" "$1"' > "$scratch/tree/scripts/ordinary.sh"
    printf 'scripts/gone.sh\ta row for a file that is not there\n' > "$scratch/tree/exemptions.txt"
    Case "an exemption row that matches nothing is REFUSED as stale" want-fail

    # ---- the must-NOT-catch half ------------------------------------------
    # Without these a pattern broadened until it fires on the remedy would pass
    # every case above. Each is a NEAR MISS of the shape, not an unrelated file.
    Stage
    printf '%s\n' '# reads .github/workflows, depth-relative' \
        "awk '/^[ \\t]*\$/ { next } { ind = match(\$0, /[^ ]/) }' \"\$1\"" \
        > "$scratch/tree/scripts/relative.sh"
    Case "a DEPTH-RELATIVE pattern is not a fixed indent and passes" want-pass

    Stage
    # No `sed` anywhere in this prose -- not even inside "used": the sed-stripping rule would
    # remove the shape itself and this case would pass with comment-stripping switched off,
    # which is what it did until the prose was reworded (measured by neutering that rule).
    printf '%s\n' '# reads .github/workflows; the shape below is quoted in prose' \
        '# it once read /^  [A-Za-z0-9_-]+:/ and that was the defect' \
        'awk -f scripts/lib/workflow-walk.awk -f mine.awk "$1" "$1"' \
        > "$scratch/tree/scripts/documented.sh"
    Case "the shape inside a full-line COMMENT is documentation and passes" want-pass

    Stage
    printf '%s\n' '# stages a fragment for .github/workflows' \
        "sed -i.bak 's/^    if: true\$/    if: false/' \"\$1\"" \
        > "$scratch/tree/scripts/stager.sh"
    Case "the shape inside a sed expression writes YAML rather than reading it, and passes" want-pass

    # Two files, because a tree where NOTHING names a workflow is the enumeration
    # refusal rather than a pass -- so the out-of-scope file needs an in-scope one
    # beside it, or the case measures a different verdict than the one it names.
    Stage
    printf '%s\n' '# reads .github/workflows through the shared walk' \
        'awk -f scripts/lib/workflow-walk.awk -f mine.awk "$1" "$1"' > "$scratch/tree/scripts/ordinary.sh"
    printf '%s\n' '# reads a TSV table, not YAML' \
        "awk '/^  [A-Za-z0-9_-]+:/ { print }' table.tsv" > "$scratch/tree/scripts/tsv.sh"
    Case "a file carrying the shape that names no workflow is out of scope and passes" want-pass

    # walk-sole-fixtures: end

    # A region marker without its closing one is REFUSED, never read as an empty
    # scan. Unbalanced, an exempt region runs to the end of the file, which is the
    # whole-file exemption arriving by accident.
    #
    # The marker is SPLIT across the format and its argument, and that is necessary
    # rather than cute: spelled whole it would be a real marker in THIS file, and
    # the real run -- which does scan this file -- would open a region at that line
    # and carry it to the end. A fixture that plants a marker cannot spell one.
    Stage
    {
        printf '%s\n' '# reads .github/workflows through the shared walk'
        printf '# walk-sole-%s: begin\n' fixtures
        printf '%s\n' 'awk -f scripts/lib/workflow-walk.awk -f mine.awk "$1" "$1"'
    } > "$scratch/tree/scripts/halfmarked.sh"
    Case "a region marker with no closing marker is REFUSED, not read as an empty scan" want-fail

    # ---- the enumeration, which is the other half -------------------------
    Stage
    Case "a tree whose scripts name no workflow at all is REFUSED, not read as clean" want-refuse

    Stage
    rm -rf "$scratch/tree/.git"
    Case "a tree git cannot read is REFUSED, never walked instead" want-refuse

    echo "check-workflow-walk-sole --self-test: ${selfTestCases} case(s) ran"
    if [ "$selfTestStatus" -ne 0 ]; then
        echo "check-workflow-walk-sole --self-test: FAILED" >&2
        exit 1
    fi
    echo "check-workflow-walk-sole --self-test: every verdict as it must be"
    exit 0
fi

Problems=0
Fail() { echo "  FAIL: $*" >&2; Problems=$((Problems + 1)); }

# Every first-party shell script and awk program under `scripts/`, one per line, into
# `SubjectList` -- a global rather than stdout, so it runs ONCE: returned through
# `$(...)` it ran twice, the first time only so `FastCachedDeclined` survived the
# subshell. Enumerated through git, which is the one answer to which files are this
# project's: a directory walk would take a stray copy in a build tree as a subject
# and report on a file nobody tracks.
Subjects() {
    local tracked firstParty declined
    tracked="$(git -C "${FastCachedRoot}" ls-files -- 'scripts/*.sh' 'scripts/*.awk')" || return 2
    if [ -z "${tracked}" ]; then
        echo "check-workflow-walk-sole: git ls-files named no script under scripts/, which cannot be" >&2
        echo "  true. Refused rather than read as 'no script keeps a private walk'." >&2
        return 2
    fi
    if ! firstParty="$(first_party_paths "${FastCachedRoot}" "${tracked}")"; then
        echo "check-workflow-walk-sole: the third-party roots could not be read, so which files are" >&2
        echo "  this project's own is unknown. Refused." >&2
        return 2
    fi
    declined="$(third_party_paths "${FastCachedRoot}" "${tracked}")" || return 2
    FastCachedDeclined="$(third_party_declined_summary 'script(s)' "${declined}")"
    SubjectList="${firstParty}"
}

# The subjects that name a workflow, and their fixed-indent awk literals, for every file
# named after the arguments -- in ONE awk pass, as records:
#
#   SCANNED<TAB><path>                 the file names `workflows`, so it is in scope
#   UNBALANCED<TAB><path>              its `walk-sole-fixtures` markers do not balance
#   HIT<TAB><path><TAB><line>:<text>   a fixed-indent literal, the line as it was judged
#
# One process for the whole tree, not a `grep` per script and an `awk`, a `sed` and a
# `grep` per script that names a workflow: under a loaded ctest a process start costs
# far more than the scan, and this timed out at 60 s. What is judged is unchanged:
#
# Full-line comments are stripped first, because a COMMENT is not a call site --
# and here that is load-bearing rather than tidy: every migrated check documents
# the column counts it used to carry, quoting them exactly, so a scan that read
# comments would refuse the four files that prove it is satisfied.
#
# `sed` expressions are stripped too. A `s/^        WfBlockOwner = .../` neuter
# and a staged `sed -i 's/^  cancel-in-progress: false$/...'` both carry the
# shape and neither reads a workflow: one edits awk source, the other writes a
# fixture. They are recognised by the `s` before the slash and by `sed` on the
# line, which is narrower than it sounds and is why the exemption file exists.
#
# A REGION may exempt itself, and this file is the reason the mechanism exists:
# the self-test below plants the very shape this scan refuses, as string
# literals, so without a region the scan flags itself over the real tree. The
# rulebook's rule is exactly this -- *a file that matches its own scan by
# construction exempts a REGION, never itself* -- and exempting the FILE would
# turn the scan off for the one file whose fixtures are its own evidence.
#
# The markers are BALANCED or the file is refused. A `begin` whose `end` was deleted
# would skip the rest of the file in silence, which is the whole-file exemption
# arriving by accident. `workflows` is looked for in the RAW line, comments and
# regions included, as the per-file `grep -Fq` did.
#
# The literal `[`, `]`, `{` and `}` of the pattern are bracket expressions rather than
# escapes, so every awk reads them the same way.
ScanSubjects() {
    awk '
        function flush(    i) {
            if (file == "" || !hasWorkflow)
                return
            printf "SCANNED\t%s\n", file
            if (begins != ends)
                printf "UNBALANCED\t%s\n", file
            else
                for (i = 1; i <= hits; i++)
                    printf "HIT\t%s\t%s\n", file, hit[i]
        }
        FNR == 1 {
            flush()
            file = FILENAME
            hasWorkflow = 0
            begins = 0
            ends = 0
            inRegion = 0
            hits = 0
        }
        index($0, "workflows") > 0 { hasWorkflow = 1 }
        /walk-sole-fixtures: begin/ { begins++; inRegion = 1; next }
        /walk-sole-fixtures: end/   { ends++; inRegion = 0; next }
        inRegion { next }
        /^[ \t\r\f\v]*#/ { next }
        {
            line = $0
            gsub(/sed[^|;&]*/, "", line)
            gsub(/[^A-Za-z0-9]s\/\^[^\/]*\/[^\/]*\//, "", line)
            if (line ~ /\/\^(  +|[[] []][{][2-9][0-9]*[}])/)
                hit[++hits] = FNR ":" line
        }
        END { flush() }
    ' "$@"
}

SubjectList=""
Subjects || exit 2

# The exemption rows: `<path><TAB><reason>`. Read before the scan, so a row for a
# file that is not a subject at all is refused rather than quietly unused.
ExemptPaths=""
if [ -r "${FastCachedExemptions}" ]; then
    ExemptPaths="$(sed -e 's/^[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' "${FastCachedExemptions}" | cut -f1)"
fi

# True when @p 1 is one whole line of the newline-separated @p 2. A `case`, not a
# `grep -Fqx` per subject: the lookup is a membership test and needs no process.
IsLineOf() {
    case $'\n'"$2"$'\n' in
        *$'\n'"$1"$'\n'*) return 0 ;;
    esac
    return 1
}

# Judge one scanned subject's hits, which arrive together after its SCANNED record.
# @param 1 the subject  @param 2 its hits, one `<line>:<text>` per line, or nothing
JudgeSubject() {
    [ -n "$2" ] || return 0
    if IsLineOf "$1" "${ExemptPaths}"; then
        ExemptUsed="${ExemptUsed}${1}"$'\n'
        return 0
    fi
    Flagged=$((Flagged + 1))
    Fail "${1} carries an awk regex anchored on a fixed indentation, which is a column count standing in for a depth. Read the workflow through scripts/lib/workflow-walk.awk, which answers the depth itself; if this site STAGES a fragment rather than reading one, add a row to scripts/check-workflow-walk-sole-exemptions.txt saying so."
    local hit
    while IFS= read -r hit; do
        [ -n "${hit}" ] && printf '        %s\n' "${hit}" >&2
    done <<< "$2"
}

# The readable subjects, as paths: an unreadable one was skipped, and still is.
Readable=()
while IFS= read -r subject; do
    [ -n "${subject}" ] || continue
    [ -r "${FastCachedRoot}/${subject}" ] && Readable[${#Readable[@]}]="${FastCachedRoot}/${subject}"
done <<< "${SubjectList}"

Scanned=0
Flagged=0
ExemptUsed=""
Records=""
if [ "${#Readable[@]}" -gt 0 ]; then
    Records="$(ScanSubjects "${Readable[@]}")" || { echo "check-workflow-walk-sole: the scan did not complete. Refused." >&2; exit 2; }
fi
# Only a file that READS a workflow is a subject. A script matching indented lines in a
# TSV table or in markdown is not modelling YAML, and flagging it would be the
# over-broad half of this scan rather than its point.
current=""
currentHits=""
while IFS=$'\t' read -r kind path rest; do
    [ -n "${kind}" ] || continue
    subject="${path#"${FastCachedRoot}"/}"
    case "${kind}" in
        SCANNED)
            [ -z "${current}" ] || JudgeSubject "${current}" "${currentHits}"
            current="${subject}"
            currentHits=""
            Scanned=$((Scanned + 1))
            ;;
        UNBALANCED)
            echo "check-workflow-walk-sole: ${path} has unbalanced \`walk-sole-fixtures\` markers, so an" >&2
            echo "  exempt REGION would run to the end of the file. Refused rather than scanned." >&2
            Fail "${subject} could not be scanned, so nothing about it has been established."
            current=""
            currentHits=""
            ;;
        HIT)
            currentHits="${currentHits}${rest}"$'\n'
            ;;
        *)
            echo "check-workflow-walk-sole: the scan wrote a record of kind '${kind}', which this reader does not know. Refused." >&2
            exit 2
            ;;
    esac
done <<< "${Records}"
[ -z "${current}" ] || JudgeSubject "${current}" "${currentHits}"

# A row that matched nothing is refused. An exemption kept past its subject goes
# on excusing whatever next takes that path, and reads as a considered decision.
while IFS= read -r row; do
    [ -n "${row}" ] || continue
    if ! IsLineOf "${row}" "${ExemptUsed}"; then
        Fail "the exemption row for '${row}' matched nothing: either the file no longer carries a fixed-indent literal, or it is no longer scanned. Delete the row rather than leaving it to excuse the next site at that path."
    fi
done <<< "${ExemptPaths}"

if [ "${Scanned}" -eq 0 ]; then
    echo "check-workflow-walk-sole: no script under scripts/ names a workflow, which cannot be true." >&2
    echo "  Refused rather than read as 'no script keeps a private walk'." >&2
    exit 2
fi

echo "check-workflow-walk-sole: enumerated via git ls-files"
[ -z "${FastCachedDeclined:-}" ] || echo "check-workflow-walk-sole: ${FastCachedDeclined}"
exemptCount=0
while IFS= read -r row; do
    [ -n "${row}" ] && exemptCount=$((exemptCount + 1))
done <<< "${ExemptPaths}"
echo "check-workflow-walk-sole: ${Scanned} script(s) that name a workflow scanned, ${Flagged} carrying a private fixed-indent walk, ${exemptCount:-0} exempt by row"
if [ "${Problems}" -ne 0 ]; then
    echo "check-workflow-walk-sole: ${Problems} problem(s); a second model of workflow YAML is how the first five got written" >&2
    exit 1
fi
echo "check-workflow-walk-sole: the shared walk is the only model of workflow YAML under scripts/"
exit 0
