#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# What the node's option table says RELOADS, and what the shipped artefacts claim.
#
# `OptionSpec::reloadable` decides whether a setting can change under SIGHUP. Two
# artefacts describe that set to operators and nothing derived either from the table:
# the docs' Reloadable/restart table, and the shipped reference configuration's count.
# It had already drifted -- hand-corrected from four to five to seven in one branch --
# and the population is growing rather than static (#403, #293, #404, #405 each added
# a row), so the drift rate rises with no signal (#1026).
#
# BOTH DIRECTIONS, and the second is the one that matters. A check written from the
# drift that was observed covers only "a new row, no artefact update", which costs a
# reader a stale doc. The operator failure is the other way: an artefact naming a
# setting as reloadable that the table says is NOT -- the operator edits the shipped
# file, sends SIGHUP, sees no error, and believes a setting is in force that is not.
#
# The artefact SET is derived by MARKER, never listed. A hand-kept list is exact about
# the files it knows and silent about the ones it does not, and silence reads
# identically to complete coverage (#492) -- that argument applies to a checker's own
# scope list as much as to its subject. So a third file adopting either marker is
# covered the day it does, and a file that drops one makes the scan match nothing,
# which is a REFUSAL: two empty lists agree perfectly.
#
# The stated count is PARSED from the table rather than compared to a number typed
# beside it (#780). It is spelled as an English WORD in the shipped file, so the
# numerals are a table here rather than a regex that would silently stop matching.
#
# What this does NOT cover, stated rather than papered over: the reference
# configuration also marks individual settings "Reloadable" in prose beside their
# keys, and that prose is not attached to a key in any machine-readable way. Parsing
# it would break on a rewording and report on the wrong thing; the COUNT is what this
# check reads from that file. The per-setting claim in the DOCS table is covered, and
# it is the one an operator reads first.
#
# bash 3.2: no mapfile, no `declare -A`, no `${var^^}` -- a hygiene script runs on
# every platform CI builds and macOS ships a 2007 bash. Sets are sorted newline lists
# compared with `comm`, which both platforms have.
#
# Invoke as `bash scripts/check-node-reloadable-docs.sh [<source-dir>]`, never bare:
# 15 of the scripts here are mode 644 in git and a bare call exits 126, which inside a
# want-fail assertion is indistinguishable from the rule firing (#723).
#
# **The NAME is historical, and the scope is not.** This was written for
# `fastcache-compile-node` (#1026) and now checks every binary in `SubjectTable` below --
# the daemon joined at
# [#1070](https://github.com/LASTRADA-Software/fastcached/issues/1070). The file and its
# two ctest entries keep the node-shaped name on purpose rather than for want of a
# rename: `.agent/rules/build-and-toolchain.md` cites `node-reloadable-docs` and
# `node-reloadable-docs-selftest` by name in its table of ways an instrument reported on
# a tree other than the one under test, and those rows record what was OBSERVED at an
# instant. Renaming the check would re-attribute a real measurement to a name that did
# not exist when it was taken, which is the failure that rulebook file warns about one
# section above. Every line this prints names its SUBJECT instead, so a reader of a
# failure is never in doubt about which binary it is talking about.
#
# Usage:
#   bash scripts/check-node-reloadable-docs.sh [<source-dir>]
#   bash scripts/check-node-reloadable-docs.sh --self-test

set -uo pipefail

# Which files are third-party, asked of the tree being checked (#1370). An upstream file
# cannot declare a subject of THIS project's, whatever text it happens to carry.
# shellcheck source=lib/third-party-roots.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/third-party-roots.sh" \
    || { echo "FAIL node-reloadable-docs: cannot read scripts/lib/third-party-roots.sh" >&2; exit 1; }

# The markers that DEFINE the artefact set. Spelled once each: they are what the scan
# searches for, what the refusal names, and what a new artefact would have to adopt.
# The SUBJECT marker: which binary's option table an artefact is describing.
#
# It exists because the marker alone was not enough, and that was measured rather than
# foreseen. Scanning for the Reloadable/restart table found a THIRD file the ticket did
# not name -- `docs/snippets/reload-matrix.md`, included into two daemon pages -- which
# carries the identical table header while describing `CliOptions()`, a different
# binary with a different set. A pattern is broader than its author reads it as, and
# this one matched two subjects as though they were one.
#
# So an artefact DECLARES its subject and the scope is derived from that declaration.
# A file with no marker is not in scope, which is the daemon snippet's honest state
# today; a file whose marker names a binary this check does not handle is REFUSED by
# name, so the gap cannot be filled silently by adding a marker somewhere.
readonly SubjectMarker="reloadable-for:"
readonly DocsTableMarker="| Reloadable | Requires a restart |"
readonly CountMarker="settings are reloadable"

# One row per binary whose reloadable set is checked: the subject an artefact declares,
# and the option table that is the truth for it.
#
# **A TABLE rather than one `Subject`, which is what #1070 is**
# ([#1070](https://github.com/LASTRADA-Software/fastcached/issues/1070)). #1026 built this
# for the node and hard-coded one binary, and the daemon's matrix -- `docs/snippets/reload-matrix.md`,
# included into two pages -- then stated a set for `CliOptions()` with nothing deriving it.
# The marker mechanism was designed for exactly this: a second binary is a row here plus a
# marker on its artefacts, never a second script.
#
# The refusal below is what makes the table complete rather than a prefix of one: an
# artefact declaring a subject with no row is REFUSED BY NAME, so the coverage gap cannot
# be closed silently by adding a marker somewhere.
SubjectTable='
fastcache-compile-node|src/apps/fastcache-compile-node/NodeConfig.cpp
fastcached|src/FastCache/Config/CliParser.cpp
'

# The option table a subject is checked against.
# @param 1 The subject.
# @return The path, relative to the tree root, or nothing when this check knows no such subject.
table_for_subject() {
    local name path
    while IFS='|' read -r name path; do
        [[ -n "$name" ]] || continue
        if [[ "$name" == "$1" ]]; then
            printf '%s' "$path"
            return 0
        fi
    done <<< "$SubjectTable"
}

# Every subject this check knows, for a refusal to name.
# @return The subjects, space separated.
known_subjects() {
    local name path out=""
    while IFS='|' read -r name path; do
        [[ -n "$name" ]] || continue
        out="${out}${out:+, }${name}"
    done <<< "$SubjectTable"
    printf '%s' "$out"
}

refuse() {
    echo "FAIL node-reloadable-docs: $*" >&2
    # A plain exit, deliberately: nothing here calls this from a subshell, so the
    # `kill -s TERM $$` idiom the e2e fixtures need would only buy an exit status of
    # 143 -- and anything neither 0 nor 1 reads as "something killed this" rather than
    # "the tree is bad", which is the opposite of what a verdict should say.
    exit 1
}

# The truth: every YAML key whose option row is `Reloadable::Yes`.
#
# Walked row by row rather than matched flat. The association between a row's key and
# its reloadable column is the whole question, and a flat scan for `.yamlKey` -- which
# is what `check-node-config-reference.cmake` does for its own, different question --
# cannot express it.
#
# **A row ends where the next one BEGINS, and that is a correction rather than a
# tidy-up** ([#1070](https://github.com/LASTRADA-Software/fastcached/issues/1070)). This
# used to terminate a row on `/^        },$/` -- a closing brace alone at eight spaces,
# which is how `NodeConfig.cpp` lays most of its rows out. `CliParser.cpp` uses the
# compact style, where the open brace shares the `.primary` line and the row closes with a
# trailing ` },` on its last field, so that pattern matches ZERO of its 48 rows: pointed at
# the daemon, the old walk returned an empty set, which line-258 refuses -- correctly, and
# not for the reason a reader would have guessed.
#
# It was also already wrong about the NODE, silently: `NodeConfig.cpp` MIXES the two
# styles (`:716` `{ .primary = "--config",` is compact), so a compact row there that went
# `Reloadable::Yes` would have been invisible to a check that reports a confident count.
# Nothing was wrong today, which is exactly why nobody would have found it.
#
# Keying on `.primary` instead needs no brace at all, and the fields cannot be misread:
# `.yamlKey = "` appears in no other construct, and the `static_assert` lambdas that
# mention the reloadable column spell it `!=` and `==` rather than `= Reloadable::Yes,`.
#
# @param 1 The option table source.
# @return The keys, one per line, unsorted.
reloadable_keys_from_table() {
    awk '
        function emit() {
            if (primary != "" && rel == "Yes" && yaml != "") print yaml
            primary = ""
        }
        /\.primary = "/                   { emit(); primary = $0; yaml = ""; rel = "No" }
        /\.yamlKey = "/                   { match($0, /"[^"]+"/); yaml = substr($0, RSTART + 1, RLENGTH - 2) }
        /\.reloadable = Reloadable::Yes,/ { if (primary != "") rel = "Yes" }
        END                                   { emit() }
    ' "$1"
}

# The keys a docs Reloadable/restart table claims reload.
#
# The cell is the first row under the separator, first column. Keys are the backticked
# tokens in it, so prose in the same cell is not read as a setting.
#
# @param 1 The markdown file.
# @return The keys, one per line, unsorted.
reloadable_keys_from_docs() {
    awk '
        index($0, "| Reloadable | Requires a restart |") { seen = 1; next }
        seen == 1 && /^\|[ :-]*\|/ { seen = 2; next }
        seen == 2 {
            # First column only: everything before the second bar.
            line = $0
            sub(/^\|/, "", line)
            sub(/\|.*$/, "", line)
            while (match(line, /`[a-z_]+`/)) {
                print substr(line, RSTART + 1, RLENGTH - 2)
                line = substr(line, RSTART + RLENGTH)
            }
            exit
        }
    ' "$1"
}

# The number a file states, as a numeral, from a count written in words or digits.
#
# A table rather than a regex over digits: the shipped node configuration spells its
# count as a WORD -- `EIGHT` today, `SEVEN` when this was written -- and a check that
# only understood digits would stop matching the day somebody wrote one, matching
# nothing, which reads as agreement. The daemon's spells `FIVE` (#1070).
#
# @param 1 The word or numeral.
# @return The numeral, or nothing when it is not a number this understands.
numeral_of() {
    local word
    word="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
    case "$word" in
        0|1|2|3|4|5|6|7|8|9|1[0-9]|20) echo "$word" ;;
        zero) echo 0 ;;      one) echo 1 ;;       two) echo 2 ;;
        three) echo 3 ;;     four) echo 4 ;;      five) echo 5 ;;
        six) echo 6 ;;       seven) echo 7 ;;     eight) echo 8 ;;
        nine) echo 9 ;;      ten) echo 10 ;;      eleven) echo 11 ;;
        twelve) echo 12 ;;   thirteen) echo 13 ;; fourteen) echo 14 ;;
        fifteen) echo 15 ;;  sixteen) echo 16 ;;  seventeen) echo 17 ;;
        eighteen) echo 18 ;; nineteen) echo 19 ;; twenty) echo 20 ;;
        *) ;;
    esac
}

# The count a file states, as a numeral.
# @param 1 The file.
# @return The numeral, or nothing when the sentence is there and the number is not.
stated_count_of() {
    local word
    word="$(awk -v marker="settings are reloadable" '
        {
            where = index($0, marker)
            if (where > 0) {
                before = substr($0, 1, where - 1)
                n = split(before, parts, /[ \t]+/)
                # The LAST NON-EMPTY field. A trailing separator makes `parts[n]`
                # empty, so the check refused a correct file for stating no number.
                # Found by running it against the shipped reference, whose line reads
                # `# SEVEN settings are reloadable ...` -- one space before the marker.
                for (i = n; i >= 1; i--) {
                    if (parts[i] != "") { print parts[i]; break }
                }
                exit
            }
        }
    ' "$1")"
    [[ -n "$word" ]] || return 0
    numeral_of "$word"
}

# Which binary a file says it is describing.
# @param 1 The file.
# @return The subject, or nothing when the file declares none.
subject_of() {
    awk -v marker="reloadable-for:" '
        {
            where = index($0, marker)
            if (where > 0) {
                rest = substr($0, where + length(marker))
                n = split(rest, parts, /[ \t]+/)
                for (i = 1; i <= n; i++) {
                    if (parts[i] != "") {
                        gsub(/[^A-Za-z0-9._-]/, "", parts[i])
                        if (parts[i] != "") { print parts[i]; exit }
                    }
                }
                exit
            }
        }
    ' "$1"
}

# Every tracked file carrying a marker.
# @param 1 The source directory. @param 2 The marker.
# @return The paths, one per line, relative to the source directory.
files_carrying() {
    local root="$1" marker="$2" tracked f
    local -a existing
    existing=()
    tracked="$(git -C "$root" ls-files 2>/dev/null)" || return 0
    [[ -n "$tracked" ]] || return 0

    # The eligibility pass is BUILTINS ONLY -- `[[ -f ]]` and `case` fork nothing -- so
    # the whole tracked set is filtered without spending a process on it. A herestring
    # rather than a pipe, because a `while` on the right of a pipe runs in a subshell
    # and the array would not survive it.
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        [[ -f "${root}/${f}" ]] || continue
        case "$f" in
            scripts/check-node-reloadable-docs.sh) continue ;;
        esac
        existing+=("$f")
    done <<< "$tracked"

    # Guarded before expanding: `"${arr[@]}"` on an empty array is an error under
    # `set -u` on bash 3.2, and it would surface as this helper failing rather than as
    # the tree carrying nothing (#793).
    [[ ${#existing[@]} -gt 0 ]] || return 0

    # ONE `grep -l` across the set instead of one `grep` per file. This used to spend a
    # process on every tracked file -- about a thousand of them -- for a question a
    # single grep answers, which is what put its sibling scan over a CI budget on a host
    # where a fork is tens of milliseconds.
    #
    # `xargs -0` rather than passing the list directly: a thousand paths is tens of
    # kilobytes of command line, and Git Bash's limit is far lower than Linux's, so the
    # one platform this is being made fast for is the one that would refuse the argv.
    # NUL-delimited, so a path with a space survives; `-0` has precedent in
    # `check-tidy-sweep-database.sh`.
    #
    # `-l` alone and never `-lq`: those two flags contradict each other -- one lists
    # files, the other stops at the first match -- and the shell picks one rather than
    # refusing, which is how a scan reports files as lacking a marker they contain.
    #
    # `|| true` because `grep` exits 1 when a chunk has no match, and `pipefail` is on:
    # without it, "nothing in this chunk" would read as the helper failing.
    ( cd "$root" && printf '%s\0' "${existing[@]}" | xargs -0 grep -l -F -- "$marker" 2>/dev/null ) || true
}

# @param 1 The source directory.
check_tree() {
    local root="$1"
    local declaring subject carrying declined f name path mine

    # Scope: every tracked file that DECLARES a subject -- and is this project's own.
    # A vendored file carrying the marker is declined BY NAME rather than read: refused, an
    # upstream sync reddens a check about our documentation; read, it is a claim about our
    # binary written by somebody else.
    carrying="$(files_carrying "$root" "$SubjectMarker")"
    declaring="$(first_party_paths "$root" "$carrying")" \
        || refuse "the third-party roots of ${root} could not be read (the reader says why above), so a vendored file carrying '${SubjectMarker}' cannot be told from a first-party artefact"
    declined="$(third_party_paths "$root" "$carrying")"
    [[ -z "$declined" ]] \
        || echo "node-reloadable-docs: $(third_party_declined_summary "file(s) carrying '${SubjectMarker}'" "$declined")"
    [[ -n "$declaring" ]] || refuse "no tracked file carries '${SubjectMarker}'; the artefacts that state a reloadable set declare which binary they describe, and a scan that finds none is describing nothing"

    # Every declared subject must be one this check reads. Refused rather than skipped: a
    # subject nobody handles is a claim nothing verifies, and skipping it silently is how
    # it stays that way -- which was the daemon matrix's position for as long as #1070 was
    # open, honestly out of scope by carrying no marker at all.
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        subject="$(subject_of "${root}/${f}")"
        [[ -n "$subject" ]] || refuse "${f} carries '${SubjectMarker}' and names no binary after it"
        path="$(table_for_subject "$subject")"
        [[ -n "$path" ]] \
            || refuse "${f} declares subject '${subject}', which this check does not read -- it knows $(known_subjects). Add a SubjectTable row naming that binary's option table, or drop the marker; do not leave a declared claim unchecked"
    done <<< "$declaring"

    # Then one pass per subject. Every row must find an artefact: a binary whose matrix has
    # been deleted is the coverage going away, and it looks exactly like a tree with
    # nothing left to check.
    while IFS='|' read -r name path; do
        [[ -n "$name" ]] || continue
        mine=""
        while IFS= read -r f; do
            [[ -n "$f" ]] || continue
            if [[ "$(subject_of "${root}/${f}")" == "$name" ]]; then
                mine="${mine}${f}
"
            fi
        done <<< "$declaring"
        [[ -n "$mine" ]] || refuse "no tracked file declares subject '${name}', which this check has a table row for"
        check_subject "$root" "$name" "$path" "$mine"
    done <<< "$SubjectTable"
}

# Compare one binary's artefacts against one option table.
# @param 1 The source directory.
# @param 2 The subject, which every message this prints names.
# @param 3 The option table, relative to the source directory.
# @param 4 The artefacts declaring that subject, one path per line.
check_subject() {
    local root="$1" subject="$2" table="${1}/$3" mine="$4"
    local truth truthCount docsFiles countFiles f found missing extra stated carries

    [[ -f "$table" ]] || refuse "${subject}: no option table at ${table}"

    truth="$(reloadable_keys_from_table "$table" | sort -u)"
    [[ -n "$truth" ]] || refuse "${subject}: the option table yielded no Reloadable::Yes row with a yamlKey; the scan has stopped matching, so a clean result would describe a table it never read"
    truthCount="$(printf '%s\n' "$truth" | wc -l | tr -d ' ')"

    docsFiles=""
    countFiles=""
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        carries=0
        if grep -F -- "$DocsTableMarker" "${root}/${f}" >/dev/null 2>&1; then
            docsFiles="${docsFiles}${f}
"
            carries=1
        fi
        if grep -F -- "$CountMarker" "${root}/${f}" >/dev/null 2>&1; then
            countFiles="${countFiles}${f}
"
            carries=1
        fi
        # A file that declared a subject and states nothing about it: the marker is
        # decoration, and decoration that looks like coverage is the thing this check
        # exists to stop.
        [[ "$carries" -eq 1 ]] \
            || refuse "${f} declares subject '${subject}' and states no reloadable set and no count"
    done <<< "$mine"

    [[ -n "$docsFiles" ]] || refuse "${subject}: nothing declaring it carries the docs marker '${DocsTableMarker}'"

    # **Every subject states a count, and requiring one of the DAEMON was a decision rather
    # than an inherited default** (#1070). The node states one in
    # `packaging/config/fastcache-compile-node.yaml`. The daemon ships the same kind of
    # file, `packaging/config/fastcached.yaml`, and it carried a hand-typed list AND a
    # hand-typed total -- "The five above are the complete list" -- with nothing deriving
    # either. Exempting the daemon would have left the one artefact an operator actually
    # edits as the one artefact nothing checks, which is the wrong half to skip.
    #
    # So the two halves are checked in two places on purpose: the NAMES through the
    # markdown table, which only the documentation carries, and the TOTAL through this
    # sentence, which only the shipped configuration carries. A binary that shipped no
    # reference configuration would want this clause reconsidered rather than a count
    # invented for it, and that is a row-shaped question for whoever adds one.
    [[ -n "$countFiles" ]] || refuse "${subject}: nothing declaring it carries the count marker '${CountMarker}'"

    echo "node-reloadable-docs[${subject}]: ${truthCount} reloadable setting(s) in the option table"

    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        found="$(reloadable_keys_from_docs "${root}/${f}" | sort -u)"
        [[ -n "$found" ]] || refuse "${f} carries the table marker and its Reloadable cell parsed to nothing"

        missing="$(comm -23 <(printf '%s\n' "$truth") <(printf '%s\n' "$found") | tr '\n' ' ')"
        extra="$(comm -13 <(printf '%s\n' "$truth") <(printf '%s\n' "$found") | tr '\n' ' ')"

        # The direction a reader loses: a setting reloads and the table does not say so.
        [[ -z "${missing// /}" ]] \
            || refuse "${f} does not list ${missing}as reloadable, and the ${subject} option table says they are"

        # The direction an OPERATOR loses, which is why both are checked: the table
        # names a setting as reloadable that is not, so an edit plus SIGHUP is accepted
        # in silence and believed to be in force.
        [[ -z "${extra// /}" ]] \
            || refuse "${f} lists ${extra}as reloadable, and the ${subject} option table says they are NOT -- an operator editing one and reloading is told nothing and gets nothing"

        echo "node-reloadable-docs[${subject}]: ${f} agrees, both directions"
    done <<< "$docsFiles"

    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        stated="$(stated_count_of "${root}/${f}")"
        [[ -n "$stated" ]] \
            || refuse "${f} says '${CountMarker}' and the word before it is not a number this understands"
        [[ "$stated" -eq "$truthCount" ]] \
            || refuse "${f} states ${stated} reloadable setting(s); the ${subject} option table has ${truthCount}"
        echo "node-reloadable-docs[${subject}]: ${f} states ${stated}, which is what the table has"
    done <<< "$countFiles"
}

# ---------------------------------------------------------------------------
# Self-test. A scan nobody has watched refuse is not a guard.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--self-test" ]]; then
    work="$(mktemp -d)"
    trap 'rm -rf "$work"' EXIT
    self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    ran=0
    failed=0

    # @param 1 case, 2 expected exit, 3 phrase the output must carry, 4 tree
    _case() {
        ran=$((ran + 1))
        local out rc=0
        out="$(bash "$self" "$4" 2>&1)" || rc=$?
        if [[ "$rc" != "$2" ]]; then
            echo "FAIL selftest/$1: exit ${rc}, expected $2" >&2
            printf '%s\n' "$out" | sed 's/^/     | /' >&2
            failed=$((failed + 1))
            return
        fi
        case "$out" in
            *"$3"*) ;;
            *)
                echo "FAIL selftest/$1: the output does not carry '$3'" >&2
                printf '%s\n' "$out" | sed 's/^/     | /' >&2
                failed=$((failed + 1))
                ;;
        esac
    }

    # Build ONE binary's three artefacts: an option table, a docs table, a reference
    # configuration stating a count.
    #
    # **The brace STYLE is a parameter, and that is #1070's second finding.** The two real
    # tables are laid out differently -- `NodeConfig.cpp` puts a row's open brace on its
    # own line and closes with `        },`, while `CliParser.cpp` shares the open brace
    # with `.primary` and closes on the last field's line -- and the walk has to read both.
    # A self-test driving only one style is exactly what let the old terminator be wrong
    # about the other, silently, for as long as this ticket was open.
    #
    # @param 1 tree, 2 subject, 3 option table path, 4 docs path, 5 reference config path,
    #        6 reloadable keys for the table, 7 keys the docs list, 8 the stated count,
    #        9 brace style: expanded or compact
    _artefacts() {
        local at="$1" subject="$2" tablePath="$3" docsPath="$4" yamlPath="$5"
        local tableKeys="$6" docsKeys="$7" count="$8" style="$9" k cell=""
        mkdir -p "${at}/$(dirname "$tablePath")" "${at}/$(dirname "$docsPath")" "${at}/$(dirname "$yamlPath")"
        {
            echo 'std::span<OptionSpec const> TheOptions()'
            echo '{'
            for k in $tableKeys; do
                if [[ "$style" == compact ]]; then
                    echo "        { .primary = \"--${k}\","
                    echo "          .yamlKey = \"${k}\","
                    echo '          .reloadable = Reloadable::Yes,'
                    echo "          .same = FieldEq<&Config::${k}>() },"
                else
                    echo '        {'
                    echo "            .primary = \"--${k}\","
                    echo "            .yamlKey = \"${k}\","
                    echo '            .reloadable = Reloadable::Yes,'
                    echo '        },'
                fi
            done
            if [[ "$style" == compact ]]; then
                echo '        { .primary = "--never",'
                echo '          .yamlKey = "never" },'
            else
                echo '        {'
                echo '            .primary = "--never",'
                echo '            .yamlKey = "never",'
                echo '        },'
            fi
            echo '}'
        } > "${at}/${tablePath}"

        for k in $docsKeys; do
            cell="${cell}\`${k}\`, "
        done
        {
            echo '## Reloading it'
            echo ''
            echo "<!-- reloadable-for: ${subject} -->"
            echo ''
            echo '| Reloadable | Requires a restart |'
            echo '|---|---|'
            echo "| ${cell}and nothing else | \`never\` |"
        } > "${at}/${docsPath}"

        {
            echo '# a shipped reference'
            echo "# reloadable-for: ${subject}"
            echo "# ${count} settings are reloadable and the rest are read once at startup."
        } > "${at}/${yamlPath}"
    }

    # Build a tree carrying artefacts for EVERY subject in the table, because every row
    # must find one: a subject with a row and no artefact is refused, and a self-test that
    # built one binary would meet that refusal in every case rather than the one it meant
    # to exercise.
    #
    # The daemon triple defaults to the node's, so a case that only cares about the node
    # passes four arguments exactly as before and the daemon leg agrees quietly.
    #
    # @param 1 tree, 2 node table keys, 3 node docs keys, 4 node count,
    #        5 daemon table keys, 6 daemon docs keys, 7 daemon count -- 5 to 7 optional
    _tree() {
        local at="$1" nodeTable="$2" nodeDocs="$3" nodeCount="$4"
        local dTable="${5-$2}" dDocs="${6-$3}" dCount="${7-$4}"

        _artefacts "$at" fastcache-compile-node \
            src/apps/fastcache-compile-node/NodeConfig.cpp docs/tools/node.md packaging/config/node.yaml \
            "$nodeTable" "$nodeDocs" "$nodeCount" expanded

        _artefacts "$at" fastcached \
            src/FastCache/Config/CliParser.cpp docs/snippets/reload-matrix.md packaging/config/fastcached.yaml \
            "$dTable" "$dDocs" "$dCount" compact

        # The tree states its third-party roots (#1370), or every case is refused for
        # the roots file -- including the ones expecting a refusal for something else.
        mkdir -p "${at}/scripts/lib"
        printf '# planted\nvendor/upstream\n' > "${at}/scripts/lib/third-party-roots.txt"

        ( cd "$at" && git init -q . && git add -A \
            && git -c user.email=t@t -c user.name=t commit -qm t ) >/dev/null 2>&1
    }

    # 1. The control. A check that refuses everything refuses a correct tree too, and
    #    only a case that must PASS can see that.
    _tree "${work}/ok" "log_level fleet_open" "log_level fleet_open" "two"
    _case "control-a-tree-that-agrees" 0 "agrees, both directions" "${work}/ok"

    # 2. The drift that HAS happened: a row added, the docs untouched.
    _tree "${work}/undocumented" "log_level fleet_open requirepass" "log_level fleet_open" "three"
    _case "a-reloadable-row-the-docs-omit" 1 "does not list requirepass" "${work}/undocumented"

    # 3. The direction that costs an OPERATOR rather than a reader, and the one a
    #    check written from the observed drift would not have.
    _tree "${work}/overclaimed" "log_level" "log_level fleet_open" "one"
    _case "a-docs-claim-the-table-denies" 1 "lists fleet_open" "${work}/overclaimed"

    # 4. The stated count, parsed from the table rather than trusted (#780).
    _tree "${work}/miscounted" "log_level fleet_open" "log_level fleet_open" "five"
    _case "a-stated-count-that-is-wrong" 1 "states 5 reloadable setting(s)" "${work}/miscounted"

    # 5. Two empty lists agree perfectly: with no reloadable row the scan matches
    #    nothing, and a clean verdict would describe a table it never read.
    _tree "${work}/norows" "" "log_level" "one"
    _case "no-reloadable-row-at-all-is-refused" 1 "stopped matching" "${work}/norows"

    # 6. And the same for the artefact scan: the marker reworded is the scan silently
    #    covering nothing, which is why the set is derived from the marker.
    _tree "${work}/nomarker" "log_level" "log_level" "one"
    sed -i.bak 's/| Reloadable | Requires a restart |/| Reloads | Needs a restart |/' \
        "${work}/nomarker/docs/tools/node.md" && rm -f "${work}/nomarker/docs/tools/node.md.bak"
    ( cd "${work}/nomarker" && git add -A && git -c user.email=t@t -c user.name=t commit -qm r ) >/dev/null 2>&1
    # The refusal is the per-FILE one rather than the scope one, and that is the better
    # of the two: it names the file that declared a subject and then said nothing about
    # it, where "no tracked file carries the marker" would only say something is absent.
    _case "the-docs-marker-reworded-is-refused" 1 "states no reloadable set and no count" "${work}/nomarker"

    # 7. A count written as a word is read, because the shipped file writes one.
    _tree "${work}/word" "log_level fleet_open requirepass" "log_level fleet_open requirepass" "THREE"
    _case "a-count-spelled-as-a-word" 0 "states 3, which is what the table has" "${work}/word"

    # 8. An artefact describing a binary this check has NO ROW for. This is the case
    #    that found the design: the daemon matrix carried the identical table header and
    #    a different set, so a scan keyed on the header alone reported a correct tree as
    #    broken -- which is why a subject is DECLARED and the scope derived from it.
    #
    #    The planted subject used to be `fastcached`, and #1070 is precisely the change
    #    that made `fastcached` a row. Kept rather than deleted with the gap it described:
    #    what it asserts is that an UNHANDLED subject is refused BY NAME, and that has to
    #    keep working for the binary after this one.
    _tree "${work}/otherbinary" "log_level" "log_level" "one"
    {
        echo '<!-- reloadable-for: fastcache-cli -->'
        echo ''
        echo '| Reloadable | Requires a restart |'
        echo '|---|---|'
        echo '| `log_level` | `bind` |'
    } > "${work}/otherbinary/docs/tools/cli.md"
    ( cd "${work}/otherbinary" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm o ) >/dev/null 2>&1
    _case "an-artefact-for-another-binary-is-refused-by-name" 1 "declares subject 'fastcache-cli'" "${work}/otherbinary"

    # 9. A subject declared and nothing stated: the marker is decoration, and decoration
    #    that reads as coverage is what this check exists to stop.
    _tree "${work}/emptyclaim" "log_level" "log_level" "one"
    {
        echo '<!-- reloadable-for: fastcache-compile-node -->'
        echo ''
        echo 'This page says nothing about which settings reload.'
    } > "${work}/emptyclaim/docs/tools/silent.md"
    ( cd "${work}/emptyclaim" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm e ) >/dev/null 2>&1
    _case "a-declared-subject-that-states-nothing" 1 "states no reloadable set and no count" "${work}/emptyclaim"

    # 10. A subject with a TABLE ROW and no artefact. Two empty lists agree perfectly, so
    #     a tree where nothing describes a binary this check knows about is a REFUSAL
    #     rather than a clean run over nothing.
    #
    #     It names the SUBJECT since #1070, where it used to say only that the marker was
    #     absent tree-wide. That is the more useful of the two and it is also the only one
    #     this arrangement reaches: with two rows, stripping one binary's markers leaves
    #     the other's, so the scope scan still finds files. Case 14 reaches the scope arm.
    _tree "${work}/nosubject" "log_level" "log_level" "one"
    sed -i.bak 's/reloadable-for: fastcache-compile-node/describes: something-else-entirely/' \
        "${work}/nosubject/docs/tools/node.md" "${work}/nosubject/packaging/config/node.yaml"
    rm -f "${work}/nosubject/docs/tools/node.md.bak" "${work}/nosubject/packaging/config/node.yaml.bak"
    ( cd "${work}/nosubject" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm n ) >/dev/null 2>&1
    _case "no-artefact-declares-a-subject-with-a-row" 1 "which this check has a table row for" "${work}/nosubject"

    # 11. The scope arm the case above no longer reaches: files declaring the subject
    #     exist and NONE of them carries the docs table. Exercised on purpose, because
    #     an arm no case reaches is an arm nobody has watched refuse.
    _tree "${work}/nodocstable" "log_level" "log_level" "one"
    rm -f "${work}/nodocstable/docs/tools/node.md"
    ( cd "${work}/nodocstable" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm d ) >/dev/null 2>&1
    _case "nothing-declaring-the-subject-has-the-table" 1 "carries the docs marker" "${work}/nodocstable"

    # 12. A THIRD-PARTY file declaring a subject this check does not read is declined, by
    #     name (#1370). The same file outside the roots is case 8's refusal, so a check
    #     that failed to decline it refuses this tree: exit 0 is the proof.
    _tree "${work}/vendored" "log_level" "log_level" "one"
    mkdir -p "${work}/vendored/vendor/upstream/docs"
    {
        echo '<!-- reloadable-for: fastcache-cli -->'
        echo '| Reloadable | Requires a restart |'
    } > "${work}/vendored/vendor/upstream/docs/daemon.md"
    ( cd "${work}/vendored" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm v ) >/dev/null 2>&1
    _case "a-third-party-artefact-is-declined-by-name" 0 \
        "declined 1 third-party file(s) carrying 'reloadable-for:' under the roots in scripts/lib/third-party-roots.txt, first vendor/upstream/docs/daemon.md" \
        "${work}/vendored"

    # 13. A roots file naming no root is a refusal, never "nothing is third-party".
    _tree "${work}/noroots" "log_level" "log_level" "one"
    printf '# no root at all\n' > "${work}/noroots/scripts/lib/third-party-roots.txt"
    _case "a-roots-file-naming-no-root-is-refused" 1 "the third-party roots of" "${work}/noroots"

    # 14. The SCOPE arm: no artefact anywhere declares any subject. Reachable only by
    #     stripping EVERY binary's marker, which is why case 10 no longer reaches it --
    #     an arm no case reaches is an arm nobody has watched refuse.
    _tree "${work}/nomarkers" "log_level" "log_level" "one"
    sed -i.bak 's/reloadable-for:/describes:/' \
        "${work}/nomarkers/docs/tools/node.md" "${work}/nomarkers/packaging/config/node.yaml" \
        "${work}/nomarkers/docs/snippets/reload-matrix.md" "${work}/nomarkers/packaging/config/fastcached.yaml"
    find "${work}/nomarkers" -name "*.bak" -delete
    ( cd "${work}/nomarkers" && git add -A \
        && git -c user.email=t@t -c user.name=t commit -qm m ) >/dev/null 2>&1
    _case "nothing-declares-any-subject-at-all" 1 "no tracked file carries" "${work}/nomarkers"

    # 15 and 16. The same two directions as cases 2 and 3, on the DAEMON -- which is the
    #     whole of #1070. A subject present in the table is not a subject compared: the
    #     row could be there with the artefacts never read, and every case above would
    #     still pass. These two fail if the daemon leg does nothing.
    #
    #     They also carry the compact brace style, since `_tree` writes the daemon table
    #     that way. A walk that could only read the expanded style yields an empty set
    #     here and is caught by the control at case 1 rather than by these.
    _tree "${work}/daemonmissing" "log_level" "log_level" "one" "log_level requirepass" "log_level" "two"
    _case "a-daemon-row-the-matrix-omits" 1 "does not list requirepass" "${work}/daemonmissing"

    _tree "${work}/daemonextra" "log_level" "log_level" "one" "log_level" "log_level max_memory" "one"
    _case "a-daemon-matrix-claim-the-table-denies" 1 "lists max_memory" "${work}/daemonextra"

    echo "node-reloadable-docs-selftest: ${ran} case(s) ran, ${failed} failed"
    [[ "$failed" -eq 0 ]] || exit 1
    exit 0
fi

check_tree "${1:-.}"
echo "node-reloadable-docs: every artefact agrees with the option table"
