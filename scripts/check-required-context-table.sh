#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every `RequiredContexts=(` declaration says at its own site whether it is THE table
# or a fixture, and says why.
#
# ## The failure this refuses
#
# Four files under `scripts/` declare an array literally named `RequiredContexts=(` and
# only ONE of them is authoritative. Three are self-test fixtures, and the 11-row one in
# `check-merge-group-report.sh` is a strict SUBSET of the live 14-row table whose every
# row is a real context name.
#
# That subset is the dangerous shape, and it is dangerous precisely because nothing about
# it looks wrong: a reader pointed at it checks 11 real contexts, finds 11 of them
# reported and passing, and reports a TRUE statement about a set nobody asked about. It
# fails toward a confident green, and it is short in the direction that cannot be noticed
# -- three fewer contexts than required, all three of them the expensive ones
# (`clang-asan-ubsan`, `clang-tsan`, `Check the PR body's closing keywords`).
#
# It is a live instance, not a hypothetical (#1360): a lane's hand-rolled reader read the
# fixture, reported 11 of 11 as complete, and did it on a pull request that had already
# merged.
#
# ## Why the guard could not live inside a reader
#
# `ci-pr-required.sh` already refuses when not ONE check run matches a required name --
# and that guard cannot catch this, because 11 of 11 matched real contexts. Its actual
# protection is that it pins the SOURCE FILE. So the defect is choosing the wrong file,
# which no amount of care inside a reader detects.
#
# ## Why markers at the SITE rather than a table in here
#
# A table of exempt files in this script and a set of declarations in the tree are two
# copies meant to stay equal, which is the shape `AGENT.md` refuses. It also puts the
# warning where only this file's reader meets it, and the person who needs warning is the
# one who just landed on a fixture by searching for `RequiredContexts=(`. A marker in the
# comment block immediately above the declaration is the only placement that is there
# when they arrive -- and it removes the STALE-row class by construction, because the
# marker travels with the thing it describes.
#
# What remains stale-able is a marker whose declaration has GONE, and rule 6 refuses that.
#
# ## Renaming the fixtures was the cheaper fix and is NOT available
#
# The obvious repair is to stop the fixtures sharing the name. It cannot be done: three
# readers (`ci-pr-required.sh`, `ci-merge-group-report.sh`, `check-gated-jobs.sh`) find
# the table with `/^RequiredContexts=\(/`, so the array NAME is a wire contract between a
# fixture file and the reader under test. Renaming it makes the fixtures test nothing.
# Said here because it reads as the obvious first move and costs a round trip to refute.
#
# ## Scope, stated rather than left to the pattern
#
# Every regular file under `scripts/`, recursively. That is where every declaration and
# every reader lives today. A scan is silent about what it cannot reach and silence reads
# identically to complete coverage (#492), so the file count is printed on every run and
# an empty scan is a REFUSAL rather than a pass.
#
# bash 3.2: macOS ships a 2007 `/bin/bash` and this runs in the default ctest set.
#
# Usage:
#   bash scripts/check-required-context-table.sh [--root <dir>]
#   bash scripts/check-required-context-table.sh --self-test

set -u

Root=""
SelfTest=0

while [ $# -gt 0 ]; do
    case "$1" in
        --root) Root="${2:-}"; shift 2 ;;
        --self-test) SelfTest=1; shift ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

# NOT named `fail`. That is the reserved name of the shared e2e helper and
# `check-e2e-helpers.sh` refuses a second definition of it -- and sourcing the shared one
# is wrong here, because it signals the top-level pid and would make this check exit 143,
# a status that means the instrument failed rather than the tree being bad.
Refuse() {
    printf 'REFUSED: %s\n' "$1" >&2
    exit 2
}

Fail() {
    printf 'FAIL %s\n' "$1" >&2
    Failures=$(( Failures + 1 ))
}

Marker='required-context-table:'

# This file declares the array, spells the marker and names the reader variable, all by
# construction -- it is the check. So it exempts REGIONS and never itself: a file excused
# entirely stops being scanned everywhere else in it too, which is how a check comes to
# wave through the one defect it would have caught in its own source.
#
# A DIFFERENT prefix from `$Marker`, deliberately -- `...-scan:` rather than `...-table:`.
# Sharing the prefix, a region's own begin and end lines would be read as KIND markers,
# and the balance guard would then be reporting on lines it had itself mis-parsed.
RegionBegin='required-context-scan: data-begin'
RegionEnd='required-context-scan: data-end'

# ---------------------------------------------------------------------------
# One awk pass per file, emitting records. Space-separated with the free text LAST, so
# `read -r kind file line rest` cannot lose a field -- `IFS=$'\t' read` does not read TSV
# (tab is IFS whitespace, so an empty field collapses and shifts every field after it).
# No path under `scripts/` contains a space, asserted by the scan itself below.
#
#   DECL    <file> <line> <indent>          a declaration, and how far it is indented
#   HDRMARK <file> <line> <declLine> <kind> <reason...>   a marker in a declaration's header
#   ORPHAN  <file> <line> <kind>            a marker belonging to no declaration
#   ROW     <file> <declLine> <text>        a quoted row of a declaration
#   READER  <file> <line> <path>            a reader's defaulted table path (rule 5)
#
# A DECLARATION is a line whose first non-blank characters are exactly
# `RequiredContexts=(`. That is deliberately BROADER than the readers, which anchor at
# column 0 -- an indented declaration is one `source` can see and the three awk readers
# cannot, so the two disagree about whether it exists at all. Rule 7 refuses it.
#
# A comment line, a `printf` staging the token and a `grep` pattern containing it are all
# NOT declarations, because none of them starts with it. A COMMENT is not a call site.
# ---------------------------------------------------------------------------
# $1: file to scan, $2: path to report it as
ScanFile() {
    awk -v FNAME="$2" -v MARK="$Marker" -v RBEG="$RegionBegin" -v REND="$RegionEnd" '
        # A data REGION is fixture text by construction. Its own begin/end lines are
        # reported so the consumer can check the region BALANCES -- an unclosed
        # `data-begin` silently exempts the rest of the file, which is the one way this
        # mechanism turns into a whole-file excuse.
        index($0, RBEG) > 0 { printf "RGNBEG %s %d\n", FNAME, FNR; inData = 1; next }
        index($0, REND) > 0 { printf "RGNEND %s %d\n", FNAME, FNR; inData = 0; next }
        inData { next }
        # Remember the contiguous comment block ending at the previous line: a
        # declaration'"'"'s HEADER. Contiguous COMMENT lines only -- a blank line or code
        # breaks it -- so the reason may run over several lines on either side of the
        # marker without the marker having to be the adjacent one.
        {
            isComment = ($0 ~ /^[ \t]*#/)
            isDecl    = ($0 ~ /^[ \t]*RequiredContexts=\(/)
            isMark    = (isComment && index($0, MARK) > 0)
        }
        isMark { markLine[++pendingMarks] = FNR; markText[pendingMarks] = $0 }
        isDecl {
            indent = $0
            sub(/RequiredContexts=\(.*$/, "", indent)
            printf "DECL %s %d %d\n", FNAME, FNR, length(indent)
            for (i = 1; i <= pendingMarks; i++) {
                text = markText[i]
                sub(/^[ \t]*#[ \t]*/, "", text)
                sub(MARK "[ \t]*", "", text)
                printf "HDRMARK %s %d %d %s\n", FNAME, markLine[i], FNR, text
            }
            pendingMarks = 0
            inTable = FNR
            next
        }
        # Anything that is not a comment ends the header block, so the markers collected
        # so far belong to no declaration.
        !isComment {
            for (i = 1; i <= pendingMarks; i++) {
                text = markText[i]
                sub(/^[ \t]*#[ \t]*/, "", text)
                sub(MARK "[ \t]*", "", text)
                printf "ORPHAN %s %d %s\n", FNAME, markLine[i], text
            }
            pendingMarks = 0
        }
        inTable && /^[ \t]*\)/ { inTable = 0 }
        inTable && /^[ \t]*"/ {
            row = $0
            sub(/^[ \t]*/, "", row); sub(/[ \t]*$/, "", row)
            printf "ROW %s %d %s\n", FNAME, inTable, row
        }
        # required-context-scan: data-begin -- the two lines below SPELL the reader
        # variable, so this file matches its own rule 5 by construction. A REGION and
        # never the whole file: excused entirely, this script would stop being scanned
        # for every other rule too, including in its own production half.
        {
            if (match($0, /FASTCACHED_REQUIRED_CONTEXTS_FILE:-[^}"'"'"']*/)) {
                d = substr($0, RSTART, RLENGTH)
                sub(/^FASTCACHED_REQUIRED_CONTEXTS_FILE:-/, "", d)
        # required-context-scan: data-end
                printf "READER %s %d %s\n", FNAME, FNR, d
            }
        }
        END {
            for (i = 1; i <= pendingMarks; i++) {
                text = markText[i]
                sub(/^[ \t]*#[ \t]*/, "", text)
                sub(MARK "[ \t]*", "", text)
                printf "ORPHAN %s %d %s\n", FNAME, markLine[i], text
            }
        }
    ' "$1"
}

# $1: tree root. Fills the globals below; refuses rather than returning on a broken scan.
Records=""
ScannedFiles=0
Failures=0

Collect() {
    local tree="$1" dir="$tree/scripts" f rel records=""
    [ -d "$dir" ] || Refuse "no such directory: $dir -- the scan has nothing to read, which is not a pass"

    local listing
    listing="$(find "$dir" -type f 2>/dev/null)"
    [ -n "$listing" ] || Refuse "no regular files under $dir -- an empty scan agrees with every rule"

    local count=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        case "$f" in
            *' '*) Refuse "$f contains a space; this scan's record format cannot carry one" ;;
        esac
        rel="${f#"$tree"/}"
        count=$(( count + 1 ))
        records="${records}$(ScanFile "$f" "$rel")
"
    done <<EOF
$listing
EOF

    ScannedFiles="$count"
    Records="$records"
}

# ---------------------------------------------------------------------------
# The rules. Seven, and every one of them can fire -- driven both directions by the
# self-test, because a guard nobody has watched ACCEPT is not known to work either.
# ---------------------------------------------------------------------------
# $1: tree root. Returns 0 clean, 1 on a finding; refuses with 2.
Judge() {
    local tree="$1"
    Collect "$tree"

    local kind file line rest
    local decls="" liveFiles="" declMarks="" rowLines="" readers="" orphans=""
    local regions=""

    while read -r kind file line rest; do
        case "$kind" in
            RGNBEG)  regions="${regions}${file} ${line} begin
" ;;
            RGNEND)  regions="${regions}${file} ${line} end
" ;;
            DECL)    decls="${decls}${file} ${line} ${rest}
" ;;
            HDRMARK) declMarks="${declMarks}${file} ${line} ${rest}
" ;;
            ORPHAN)  orphans="${orphans}${file} ${line} ${rest}
" ;;
            ROW)     rowLines="${rowLines}${file} ${line} ${rest}
" ;;
            READER)  readers="${readers}${file} ${line} ${rest}
" ;;
            '')      ;;
        esac
    done <<EOF
$Records
EOF

    # --- Rule 1: the scan must have found a declaration ---------------------
    #
    # A scan that selects nothing is a refusal. Without this the check is green on a
    # tree where the array has been renamed out from under it, which is the one state
    # that needs saying loudest.
    local declCount=0
    declCount="$(printf '%s' "$decls" | grep -c '[^[:space:]]')" || declCount=0
    if [ "$declCount" -eq 0 ]; then
        Refuse "the scan found no \`RequiredContexts=(\` declaration under $tree/scripts
       ($ScannedFiles file(s) read). A scan that selects nothing is not a pass: either
       the array was renamed -- in which case every reader of it is now reading an empty
       list and reporting every context ABSENT -- or this check's pattern has drifted."
    fi

    # --- Rule 0: every data region closes ----------------------------------
    #
    # Ahead of the rest, because an unbalanced region is not a finding about the tree,
    # it is this scan having read less of the tree than it reported. A `data-begin` with
    # no `data-end` exempts everything after it, in a file that still prints a file
    # count, so the coverage loss is invisible in exactly the way #492 describes.
    local rgFile rgLine rgKind rgDepth rgPrev
    rgPrev=""
    rgDepth=0
    while read -r rgFile rgLine rgKind; do
        [ -n "${rgFile:-}" ] || continue
        if [ "$rgFile" != "$rgPrev" ]; then
            if [ -n "$rgPrev" ] && [ "$rgDepth" -ne 0 ]; then
                Refuse "$rgPrev opens a \`$RegionBegin\` that it never closes.
       Everything after it went unscanned while the file count still counted the file."
            fi
            rgPrev="$rgFile"
            rgDepth=0
        fi
        if [ "$rgKind" = "begin" ]; then
            rgDepth=$(( rgDepth + 1 ))
            [ "$rgDepth" -le 1 ] || Refuse "$rgFile:$rgLine nests \`$RegionBegin\` inside an open region"
        else
            rgDepth=$(( rgDepth - 1 ))
            [ "$rgDepth" -ge 0 ] || Refuse "$rgFile:$rgLine closes a data region that was never opened"
        fi
    done <<EOF
$regions
EOF
    if [ -n "$rgPrev" ] && [ "$rgDepth" -ne 0 ]; then
        Refuse "$rgPrev opens a \`$RegionBegin\` that it never closes.
       Everything after it went unscanned while the file count still counted the file."
    fi

    # --- Rules 3 and 7, per declaration -------------------------------------
    local declFile declLine declIndent markKind markReason marks n
    while read -r declFile declLine declIndent; do
        [ -n "${declFile:-}" ] || continue

        # Rule 7: reader-visible or nothing.
        if [ "$declIndent" -ne 0 ]; then
            Fail "$declFile:$declLine declares \`RequiredContexts=(\` indented by $declIndent column(s).
     \`source\` can see it and the three awk readers -- ci-pr-required.sh,
     ci-merge-group-report.sh, check-gated-jobs.sh -- anchor at column 0 and cannot, so
     the two disagree about whether this declaration exists. Put it at column 0."
        fi

        # Rule 3: exactly one marker in the header, carrying a reason.
        marks="$(MarksFor "$declMarks" "$declFile" "$declLine")"
        n=0
        n="$(printf '%s' "$marks" | grep -c '[^[:space:]]')" || n=0
        if [ "$n" -eq 0 ]; then
            Fail "$declFile:$declLine declares \`RequiredContexts=(\` with no
     \`# $Marker <live|fixture -- why>\` line in the comment block above it.
     Four files in this tree declare this array and only one is authoritative; a reader
     that finds it by searching has no way to tell which one it landed on (#1360). An
     unmarked declaration is refused rather than guessed at: mark it \`live\` if every
     reader is meant to read it, or \`fixture -- <why>\` if it is a stand-in."
            continue
        fi
        if [ "$n" -gt 1 ]; then
            Fail "$declFile:$declLine has $n \`$Marker\` lines in its header block.
     Two markers is two claims about one declaration and nothing says which holds."
            continue
        fi
        read -r markKind markReason <<EOF
$(printf '%s' "$marks" | sed 's/^[^ ]* [^ ]* //')
EOF
        case "$markKind" in
            live)
                liveFiles="${liveFiles}${declFile} ${declLine}
"
                ;;
            fixture)
                # The reason is a forcing function: a marker with no reason spells
                # "forgot" in the vocabulary of "decided".
                #
                # Two ways to have none, and they need one answer: `-- ` with nothing
                # after it (the trim empties it), and a marker carrying no `--` at all
                # (the trim changes nothing, so `trimmed` still equals the original).
                local trimmed
                trimmed="$(printf '%s' "$markReason" | sed 's/^--[[:space:]]*//; s/[[:space:]]*$//')"
                if [ -z "$trimmed" ] || [ "$trimmed" = "$markReason" ]; then
                    Fail "$declFile:$declLine is marked \`fixture\` with no reason after \`--\`.
     Spell it \`# $Marker fixture -- <why this is not the live table>\`. A reason nobody
     had to write reads exactly like one nobody thought about."
                fi
                ;;
            *)
                Fail "$declFile:$declLine carries \`$Marker $markKind\`, which is not a
     kind this check enumerates. The two kinds are \`live\` and \`fixture\`. An
     unenumerated value is refused rather than falling out of the case with nothing
     said -- that arm is where this repository's last several state collapses were
     introduced."
                ;;
        esac
    done <<EOF
$decls
EOF

    # --- Rule 2: exactly one live table -------------------------------------
    local liveCount liveFile
    liveCount="$(printf '%s' "$liveFiles" | grep -c '[^[:space:]]')" || liveCount=0
    if [ "$liveCount" -eq 0 ]; then
        Fail "no declaration is marked \`$Marker live\`, so nothing in this tree says
     which copy of the table is authoritative. Every reader defaults to one file; that
     file is the one to mark."
    elif [ "$liveCount" -gt 1 ]; then
        Fail "$liveCount declarations are marked \`$Marker live\`:
$(printf '%s' "$liveFiles" | sed 's/^/       /')
     Two authorities is no authority. Exactly one declaration is the table."
    fi
    liveFile="$(printf '%s' "$liveFiles" | sed -n '1s/ .*//p')"
    local liveLine
    liveLine="$(printf '%s' "$liveFiles" | sed -n '1s/^[^ ]* //p')"

    # --- Rule 6: a marker whose declaration has gone is STALE ---------------
    #
    # The one staleness class site markers do not remove by construction. An exemption
    # nobody must keep true would wave through the next one.
    local orphFile orphLine orphRest
    while read -r orphFile orphLine orphRest; do
        [ -n "${orphFile:-}" ] || continue
        Fail "$orphFile:$orphLine carries \`$Marker $orphRest\` and no
     \`RequiredContexts=(\` declaration follows it in the same comment block. The marker
     is STALE: the declaration it described has been moved or deleted, and a marker that
     describes nothing is an exemption nobody has to keep true."
    done <<EOF
$orphans
EOF

    # --- Rule 4: a fixture must not be a copy of the live table -------------
    #
    # Two copies meant to stay EQUAL is the shape one-source-of-truth refuses, and
    # `check-merge-group-report.sh`'s own header forbids it in words ("Do not sync it").
    # A fixture that has been synced has stopped being a pinned stand-in without its
    # marker changing, which is this rule's whole subject.
    if [ -n "$liveFile" ]; then
        local liveRows fixFile fixLine fixRows
        liveRows="$(RowsFor "$rowLines" "$liveFile" "$liveLine")"
        while read -r declFile declLine declIndent; do
            [ -n "${declFile:-}" ] || continue
            [ "$declFile" = "$liveFile" ] && [ "$declLine" = "$liveLine" ] && continue
            fixRows="$(RowsFor "$rowLines" "$declFile" "$declLine")"
            [ -n "$fixRows" ] || continue
            if [ "$fixRows" = "$liveRows" ]; then
                Fail "$declFile:$declLine holds the SAME rows as the live table at
     $liveFile:$liveLine. That is two copies meant to stay equal: the next promotion
     moves one and not the other, silently. Read the live table instead of restating it,
     or make the stand-in deliberately different and say so in its marker."
            fi
        done <<EOF
$decls
EOF
    fi

    # --- Rule 5: every reader default names the live file -------------------
    #
    # DERIVED from the marker, never restated here: a path written down in this check is
    # a second place to be wrong when the table moves.
    local rdFile rdLine rdPath rdCount=0
    while read -r rdFile rdLine rdPath; do
        [ -n "${rdFile:-}" ] || continue
        rdCount=$(( rdCount + 1 ))
        [ -n "$liveFile" ] || continue
        if [ "$rdPath" != "$liveFile" ]; then
            Fail "$rdFile:$rdLine defaults \`FASTCACHED_REQUIRED_CONTEXTS_FILE\` to
     \`$rdPath\`, and the declaration marked \`live\` is in \`$liveFile\`. A reader
     pointed at a fixture reports a true statement about a set nobody asked about, and
     every row it checks is real, so nothing looks wrong (#1360)."
        fi
    done <<EOF
$readers
EOF

    printf 'check-required-context-table: %d file(s) scanned, %d declaration(s), %d reader default(s)\n' \
        "$ScannedFiles" "$declCount" "$rdCount"
    if [ -n "$liveFile" ]; then
        printf 'check-required-context-table: live table is %s:%s\n' "$liveFile" "$liveLine"
    fi

    # The positive half of the scan, printed on every run: a reader count of zero means
    # this rule reached nothing, which reads identically to every reader being correct.
    if [ "$rdCount" -eq 0 ]; then
        Refuse "no defaulted reader path was found anywhere under
       $tree/scripts. Rule 5 then checked nothing, and a rule that reached no site reports
       exactly as green as one every site satisfied."
    fi

    [ "$Failures" -eq 0 ] && return 0
    return 1
}

# Markers belonging to the declaration at $2:$3, as `<line> <kind> <reason...>`.
# $1 records, $2 file, $3 declaration line
MarksFor() {
    awk -v F="$2" -v L="$3" '$1 == F && $3 == L { $3 = ""; print }' <<EOF
$1
EOF
}

# The rows of the declaration at $2:$3, one per line, in order.
# $1 records, $2 file, $3 declaration line
RowsFor() {
    awk -v F="$2" -v L="$3" '$1 == F && $2 == L { $1 = ""; $2 = ""; sub(/^[ \t]+/, ""); print }' <<EOF
$1
EOF
}

# ---------------------------------------------------------------------------
# Self-test. Every rule, both directions.
#
# The ACCEPTING direction comes first and is not decoration: a guard that refuses every
# tree passes every negative case, and one stood in this repository for two days (#1031).
# ---------------------------------------------------------------------------
# required-context-scan: data-begin -- every tree staged below carries declarations,
# markers and reader defaults as fixture DATA. A REGION, never this whole file: excused
# entirely, the production half above would stop being scanned by its own rules.
RunSelfTest() {
    local cases=0 fails=0
    # NOT `local`: the EXIT trap fires where a function local is out of scope, and under
    # `set -u` that expansion errors inside the trap's subshell and the parent walks on.
    SelfTestTmp="$(mktemp -d)" || Refuse "mktemp failed"
    trap 'rm -rf "$SelfTestTmp"' EXIT
    local tmp="$SelfTestTmp"

    # A tree with a live table, a fixture, and a reader pointed at the live one.
    # $1 name
    Mk() {
        mkdir -p "$tmp/$1/scripts"
        cat > "$tmp/$1/scripts/live.sh" <<'LIVE'
# required-context-table: live
RequiredContexts=(
    "Alpha|w.yml"
    "Beta|w.yml"
)
LIVE
        cat > "$tmp/$1/scripts/fixture.sh" <<'FIX'
Stage() {
    cat > "$f" <<'REQ'
# required-context-table: fixture -- a pinned subset, so a promotion moves no verdict here
RequiredContexts=(
    "Alpha|w.yml"
)
REQ
}
FIX
        cat > "$tmp/$1/scripts/reader.sh" <<'RD'
Table="${FASTCACHED_REQUIRED_CONTEXTS_FILE:-scripts/live.sh}"
RD
    }

    # $1 what, $2 tree, $3 expected exit, $4 a substring the output must carry
    Case() {
        local what="$1" tree="$2" want="$3" text="$4" out got=0
        cases=$(( cases + 1 ))
        Failures=0
        out="$(Judge "$tree" 2>&1)" || got=$?
        if [ "$got" != "$want" ]; then
            printf 'FAIL case %d (%s): exit %s, expected %s\n' "$cases" "$what" "$got" "$want"
            printf '%s\n' "$out" | sed 's/^/       /'
            fails=$(( fails + 1 ))
            return
        fi
        # The VERDICT, not the colour. Every refusal below is distinguished by the rule
        # it names: a case asserting only "it failed" passes under any other rule firing,
        # which is how a refusal test comes to assert what both sides produce.
        # `-e` and not a bare pattern: every `wanted` string below is prose, and a
        # markdown-shaped one beginning with `-` is parsed as an option bundle. Without
        # `-e` grep exits 2, which `if ! grep ...` reads as FALSY -- so the assertion
        # silently inverts. And NOT `-e --`: that makes `--` the pattern and the prose
        # the FILE, which is what the first draft of this line did.
        if ! grep -Fqe "$text" <<EOF
$out
EOF
        then
            printf 'FAIL case %d (%s): exit %s as expected but the output does not say why\n' \
                "$cases" "$what" "$got"
            printf '       wanted: %s\n' "$text"
            printf '%s\n' "$out" | sed 's/^/       /'
            fails=$(( fails + 1 ))
            return
        fi
        printf 'ok   case %d: %s\n' "$cases" "$what"
    }

    Mk clean
    Case "a marked live table, a marked fixture and a correct reader all pass" \
        "$tmp/clean" 0 "live table is scripts/live.sh"

    # Rule 3, and the live instance of #1360: a NEW file declaring the array unmarked.
    Mk plant
    cat > "$tmp/plant/scripts/hand-rolled.sh" <<'PLANT'
RequiredContexts=(
    "Alpha|w.yml"
)
PLANT
    Case "an unmarked declaration in a new file is refused BY NAME" \
        "$tmp/plant" 1 "scripts/hand-rolled.sh:1 declares"

    Mk noreason
    cat > "$tmp/noreason/scripts/hand-rolled.sh" <<'NR'
# required-context-table: fixture
RequiredContexts=(
    "Alpha|w.yml"
)
NR
    Case "a fixture marker with no reason is refused" \
        "$tmp/noreason" 1 "marked \`fixture\` with no reason"

    Mk badkind
    cat > "$tmp/badkind/scripts/hand-rolled.sh" <<'BK'
# required-context-table: probably-fine
RequiredContexts=(
    "Alpha|w.yml"
)
BK
    Case "a marker kind this check does not enumerate is refused" \
        "$tmp/badkind" 1 "which is not a"

    # Rule 2, both ways.
    Mk nolive
    sed 's/^# required-context-table: live$/# nothing here/' "$tmp/nolive/scripts/live.sh" \
        > "$tmp/nolive/scripts/live.new" && mv "$tmp/nolive/scripts/live.new" "$tmp/nolive/scripts/live.sh"
    Case "a tree where no declaration is marked live is refused" \
        "$tmp/nolive" 1 "no declaration is marked"

    Mk twolive
    cat > "$tmp/twolive/scripts/other.sh" <<'TL'
# required-context-table: live
RequiredContexts=(
    "Gamma|w.yml"
)
TL
    Case "two live markers are refused" "$tmp/twolive" 1 "declarations are marked"

    # Rule 4: a fixture synced into a copy of the live table.
    Mk synced
    cat > "$tmp/synced/scripts/fixture.sh" <<'SY'
Stage() {
    cat > "$f" <<'REQ'
# required-context-table: fixture -- a pinned subset, so a promotion moves no verdict here
RequiredContexts=(
    "Alpha|w.yml"
    "Beta|w.yml"
)
REQ
}
SY
    Case "a fixture holding the live table's rows verbatim is refused" \
        "$tmp/synced" 1 "holds the SAME rows as the live table"

    # Rule 5: a reader pointed at the fixture -- the defect that opened #1360.
    Mk misread
    cat > "$tmp/misread/scripts/reader.sh" <<'MR'
Table="${FASTCACHED_REQUIRED_CONTEXTS_FILE:-scripts/fixture.sh}"
MR
    Case "a reader defaulting to the fixture is refused" \
        "$tmp/misread" 1 "and the declaration marked"

    # Rule 6: the marker outlived its declaration.
    Mk stale
    cat > "$tmp/stale/scripts/hand-rolled.sh" <<'ST'
# required-context-table: fixture -- a stand-in for the cases below
echo "the declaration this described is gone"
ST
    Case "a marker with no declaration under it is refused as STALE" \
        "$tmp/stale" 1 "is STALE"

    # Rule 7: indented, so `source` sees it and the three awk readers do not.
    Mk indented
    cat > "$tmp/indented/scripts/hand-rolled.sh" <<'IN'
Setup() {
    # required-context-table: fixture -- indented inside a function
    RequiredContexts=(
        "Alpha|w.yml"
    )
}
IN
    Case "an indented declaration is refused" "$tmp/indented" 1 "indented by"

    # Rule 1: the array renamed out from under every reader. Driven through `Case` like
    # every other arm, and that is load-bearing rather than tidiness: `Refuse` ends the
    # PROCESS, so a case calling `Judge` outside a command substitution takes the whole
    # self-test with it -- which is what the first draft of these two cases did. Ten
    # `ok` lines, no summary, exit 2, and three cases that never ran. A self-test that
    # stops early must not look like one that judged something, which is why the
    # summary below prints how many cases RAN.
    mkdir -p "$tmp/gone/scripts"
    printf 'echo nothing declares the array here\n' > "$tmp/gone/scripts/a.sh"
    Case "a tree with no declaration at all REFUSES rather than passing" \
        "$tmp/gone" 2 "the scan found no"

    # And the mirror: declarations but no reader, so rule 5 reached nothing. A rule that
    # reached no site reports exactly as green as one every site satisfied.
    mkdir -p "$tmp/noreader/scripts"
    cat > "$tmp/noreader/scripts/live.sh" <<'NRD'
# required-context-table: live
RequiredContexts=(
    "Alpha|w.yml"
)
NRD
    Case "a tree with no reader default REFUSES, so rule 5 cannot pass vacuously" \
        "$tmp/noreader" 2 "no defaulted reader path was found anywhere under"

    # Rule 0, both directions. A data region is the one mechanism here that can REDUCE
    # coverage, so it gets the same treatment as the rules it exempts: the balanced form
    # must be accepted and the unclosed form refused. Without the accepting arm, a guard
    # that refused every region would pass the refusing case alone.
    # UNQUOTED heredocs, so the markers come from `$RegionBegin`/`$RegionEnd` rather than
    # being spelled again. Two reasons, and the second one is a bug this already caused:
    #
    #   * restated, they are two copies meant to stay equal -- the shape rule 4 refuses;
    #   * spelled LITERALLY they are markers in THIS file, nested inside the region that
    #     wraps this whole function, so the check refused its own source. The self-test
    #     passed throughout, because a staged tree has no outer region -- the mode under
    #     test was not the mode in use, and only the live run caught it.
    Mk balanced
    cat > "$tmp/balanced/scripts/has-region.sh" <<BAL
echo "a region that closes"
# ${RegionBegin}
RequiredContexts=(
    "Alpha|w.yml"
)
# ${RegionEnd}
echo "and scanning resumes here"
BAL
    Case "a BALANCED data region exempts its own contents and nothing else" \
        "$tmp/balanced" 0 "live table is scripts/live.sh"

    Mk unbalanced
    cat > "$tmp/unbalanced/scripts/has-region.sh" <<UNBAL
# ${RegionBegin}
RequiredContexts=(
    "Alpha|w.yml"
)
UNBAL
    Case "an UNCLOSED data region is refused rather than silently exempting the rest" \
        "$tmp/unbalanced" 2 "that it never closes"

    # A COMMENT is not a declaration, and neither is a `printf` staging the token nor a
    # `grep` pattern containing it. All three are in the real tree; two checks here have
    # already matched their own explanatory headers.
    Mk quoted
    cat > "$tmp/quoted/scripts/talks-about-it.sh" <<'QT'
# RequiredContexts=( would be unmarked here, and this line is a comment
printf 'RequiredContexts=(\n)\n' > "$scratch/table.sh"
if grep -q '^RequiredContexts=(' "$Table"; then :; fi
QT
    Case "a comment, a printf and a grep pattern are not declarations" \
        "$tmp/quoted" 0 "live table is scripts/live.sh"

    printf 'self-test: %d cases run\n' "$cases"
    if [ "$fails" -eq 0 ]; then
        printf 'SELFTEST OK\n'
        return 0
    fi
    printf 'SELFTEST FAILED (%d)\n' "$fails"
    return 1
}
# required-context-scan: data-end

if [ "$SelfTest" -eq 1 ]; then
    RunSelfTest
    exit $?
fi

if [ -z "$Root" ]; then
    ScriptDir="$(cd "$(dirname "$0")" && pwd)"
    Root="$(cd "$ScriptDir/.." && pwd)"
fi

Judge "$Root"
rc=$?
if [ "$rc" -ne 0 ]; then
    printf 'FAILED: a `RequiredContexts=(` declaration does not say what it is.\n' >&2
    printf '        Only ONE copy of that table is authoritative and three are fixtures; a\n' >&2
    printf '        reader that lands on the 11-row subset reports 11 of 11 and every row it\n' >&2
    printf '        checked is real, so nothing looks wrong (#1360).\n' >&2
    printf '        This rule covers the DECLARATIONS and the readers that default through\n' >&2
    printf '        `FASTCACHED_REQUIRED_CONTEXTS_FILE`. It does NOT stop a reader that spells\n' >&2
    printf '        a path some other way -- do not read it as proof that no such reader exists.\n' >&2
    exit "$rc"
fi
printf 'OK\n'
