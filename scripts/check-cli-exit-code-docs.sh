#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The exit codes `docs/tools/fastcache-cli.md` lists are `OutcomeTable`'s, row for row:
# the same codes, the same names, the same meanings.
#
# ## Why this exists
#
# `OutcomeTable` in `src/apps/fastcache-cli/CliAnswer.hpp` is where an exit code is
# decided, and `--help` renders it, so the help cannot drift. The operator page could:
# its `## Exit codes` section restated all six codes by hand, under a sentence that
# restated their COUNT as well, and #134 added a seventh (`local`). A listing written by
# hand is right until the table grows, and the table grows exactly when a verb meets a
# case none of the existing codes answers -- which is the edit where a doc table is
# forgotten.
#
# The cost is not cosmetic. An exit code is the one part of this tool a script reads,
# and the page is where a script's author looks it up. A code missing from the page is
# a status somebody's `case` falls through; a code the page gives a different MEANING is
# a script that retries the server when the remedy is on this machine.
#
# ## Why the meaning is compared as well, word for word
#
# Comparing codes and names alone would let the prose beside each drift, and the prose
# is what a reader acts on. So the page carries the table's text verbatim. The page's
# own explanation lives BELOW the table, in the pairs that matter, where no check reads.
#
# ## What it does NOT cover, stated rather than left to be discovered
#
#   * The prose outside the table -- the pairs list, the example script, and
#     `docs/tools/index.md`'s summary. A sentence is not a list, and the sentences no
#     longer state how many codes there are, which is the claim that went stale.
#   * A meaning `CliAnswer.hpp` spells as two concatenated string literals, or one
#     carrying an escaped quote. The reader sees only the first literal, so such a row
#     REFUSES as differing -- loudly, naming the row -- rather than passing. Joining the
#     literal is the remedy; the check does not grow a C++ string parser.
#   * Two rows written on one source line. `clang-format` does not lay the table out
#     that way; a row is closed when the next `.outcome =` opens.
#
# ## Constraints
#
# bash 3.2 (macOS ships a 2007 `/bin/bash` and this runs in the default ctest set): no
# `mapfile`, `declare -A`, `${var^^}` or `local -n`. Rows travel as tab-separated lines,
# and are never split with `IFS=$'\t' read`, which collapses an empty field and shifts
# every field after it -- the comparison is one awk program instead.
set -uo pipefail

self="${0##*/}"
selfTest="no"
sourceDir=""

while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) selfTest="yes"; shift ;;
        -h|--help)
            echo "usage: ${self} <source-dir>"
            echo "       ${self} --self-test"
            exit 0
            ;;
        -*) echo "${self}: unknown argument: $1" >&2; exit 2 ;;
        *)
            if [ -n "$sourceDir" ]; then
                echo "${self}: unexpected argument: $1" >&2; exit 2
            fi
            sourceDir="$1"; shift
            ;;
    esac
done

failures=0
note() { echo "$*"; }
refuse() {
    echo "FAIL ${self}: $*" >&2
    failures=$(( failures + 1 ))
}

# `code<TAB>name<TAB>meaning` for each row of `OutcomeTable`'s literal.
#
# **Bounded to the literal**, because `OutcomeSpec` sits above it in the same header and
# its members are named `code`, `name` and `meaning` too. A row opens at `.outcome =`
# and is printed when the next one opens or the literal closes, so a row laid out on one
# line and a row laid out over four are read alike. A field the reader could not see is
# printed EMPTY rather than dropped, and the comparison refuses it by name.
#
# @param 1 path to CliAnswer.hpp
TableOutcomes() {
    awk '
        function flush() {
            if (open)
                printf "%s\t%s\t%s\n", code, name, meaning
            open = 0; code = ""; name = ""; meaning = ""
        }
        /inline constexpr EnumTable<Outcome, OutcomeSpec> OutcomeTable \{ \{/ { inside = 1; next }
        inside && /^\} \};/ { flush(); inside = 0 }
        !inside { next }
        match($0, /\.outcome = Outcome::[A-Za-z]+/) { flush(); open = 1 }
        match($0, /\.code = [0-9]+/)      { code = substr($0, RSTART + 8, RLENGTH - 8) }
        match($0, /\.name = "[^"]*"/)     { name = substr($0, RSTART + 9, RLENGTH - 10) }
        match($0, /\.meaning = "[^"]*"/)  { meaning = substr($0, RSTART + 12, RLENGTH - 13) }
        END { flush() }
    ' "$1"
}

# `code<TAB>name<TAB>meaning` for each row of the doc's `## Exit codes` table.
#
# Only rows whose first cell is a number, inside that section: the section's header row
# and separator are not, and a table in any OTHER section is not read at all. The name
# cell loses its backticks. An escaped `\|` is a literal pipe in markdown and is masked
# before the split, so a meaning carrying one is not cut in half.
#
# @param 1 path to fastcache-cli.md
DocOutcomes() {
    awk '
        function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
        /^## Exit codes/ { inside = 1; next }
        inside && /^## / { inside = 0 }
        !inside { next }
        /^\|[ \t]*[0-9]+[ \t]*\|/ {
            row = $0
            gsub(/\\\|/, "\001", row)
            n = split(row, cells, "|")
            if (n < 5) next
            code = trim(cells[2]); name = trim(cells[3]); meaning = trim(cells[4])
            if (name ~ /^`.*`$/) name = substr(name, 2, length(name) - 2)
            gsub(/\001/, "|", meaning)
            printf "%s\t%s\t%s\n", code, name, meaning
        }
    ' "$1"
}

# One line per disagreement between the two row sets, keyed on the code.
#
# **Callers must refuse an empty table file first**: with nothing in the first file,
# `FNR == NR` holds for the second one too and every doc row would be read as a table
# row.
#
# @param 1 the table's rows
# @param 2 the doc's rows
Disagreements() {
    awk -F'\t' '
        FNR == NR {
            if ($1 in table) print "OutcomeTable has two rows with code " $1
            table[$1] = $0; order[++count] = $1
            next
        }
        {
            if ($1 in doc) print "the Exit codes table lists code " $1 " twice"
            doc[$1] = $0
            if (!($1 in table))
                print "the Exit codes table lists code " $1 " (`" $2 "`) and no OutcomeTable row has that code"
        }
        END {
            for (i = 1; i <= count; ++i) {
                c = order[i]
                split(table[c], row, "\t")
                if (c == "" || row[2] == "" || row[3] == "") {
                    print "an OutcomeTable row reads as code `" c "`, name `" row[2] "`, meaning `" row[3] "`: a field is spelled some way this reader cannot see"
                    continue
                }
                if (!(c in doc)) {
                    print "code " c " (`" row[2] "`) is an OutcomeTable row and is not in the Exit codes table"
                    continue
                }
                if (doc[c] != table[c]) {
                    split(doc[c], seen, "\t")
                    print "code " c " differs: the docs say `" seen[2] "` -- " seen[3] "; OutcomeTable says `" row[2] "` -- " row[3]
                }
            }
        }
    ' "$1" "$2"
}

# Compare one tree.
#
# @param 1 source directory
Verify() {
    local dir="$1"
    local impl="${dir}/src/apps/fastcache-cli/CliAnswer.hpp"
    local doc="${dir}/docs/tools/fastcache-cli.md"
    local tableFile docFile line tableCount docCount

    if [ ! -r "$impl" ]; then
        refuse "cannot read ${impl}"
        return
    fi
    if [ ! -r "$doc" ]; then
        refuse "cannot read ${doc}"
        return
    fi

    tableFile="$(mktemp)"
    docFile="$(mktemp)"
    TableOutcomes "$impl" > "$tableFile"
    DocOutcomes "$doc" > "$docFile"

    tableCount="$(wc -l < "$tableFile" | tr -d ' ')"
    docCount="$(wc -l < "$docFile" | tr -d ' ')"

    # Fail CLOSED on either extraction coming back empty. **Two empty lists agree
    # perfectly**, and that agreement is exactly what a broken reader looks like: a
    # renamed table literal together with a renamed doc heading would otherwise report a
    # clean tree forever. With only ONE empty the comparison would still refuse, but row
    # by row, blaming every code rather than the reader that saw none -- and an empty
    # table file is one `Disagreements` cannot be handed at all.
    if [ "$tableCount" -lt 1 ]; then
        refuse "read no outcomes out of ${impl}: the OutcomeTable literal did not match"
        rm -f "$tableFile" "$docFile"
        return
    fi
    if [ "$docCount" -lt 1 ]; then
        refuse "read no exit codes out of ${doc}: the '## Exit codes' table did not match"
        rm -f "$tableFile" "$docFile"
        return
    fi

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        refuse "${line}. OutcomeTable in ${impl##*/} decides the codes and --help renders it, so make ${doc##*/}'s Exit codes table carry its rows word for word; changing the page's wording means changing the row"
    done < <(Disagreements "$tableFile" "$docFile")

    note "${self}: ${tableCount} outcomes in the table, ${docCount} exit codes in the docs"
    rm -f "$tableFile" "$docFile"
}

# --- self-test ----------------------------------------------------------------------
#
# Every way the two can disagree, both ways the reader can fail to LOOK, and the
# accepting direction -- a guard nobody has watched ACCEPT is not known to work (#1031).

# Stage a tree. Rows are newline-separated `code~name~meaning`; a meaning may carry `|`.
# The first table row is written on one line and the rest over four, as the real header
# lays them out, so every case reads both shapes. Both files carry a decoy an unbounded
# reader would take for a row: a designated initializer spelling all four fields below
# the literal, and a three-column numbered table in another section.
Stage() {
    local root="$1" tableRows="$2" docRows="$3"
    local row code rest name meaning first="yes"
    mkdir -p "${root}/src/apps/fastcache-cli" "${root}/docs/tools"
    {
        echo 'inline constexpr EnumTable<Outcome, OutcomeSpec> OutcomeTable { {'
        while IFS= read -r row; do
            [ -n "$row" ] || continue
            code="${row%%~*}"; rest="${row#*~}"; name="${rest%%~*}"; meaning="${rest#*~}"
            if [ "$first" = "yes" ]; then
                printf '    { .outcome = Outcome::Row%s, .code = %s, .name = "%s", .meaning = "%s" },\n' \
                    "$code" "$code" "$name" "$meaning"
                first="no"
            else
                printf '    { .outcome = Outcome::Row%s,\n' "$code"
                printf '      .code = %s,\n' "$code"
                printf '      .name = "%s",\n' "$name"
                printf '      .meaning = "%s" },\n' "$meaning"
            fi
        done <<< "$tableRows"
        echo '} };'
        echo ''
        echo 'OutcomeSpec Decoy()'
        echo '{'
        echo '    return { .outcome = Outcome::Decoy, .code = 99, .name = "decoy", .meaning = "not a row" };'
        echo '}'
    } > "${root}/src/apps/fastcache-cli/CliAnswer.hpp"
    {
        echo '# `fastcache-cli`'
        echo ''
        echo '## Exit codes'
        echo ''
        echo '| | | |'
        echo '|--:|---|---|'
        while IFS= read -r row; do
            [ -n "$row" ] || continue
            code="${row%%~*}"; rest="${row#*~}"; name="${rest%%~*}"; meaning="${rest#*~}"
            printf '| %s | `%s` | %s |\n' "$code" "$name" "$(printf '%s' "$meaning" | sed 's/|/\\|/g')"
        done <<< "$docRows"
        echo ''
        echo '- **1 against 3** -- prose naming codes, which must not be read as a listing.'
        echo ''
        echo '## Something else'
        echo ''
        echo '| | |'
        echo '|--:|---|'
        echo '| 9 | `decoy` | a numbered table outside the section |'
    } > "${root}/docs/tools/fastcache-cli.md"
}

RunCase() {
    local name="$1" want="$2" root="$3"
    local before="$failures"
    Verify "$root" > /dev/null 2>&1
    local got="pass"
    [ "$failures" -gt "$before" ] && got="fail"
    # The counter is reset per case: a self-test tallying its subject's refusals as its
    # own would report every deliberate refusal as a failure of the run.
    failures="$before"
    if [ "$got" = "$want" ]; then
        echo "  ok   ${name} (${got})"
        return 0
    fi
    echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
    return 1
}

# The self-test's scratch directory, at FILE scope rather than `local` to SelfTest.
# The trap runs at EXIT, by which time a local is out of scope -- under `set -u` that
# is an unbound-variable error AFTER the verdict is printed, so the run reports success
# and leaves the directory behind, which is the quiet half.
selfTestTmp=""
CleanSelfTest() { [ -n "$selfTestTmp" ] && rm -rf "$selfTestTmp"; }

SelfTest() {
    local bad=0 ran=0 tmp rows
    selfTestTmp="$(mktemp -d)"
    tmp="$selfTestTmp"
    trap CleanSelfTest EXIT

    rows="0~ok~the command was answered
1~no~the answer is no
4~refused~the server answered and declined
6~local~this machine could not carry the command out"

    Stage "${tmp}/agree" "$rows" "$rows"
    RunCase "a tree whose table and docs agree" pass "${tmp}/agree" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/unlisted" "$rows" "$(printf '%s\n' "$rows" | sed '/^6~/d')"
    RunCase "a code in the table and not in the docs" fail "${tmp}/unlisted" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/invented" "$rows" "${rows}
7~later~a code nothing exits with"
    RunCase "a code in the docs and not in the table" fail "${tmp}/invented" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/renamed" "$rows" "$(printf '%s\n' "$rows" | sed 's/^6~local~/6~host~/')"
    RunCase "a code the docs give another name" fail "${tmp}/renamed" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/reworded" "$rows" "$(printf '%s\n' "$rows" | sed 's/^4~refused~.*/4~refused~the server said no/')"
    RunCase "a code the docs give another meaning" fail "${tmp}/reworded" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/swapped" "$rows" "$(printf '%s\n' "$rows" | sed 's/^4~/X~/; s/^6~/4~/; s/^X~/6~/')"
    RunCase "two names the docs file under each other's codes" fail "${tmp}/swapped" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/twice" "$rows" "${rows}
6~local~this machine could not carry the command out"
    RunCase "a code the docs list twice" fail "${tmp}/twice" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/pipe" "${rows}
5~protocol~one | two" "${rows}
5~protocol~one | two"
    RunCase "a meaning carrying a pipe is read whole on both sides" pass "${tmp}/pipe" || bad=1
    ran=$(( ran + 1 ))

    # The reader failing to LOOK, in both places. Each leaves one list empty, and two
    # empty lists agree -- so without these the check would report a clean tree for a
    # renamed literal or a renamed heading, forever.
    Stage "${tmp}/renamed-table" "$rows" "$rows"
    sed 's/OutcomeTable { {/RenamedTable { {/' \
        "${tmp}/renamed-table/src/apps/fastcache-cli/CliAnswer.hpp" > "${tmp}/renamed-table/x" \
        && mv "${tmp}/renamed-table/x" "${tmp}/renamed-table/src/apps/fastcache-cli/CliAnswer.hpp"
    RunCase "a renamed table literal refuses rather than reading nothing" fail "${tmp}/renamed-table" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/renamed-heading" "$rows" "$rows"
    sed 's/^## Exit codes/## Statuses/' \
        "${tmp}/renamed-heading/docs/tools/fastcache-cli.md" > "${tmp}/renamed-heading/x" \
        && mv "${tmp}/renamed-heading/x" "${tmp}/renamed-heading/docs/tools/fastcache-cli.md"
    RunCase "a renamed doc heading refuses rather than reading nothing" fail "${tmp}/renamed-heading" || bad=1
    ran=$(( ran + 1 ))

    # A meaning split across two literals is read as its first half, and must refuse
    # rather than pass -- the header says so, and this is where that is watched.
    Stage "${tmp}/split-literal" "$rows" "$rows"
    sed 's/"the server answered and declined"/"the server answered "\
      "and declined"/' \
        "${tmp}/split-literal/src/apps/fastcache-cli/CliAnswer.hpp" > "${tmp}/split-literal/x" \
        && mv "${tmp}/split-literal/x" "${tmp}/split-literal/src/apps/fastcache-cli/CliAnswer.hpp"
    RunCase "a meaning split across two literals refuses rather than passing" fail "${tmp}/split-literal" || bad=1
    ran=$(( ran + 1 ))

    # Both at once is the case the empty-list refusals exist for: with ONE list empty the
    # comparison would still find every row of the other missing, but two empty lists
    # compare equal and would report a clean tree.
    Stage "${tmp}/renamed-both" "$rows" "$rows"
    sed 's/OutcomeTable { {/RenamedTable { {/'         "${tmp}/renamed-both/src/apps/fastcache-cli/CliAnswer.hpp" > "${tmp}/renamed-both/x"         && mv "${tmp}/renamed-both/x" "${tmp}/renamed-both/src/apps/fastcache-cli/CliAnswer.hpp"
    sed 's/^## Exit codes/## Statuses/'         "${tmp}/renamed-both/docs/tools/fastcache-cli.md" > "${tmp}/renamed-both/x"         && mv "${tmp}/renamed-both/x" "${tmp}/renamed-both/docs/tools/fastcache-cli.md"
    RunCase "a renamed literal AND a renamed heading refuse rather than agreeing on nothing" fail "${tmp}/renamed-both" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/missing-doc" "$rows" "$rows"
    rm -f "${tmp}/missing-doc/docs/tools/fastcache-cli.md"
    RunCase "a missing doc is a refusal, not a pass" fail "${tmp}/missing-doc" || bad=1
    ran=$(( ran + 1 ))

    # The decoys are what make the bounded reads worth having, and the fixtures carry
    # them silently -- so this says out loud that they are there, rather than letting
    # the property rest on the fixture unobserved.
    if grep -Fq '.code = 99, .name = "decoy"' "${tmp}/agree/src/apps/fastcache-cli/CliAnswer.hpp" \
        && grep -Fq '| 9 | `decoy` |' "${tmp}/agree/docs/tools/fastcache-cli.md"; then
        echo "  ok   the fixtures carry a decoy row below the literal and a numbered table outside the section"
        ran=$(( ran + 1 ))
    else
        echo "  FAIL a decoy is missing, so a bounded read is untested" >&2
        bad=1
    fi

    # A self-test that stopped early must not look like one that judged something.
    echo "${self}: ${ran} self-test cases ran"
    return "$bad"
}

if [ "$selfTest" = "yes" ]; then
    if SelfTest; then
        echo "${self}: self-test passed"
        exit 0
    fi
    echo "${self}: self-test FAILED" >&2
    exit 1
fi

if [ -z "$sourceDir" ]; then
    echo "${self}: a source directory is required" >&2
    exit 2
fi

Verify "$sourceDir"

if [ "$failures" -gt 0 ]; then
    echo "${self}: ${failures} problem(s)" >&2
    exit 1
fi
echo "${self}: OK"
exit 0
