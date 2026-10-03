#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every fixture that STARTS `fastcache-compile-node` says which of its default fleet
# features it turns off, and where it keeps its state.
#
# ## Why this exists
#
# A node with no flags is a fleet member: it runs consensus on 6680, beacons discovery on
# 6681, and keeps its identity in the platform's state directory. That is right for a
# machine and wrong for a fixture, in two ways nothing else catches:
#
#   * two fixtures on one host that leave discovery on hear each other's beacons, and a
#     worker that leaves consensus on is a cluster of itself on the shared default port --
#     a cross-talk flake that shows up in somebody else's case;
#   * a node that names no `--cluster-dir` mints its identity into the REAL per-user state
#     directory of whoever runs the suite, and every such node shares it.
#
# The rule is stated in `.agent/rules/consensus-and-cluster.md` ("A fixture written about a
# node running no consensus NAMES `--listen-raft=`"), and a rule stated in the files that
# obey it reaches no file that does not -- so this is the scan.
#
# ## What it looks for, stated as a pattern
#
# A START is the node's variable in COMMAND position in a tracked `scripts/*.sh` or
# `scripts/*.ps1`: `"$node"`, `$NODE`, `${node}` at the start of a command (a line, `$(`,
# `;`, `&&`, `||`, `-- `, after `run_bounded <bound>` or after `NAME=value` prefixes), and
# `$Node` after `&`, `Start-Background` or `-FilePath` in PowerShell. Its STATEMENT is the
# enclosing multi-line `( ... )` when the variable sits inside one (an argv array), else the
# line plus its continuation lines (`\`, or a PowerShell backtick) and any parentheses opened
# on them. Arrays the statement expands (`${name[@]}`, or a PowerShell `$name` assigned an
# `@( ... )`) contribute every assignment's text, one level deep.
#
# Each statement is one of three:
#
#   one-shot   names a verb that answers and exits (`--print-surfaces`, `--cluster-status`,
#              `--install-service`, ... -- `OneShotVerbs` below): nothing bound, nothing
#              written. Passes.
#   mints      names `--print-identity` or `--enroll-from`, which write the identity. Needs
#              `--cluster-dir`.
#   runs       everything else. Needs `--cluster-dir`, and an EMPTY `--listen-raft=` (no
#              consensus, and so no discovery) or an EMPTY `--discovery=` (consensus, but no
#              beacon on the shared port).
#
# ## What it does NOT cover, said plainly, with the direction each fails in
#
#   * A statement's text is a SUPERSET of what one run passes: every assignment of an array
#     counts, including one that empties it on another path, and a parenthesis inside a
#     string can lengthen a statement. More text finds MORE flags, so both fail OPEN -- a
#     start whose flag is only conditionally present passes. The cap (`StatementCap` lines)
#     bounds how far that reaches.
#   * A start through a variable NOT named `node`/`NODE`/`Node` is not seen, which fails
#     open too -- so the scan is paired with a census that fails CLOSED: every script
#     `src/tests/CMakeLists.txt` hands the node binary must show at least one recognised
#     start, or it is refused as "handed the node and started it some way this scan does
#     not know".
#   * A new one-shot verb is refused as a start until it is added to `OneShotVerbs`, which
#     fails closed.
#   * Starts from C++ are out of scope: no Catch2 case spawns the node (every CTest that runs
#     it goes through a script this scan reads).
#
# ## Helpers and exemptions are rows with reasons, and a stale row is refused
#
# `scripts/check-node-fixture-starts-exemptions.txt`, tab-separated:
#
#   helper <TAB> <path> <TAB> <function> <TAB> <reason>
#       The function's CALLS are the starts (each judged as above) and the start inside its
#       body -- whose flags are its callers' `"$@"` -- is not. A row naming a function that
#       has no call, or that the file does not define, is refused as stale.
#   exempt <TAB> <path> <TAB> <text> <TAB> <reason>
#       A start statement containing <text> is excused. A row matching no statement is refused.
#   census <TAB> <path> <TAB> <how it starts the node> <TAB> <reason>
#       A script handed the node whose start this scan cannot recognise, and why that start
#       needs neither flag. Refused as stale once the scan recognises a start there, or once no
#       test hands it the node.
#
# Usage:  check-node-fixture-starts.sh [--self-test | --read-rows < rows]
# Exit:   0 clean, 1 a finding, 2 could not judge (nothing to scan, git unreadable, a table
#         that will not parse).
set -uo pipefail

FastCachedRoot="${FASTCACHED_FIXTURE_STARTS_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FastCachedTable="${FASTCACHED_FIXTURE_STARTS_TABLE:-${FastCachedRoot}/scripts/check-node-fixture-starts-exemptions.txt}"
# shellcheck source=scripts/lib/third-party-roots.sh
. "${FastCachedRoot}/scripts/lib/third-party-roots.sh" \
    || { echo "check-node-fixture-starts: cannot read scripts/lib/third-party-roots.sh" >&2; exit 2; }

# The node's verbs that answer and exit, as the option table spells them. A NAME list rather
# than a pattern over `--cluster-`, because `--cluster-dir` is a setting.
OneShotVerbs="print-surfaces help version cluster-status cluster-set cluster-admit cluster-admit-learner"
OneShotVerbs="${OneShotVerbs} cluster-admit-worker cluster-admit-client cluster-forget cluster-forget-client"
OneShotVerbs="${OneShotVerbs} install-service uninstall-service enroll-list enroll-approve enroll-reject"
OneShotVerbs="${OneShotVerbs} migrate-cache seed-config cordon uncordon"
# The verbs that write this node's identity, and so need `--cluster-dir` and nothing else.
MintingVerbs="print-identity enroll-from"
# How many lines one statement may span, backwards to its opener or forwards to its end.
StatementCap=80

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    selfTestCases=0
    selfTestStatus=0
    me="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    # A scratch REPOSITORY, because the enumeration is `git ls-files`. This script is not
    # staged: its fixtures below spell the very starts it judges, and the real script is run
    # against the staged tree through the root override.
    Stage() {
        rm -rf "$scratch/tree"
        mkdir -p "$scratch/tree/scripts/lib" "$scratch/tree/src/tests"
        cp "${me%/*}/lib/third-party-roots.sh" "$scratch/tree/scripts/lib/"
        cp "${me%/*}/lib/third-party-roots.txt" "$scratch/tree/scripts/lib/"
        : > "$scratch/tree/table.txt"
        printf '%s\n' 'add_test(' '    NAME "fixture"' \
            '    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/fixture.sh"' \
            '        "$<TARGET_FILE:fastcache-compile-node>")' > "$scratch/tree/src/tests/CMakeLists.txt"
        git -C "$scratch/tree" init -q 2>/dev/null
    }
    Track() { git -C "$scratch/tree" add -A -f >/dev/null 2>&1; }

    # @param 1 what is being staged  @param 2 want-pass|want-fail|want-refuse
    Case() {
        local what="$1" want="$2" out got=0
        selfTestCases=$((selfTestCases + 1))
        Track
        out="$(FASTCACHED_FIXTURE_STARTS_ROOT="$scratch/tree" \
               FASTCACHED_FIXTURE_STARTS_TABLE="$scratch/tree/table.txt" \
               bash "$me" 2>&1)" || got=$?
        case "$want" in
            want-pass)   [ "$got" -eq 0 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-fail)   [ "$got" -eq 1 ] && { echo "  ok    ($want) $what"; return; } ;;
            want-refuse) [ "$got" -eq 2 ] && { echo "  ok    ($want) $what"; return; } ;;
            *)           echo "  FAIL  unknown expectation '$want' for: $what" >&2; selfTestStatus=1; return ;;
        esac
        echo "  FAIL  ($want, exit $got) $what" >&2
        printf '%s\n' "$out" | sed 's/^/        /' >&2
        selfTestStatus=1
    }
    Fixture() { printf '%s\n' "$@" > "$scratch/tree/scripts/fixture.sh"; }

    # node-fixture-starts-fixtures: begin -- the lines below PLANT the starts this scan judges,
    # as string literals, so they are a region this file exempts from its own scan. A row
    # exempting the whole file would turn the scan off for the one file whose fixtures are its
    # evidence.
    # The baseline: every refusing case below is evidence only if an ordinary fixture passes.
    Stage
    Fixture 'node="$1"' '"$node" --listen-node=127.0.0.1:1 --listen-raft= --cluster-dir="$work/state" &'
    Case "a worker naming --listen-raft= and --cluster-dir passes" want-pass

    # The arms this scan exists for.
    Stage
    Fixture 'node="$1"' '"$node" --listen-node=127.0.0.1:1 --cluster-dir="$work/state" &'
    Case "a start leaving consensus AND discovery on is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-node=127.0.0.1:1 --listen-raft= &'
    Case "a start keeping its state in the real state directory is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft="127.0.0.1:$raft" --discovery= --cluster-dir="$work/s" &'
    Case "a consensus node naming --discovery= passes" want-pass

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft="127.0.0.1:$raft" --cluster-dir="$work/s" &'
    Case "a consensus node whose --listen-raft has a VALUE is not an empty one, and is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' 'out="$("$node" --print-surfaces 2>&1)"'
    Case "a one-shot verb passes with neither flag" want-pass

    Stage
    Fixture 'node="$1"' 'id="$("$node" --print-identity --listen-raft=)"'
    Case "--print-identity with no --cluster-dir mints into the real directory, and is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' 'out="$(HOME="$w" "$node" --cluster-dir-typo \' '    --listen-raft= 2>&1)"'
    Case "a start behind an environment prefix is still a start, and --cluster-dir-typo is not --cluster-dir... REFUSED" want-fail

    # Resolution: arrays, argv literals, continuations, PowerShell.
    Stage
    Fixture 'node="$1"' 'solo=(--listen-raft=)' 'state=(--cluster-dir="$w/s")' \
        '"$node" ${solo[@]+"${solo[@]}"} "${state[@]}" &'
    Case "flags carried by arrays the start expands are read" want-pass

    Stage
    Fixture 'node="$1"' 'argv=(' '    "$node"' '    --listen-raft="127.0.0.1:$p"' '    --discovery=' \
        '    --cluster-dir="$w/s"' ')' '"${argv[@]}" &'
    Case "a start inside a multi-line argv array is read to the array's end" want-pass

    Stage
    Fixture 'node="$1"' 'argv=(' '    "$node"' '    --listen-raft="127.0.0.1:$p"' \
        '    --cluster-dir="$w/s"' ')' '"${argv[@]}" &'
    Case "the same array without --discovery= is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' '"$activator" --listen=127.0.0.1:1 -- \' '    "$node" \' \
        '        --listen-raft= \' '        --cluster-dir="$w/s" &'
    Case "a start after -- with continuation lines is read to its end" want-pass

    Stage
    printf '%s\n' 'param([string]$Node)' '$p = Start-Background $Node @(' '    "--cluster-dir=$s", "--listen-raft="' \
        ') $log' > "$scratch/tree/scripts/fixture.ps1"
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    Case "a PowerShell start through Start-Background reads its @( ... ) block" want-pass

    Stage
    printf '%s\n' 'param([string]$Node)' '$p = Start-Background $Node @(' '    "--cluster-dir=$s"' \
        ') $log' > "$scratch/tree/scripts/fixture.ps1"
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    Case "the same PowerShell start with neither flag is REFUSED" want-fail

    # Helpers: the calls are the starts.
    Stage
    Fixture 'node="$1"' 'launch() {' '    "$node" "$@" &' '}' 'launch --listen-raft= --cluster-dir="$w/a"' \
        'launch --discovery= --listen-raft=127.0.0.1:9 --cluster-dir="$w/b"'
    printf 'helper\tscripts/fixture.sh\tlaunch\tits callers name the flags\n' > "$scratch/tree/table.txt"
    Case "a helper's calls are judged, each passing" want-pass

    Stage
    Fixture 'node="$1"' 'launch() {' '    "$node" "$@" &' '}' 'launch --listen-raft= --cluster-dir="$w/a"' \
        'launch --cluster-dir="$w/b"'
    printf 'helper\tscripts/fixture.sh\tlaunch\tits callers name the flags\n' > "$scratch/tree/table.txt"
    Case "a helper call naming neither flag is REFUSED" want-fail

    Stage
    Fixture 'node="$1"' 'launch() {' '    "$node" "$@" &' '}' 'launch --listen-raft= --cluster-dir="$w/a"'
    Case "the same helper with no row is judged at its body, and its \"\$@\" is REFUSED" want-fail

    # Exemptions, and their staleness.
    Stage
    Fixture 'node="$1"' '"$node" --scheduler="$ep" "$@"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf 'exempt\tscripts/fixture.sh\t--scheduler="$ep" "$@"\tone-shot cluster verbs from the callers\n' \
        > "$scratch/tree/table.txt"
    Case "an exempt statement passes, so the mechanism works" want-pass

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf 'exempt\tscripts/fixture.sh\tnothing like this\ta row for a start that is gone\n' > "$scratch/tree/table.txt"
    Case "an exemption row that matches no statement is REFUSED as stale" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf 'helper\tscripts/fixture.sh\tlaunch\tno such function\n' > "$scratch/tree/table.txt"
    Case "a helper row naming a function the file does not define is REFUSED as stale" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf 'exempt\tscripts/fixture.sh\n' > "$scratch/tree/table.txt"
    Case "a row with no reason is REFUSED, never read as an exemption" want-refuse

    # ---- the must-NOT-catch half ------------------------------------------
    Stage
    Fixture 'node="$1"' '[[ -n "$node" && -x "$node" ]] || { echo "not found: '"'"'$node'"'"'"; exit 77; }' \
        '# "$node" --listen-node=1 is how this used to be started' \
        '"$node" --listen-raft= --cluster-dir="$w/s" &'
    Case "a test of the path, a message naming it and a COMMENT are not starts" want-pass

    # ---- the census, which is the fail-closed half ------------------------
    Stage
    Fixture 'bin="$1"' '"$bin" --listen-node=127.0.0.1:1 &'
    Case "a script handed the node whose start this scan cannot recognise is REFUSED" want-fail

    # The census reads a registration WHOLE: a wrapper named before the script, and the binary
    # passed through a variable `set()` beforehand, are both how this tree's own tests do it.
    Stage
    Fixture 'bin="$1"' '"$bin" --listen-node=127.0.0.1:1 &'
    printf '%s
' 'set(_node "$<TARGET_FILE:fastcache-compile-node>")' 'add_test(' '    NAME "fixture"'         '    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/fixture.sh"'         '        --node "${_node}")' > "$scratch/tree/src/tests/CMakeLists.txt"
    printf '%s
' 'exec bash "$@"' > "$scratch/tree/scripts/run-check.sh"
    Case "the node passed through a variable, behind a wrapper, still makes its script a subject -- REFUSED" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf '%s
' 'set(_node "$<TARGET_FILE:fastcache-compile-node>")' 'add_test(' '    NAME "fixture"'         '    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/fixture.sh"'         '        --node "${_node}")' > "$scratch/tree/src/tests/CMakeLists.txt"
    printf '%s
' 'exec bash "$@"' > "$scratch/tree/scripts/run-check.sh"
    Case "and the same registration over a recognised start passes, the wrapper not being the subject" want-pass

    Stage
    Fixture 'bin="$1"' '"$bin" --help > "$out"'
    # Beside an ordinary start, or the tree has none and is refused for that instead.
    printf '%s
' 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &' > "$scratch/tree/scripts/other.sh"
    printf 'census\tscripts/fixture.sh\t"$bin" --help\ta one-shot through a variable the scan does not know
'         > "$scratch/tree/table.txt"
    Case "a census row excuses a start the scan cannot see, so the mechanism works" want-pass

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    printf 'census\tscripts/fixture.sh\t"$bin" --help\ta one-shot through a variable the scan does not know
'         > "$scratch/tree/table.txt"
    Case "a census row over a script whose start the scan now reads is REFUSED as stale" want-fail

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    : > "$scratch/tree/src/tests/CMakeLists.txt"
    Case "a tree whose tests hand the node to no script is REFUSED, not read as clean" want-refuse

    Stage
    Fixture 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    rm -rf "$scratch/tree/.git"
    Case "a tree git cannot read is REFUSED, never walked instead" want-refuse

    # A region marker without its closing one is REFUSED, never read as an empty scan:
    # unbalanced, an exempt region runs to the end of the file. The marker is SPLIT across the
    # format and its argument, because spelled whole it would be a real marker in THIS file.
    Stage
    {
        printf '%s\n' 'node="$1"'
        printf '# node-fixture-starts-%s: begin\n' fixtures
        printf '%s\n' '"$node" --listen-raft= --cluster-dir="$w/s" &'
    } > "$scratch/tree/scripts/fixture.sh"
    printf '%s\n' 'node="$1"' '"$node" --listen-raft= --cluster-dir="$w/s" &' > "$scratch/tree/scripts/other.sh"
    Case "a region marker with no closing marker is REFUSED, not read as an empty scan" want-fail
    # node-fixture-starts-fixtures: end

    # The reader, fed rows directly: a kind or a verdict it does not know is REFUSED by name,
    # beside the well-formed row that shows the reader accepts at all.
    Rows() {
        local what="$1" want="$2" out got=0
        shift 2
        selfTestCases=$((selfTestCases + 1))
        out="$(printf '%s\n' "$@" | bash "$me" --read-rows 2>&1)" || got=$?
        if { [ "$want" = want-pass ] && [ "$got" -eq 0 ]; } || { [ "$want" = want-fail ] && [ "$got" -eq 1 ] \
            && grep -q "unrecognised judge row" <<< "$out"; }; then
            echo "  ok    ($want) $what"
            return
        fi
        echo "  FAIL  ($want, exit $got) $what" >&2
        printf '%s\n' "$out" | sed 's/^/        /' >&2
        selfTestStatus=1
    }
    Rows "a well-formed passing start row is read" want-pass "$(printf 'start\tscripts/a.sh\t3\truns\tpass\t')"
    Rows "a start row whose verdict is neither pass nor fail is REFUSED" want-fail \
        "$(printf 'start\tscripts/a.sh\t3\truns\tmaybe\t')"
    Rows "a row of a kind the reader does not know is REFUSED" want-fail "$(printf 'verdict\tscripts/a.sh\t3')"

    echo "check-node-fixture-starts --self-test: ${selfTestCases} case(s) ran"
    if [ "$selfTestStatus" -ne 0 ]; then
        echo "check-node-fixture-starts --self-test: FAILED" >&2
        exit 1
    fi
    echo "check-node-fixture-starts --self-test: every verdict as it must be"
    exit 0
fi

Problems=0
Fail() { echo "  FAIL: $*" >&2; Problems=$((Problems + 1)); }

# Every first-party script a start could be in, one per line, through git.
Subjects() {
    local tracked firstParty
    tracked="$(git -C "${FastCachedRoot}" ls-files -- 'scripts/*.sh' 'scripts/*.ps1')" || return 2
    if [ -z "${tracked}" ]; then
        echo "check-node-fixture-starts: git ls-files named no script under scripts/, which cannot be true." >&2
        echo "  Refused rather than read as 'no fixture starts the node'." >&2
        return 2
    fi
    firstParty="$(first_party_paths "${FastCachedRoot}" "${tracked}")" || {
        echo "check-node-fixture-starts: the third-party roots could not be read. Refused." >&2
        return 2
    }
    printf '%s\n' "${firstParty}"
}

# The scripts `src/tests/CMakeLists.txt` hands the node binary, read per `add_test( ... )` block:
# a block that names `$<TARGET_FILE:fastcache-compile-node>` -- directly, or through a variable
# `set()` to it, which is how `fastcache-cli-e2e` passes it -- hands it to the LAST script path
# the block names, since a wrapper (`run-check.sh`) comes before the script it runs.
HandedTheNode() {
    awk '
        function opens(s,    t) { t = s; return gsub(/\(/, "", t) - gsub(/\)/, "", s) }
        match($0, /set\([A-Za-z0-9_]+[ \t]+"\$<TARGET_FILE:fastcache-compile-node>"/) {
            name = substr($0, RSTART + 4, RLENGTH - 4)
            sub(/[ \t].*$/, "", name)
            nodeVars[name] = 1
        }
        /add_test\(/ { inTest = 1; depth = 0; block = "" }
        inTest {
            block = block " " $0
            depth += opens($0)
            if (depth > 0) next
            inTest = 0
            handed = (block ~ /TARGET_FILE:fastcache-compile-node>/)
            for (name in nodeVars) if (index(block, "${" name "}")) handed = 1
            if (!handed) next
            last = ""
            rest = block
            while (match(rest, /scripts\/[A-Za-z0-9._-]+\.(sh|ps1)/)) {
                last = substr(rest, RSTART, RLENGTH)
                rest = substr(rest, RSTART + RLENGTH)
            }
            if (last != "") print last
        }
    ' "${FastCachedRoot}/src/tests/CMakeLists.txt" 2>/dev/null | sort -u
}

# Judge every start in every subject, in ONE awk pass over all of them -- one process for the
# whole tree rather than several per script and per row, which is what used to spend most of
# this check's TIMEOUT on spawns. Reads the table once, keys each file by `FILENAME`, and
# prints one row per finding, each opening with its KIND:
#   start <TAB> <path> <TAB> <line> <TAB> <class> <TAB> <verdict> <TAB> <what is missing>
#   fail  <TAB> <sentence>
#   judged <TAB> <how many files were judged>
# @param 1 the subjects, newline-separated  @param 2 the scripts handed the node, newline-separated
JudgeAll() {
    local files=() path
    while IFS= read -r path; do
        [ -n "${path}" ] && files+=("${FastCachedRoot}/${path}")
    done < <(printf '%s\n' "$1")
    awk -v root="${FastCachedRoot}/" -v table="${FastCachedTable}" \
        -v subjectList="$(printf '%s\n' "$1" | tr '\n' '\034')" \
        -v handedList="$(printf '%s\n' "$2" | tr '\n' '\034')" \
        -v oneShot="${OneShotVerbs}" -v minting="${MintingVerbs}" -v cap="${StatementCap}" '
        function count(s, c,    t) { t = s; return gsub(c, "", t) }
        function opens(s) { return count(s, "\\(") - count(s, "\\)") }
        function continues(s) {
            if (lang == "sh") return s ~ /\\[ \t]*$/
            return s ~ /`[ \t]*$/
        }
        function isStart(s) {
            if (lang == "sh")
                return s ~ /(^|[;&|(]|\$\(|--[ \t]|run_bounded[ \t]+[^ \t]+[ \t]+|[A-Za-z_][A-Za-z0-9_]*=("[^"]*"|[^ \t"]*)[ \t]+)[ \t]*"?\$\{?(node|NODE|Node)\}?"?([ \t)]|$)/
            return s ~ /(^|[ \t(=])(&|Start-Background|-FilePath)[ \t]+\$Node([^A-Za-z0-9_]|$)/
        }
        function verbIn(text, verbs,    n, v, i) {
            n = split(verbs, v, " ")
            for (i = 1; i <= n; i++)
                if (match(text, "--" v[i] "([^A-Za-z0-9-]|$)")) return 1
            return 0
        }
        # The flag WHOLE -- `=value`, a separate value, or a closing quote after it -- so a
        # longer flag that merely begins with the name is not taken for it.
        function namesStateDir(text) {
            return text ~ /--cluster-dir([= \t"]|$)/
        }
        function empty(text, flag) {
            return match(text, "--" flag "=(\"\"|\047\047)?\"?([ \t),;\\\\`]|$)")
        }
        # The statement a start at line i belongs to, as a line range in lo/hi.
        function statement(i,    j, d) {
            lo = i
            d = 0
            for (j = i - 1; j >= 1 && j >= i - cap; j--) {
                d -= opens(L[j])
                if (d < 0) { lo = j; break }
            }
            d = 0
            for (hi = lo; hi <= N && hi < lo + cap; hi++) {
                d += opens(L[hi])
                if (d <= 0 && !continues(L[hi])) break
            }
            if (hi > N) hi = N
        }
        # Every assignment of @p name in this file, joined.
        function assigned(name,    k, out, d, m) {
            out = ""
            for (k = 1; k <= N; k++) {
                if (lang == "sh") m = L[k] ~ ("(^|[ \t;])(local[ \t]+)?" name "\\+?=\\(")
                else m = L[k] ~ ("^[ \t]*\\$" name "[ \t]*\\+?=[ \t]*@\\(")
                if (!m) continue
                d = 0
                for (; k <= N; k++) {
                    out = out " " L[k]
                    d += opens(L[k])
                    if (d <= 0) break
                }
            }
            return out
        }
        # The text a statement passes: its own, and every array it expands.
        function resolved(text,    rest, name, seen, out) {
            out = text
            rest = text
            while (lang == "sh" ? match(rest, /\$\{[A-Za-z_][A-Za-z0-9_]*\[@\]/) : match(rest, /\$[A-Za-z_][A-Za-z0-9_]*/)) {
                name = substr(rest, RSTART, RLENGTH)
                gsub(/^\$\{?|\[@\]$/, "", name)
                rest = substr(rest, RSTART + RLENGTH)
                if (name == "Node" || (name in seen)) continue
                seen[name] = 1
                out = out " " assigned(name)
            }
            return out
        }
        function start(line, class, verdict, missing) {
            here++
            print "start\t" path "\t" line "\t" class "\t" verdict "\t" missing
        }
        function fail(sentence) { print "fail\t" path ": " sentence }
        function judge(line, text,    k, all, missing) {
            for (k = 1; k <= nExempt; k++)
                if (index(text, E[k])) { used[k] = 1; start(line, "exempt", "pass", ""); return }
            all = resolved(text)
            if (verbIn(all, oneShot)) { start(line, "one-shot", "pass", ""); return }
            if (verbIn(all, minting)) {
                if (namesStateDir(all)) start(line, "mints", "pass", "")
                else start(line, "mints", "fail", "no --cluster-dir")
                return
            }
            missing = ""
            if (!namesStateDir(all)) missing = "no --cluster-dir"
            if (!empty(all, "listen-raft") && !empty(all, "discovery"))
                missing = missing (missing == "" ? "" : ", ") "neither --listen-raft= nor --discovery="
            start(line, "runs", missing == "" ? "pass" : "fail", missing)
        }
        # Everything about the file just read: its starts, its helper and exempt rows, its census.
        function judgeFile(    r, h, i, k, d, isDef, call, text, census) {
            judgedFiles++
            seenPath[path] = 1
            here = 0
            if (lang != "sh" && lang != "ps1") {
                fail("neither a shell nor a PowerShell script, and it was enumerated as one")
                return
            }
            if (begins != ends)
                fail("unbalanced node-fixture-starts-fixtures markers, so an exempt region would run to the end of the file")
            nHelper = 0; nExempt = 0; census = 0
            split("", H); split("", E); split("", used); split("", calls); split("", inBody)
            for (r = 1; r <= nRows; r++) {
                if (RowPath[r] != path) continue
                if (RowKind[r] == "helper") H[++nHelper] = RowText[r]
                else if (RowKind[r] == "exempt") E[++nExempt] = RowText[r]
                else if (RowKind[r] == "census") census = 1
            }
            # Each helper body: its definition line to the brace that closes it.
            for (h = 1; h <= nHelper; h++) {
                defined = 0
                for (i = 1; i <= N; i++) {
                    isDef = (lang == "sh") ? (L[i] ~ ("^[ \t]*(function[ \t]+)?" H[h] "[ \t]*\\(\\)")) \
                                           : (L[i] ~ ("^[ \t]*function[ \t]+" H[h] "([^A-Za-z0-9_-]|$)"))
                    if (!isDef) continue
                    defined = 1
                    d = 0
                    for (k = i; k <= N; k++) {
                        d += count(L[k], "{") - count(L[k], "}")
                        inBody[k] = 1
                        if (d <= 0 && k > i) break
                    }
                }
                if (!defined) fail("a helper row names " H[h] ", which this file does not define (stale row)")
            }
            for (i = 1; i <= N; i++) {
                if (L[i] == "") continue
                if (isStart(L[i]) && !(i in inBody)) {
                    statement(i)
                    text = ""
                    for (k = lo; k <= hi; k++) text = text " " L[k]
                    judge(i, text)
                    i = (hi > i) ? hi : i
                    continue
                }
                for (h = 1; h <= nHelper; h++) {
                    if (i in inBody) break
                    # In COMMAND position only: a word in a message is not a call.
                    call = (lang == "sh") ? ("(^[ \t]*|[;&|][ \t]*|\\$\\([ \t]*)" H[h] "[ \t]") \
                                          : ("(^[ \t]*|=[ \t]*)" H[h] "[ \t]")
                    if (L[i] !~ call) continue
                    calls[h]++
                    statement(i)
                    text = ""
                    for (k = lo; k <= hi; k++) text = text " " L[k]
                    judge(i, text)
                    i = (hi > i) ? hi : i
                    break
                }
            }
            for (h = 1; h <= nHelper; h++)
                if (!calls[h]) fail("a helper row names " H[h] ", which is never called (stale row)")
            for (k = 1; k <= nExempt; k++)
                if (!used[k]) fail("an exempt row (\047" E[k] "\047) matches no start statement (stale row)")
            if ((path in handed) && here == 0) {
                if (!census) {
                    fail("src/tests/CMakeLists.txt hands it the node binary and no start was recognised in it --")
                    print "fail\t  it starts the node some way this scan does not know (a variable not named node/NODE/Node?)"
                }
            } else if (census)
                fail("a census row excuses it, and the scan recognises " here " start(s) there or no test hands it the node (stale row)")
        }
        # A file begins: the one before it is judged, and this one is read from its first line.
        function begin(p) {
            path = p
            lang = (path ~ /\.ps1$/) ? "ps1" : (path ~ /\.sh$/) ? "sh" : ""
            N = 0; begins = 0; ends = 0; inRegion = 0
            split("", L)
        }
        BEGIN {
            nSubjects = split(subjectList, subjects, "\034")
            if (subjects[nSubjects] == "") nSubjects--
            for (s = 1; s <= nSubjects; s++) isSubject[subjects[s]] = 1
            n = split(handedList, list, "\034")
            for (s = 1; s <= n; s++) if (list[s] != "") handed[list[s]] = 1
            while ((getline row < table) > 0) {
                if (row ~ /^[ \t]*(#|$)/) continue
                split(row, f, "\t")
                nRows++
                RowKind[nRows] = f[1]; RowPath[nRows] = f[2]; RowText[nRows] = f[3]
            }
            close(table)
        }
        FNR == 1 {
            if (path != "") judgeFile()
            begin(substr(FILENAME, length(root) + 1))
        }
        # A REGION may exempt itself -- the self-test in this file plants the starts it judges --
        # and its markers must BALANCE, or the region would run silently to the end of the file.
        /node-fixture-starts-fixtures: begin/ { begins++; inRegion = 1; L[++N] = ""; next }
        /node-fixture-starts-fixtures: end/   { ends++; inRegion = 0; L[++N] = ""; next }
        { L[++N] = (inRegion || $0 ~ /^[ \t]*#/) ? "" : $0 }
        END {
            if (path != "") judgeFile()
            # An EMPTY file has no first line, so no rule above saw it: judged here, as nothing.
            for (s = 1; s <= nSubjects; s++)
                if (!(subjects[s] in seenPath)) { begin(subjects[s]); judgeFile() }
            # Table rows for files that are not subjects at all.
            for (r = 1; r <= nRows; r++)
                if (!(RowPath[r] in isSubject))
                    print "fail\ta " RowKind[r] " row names " RowPath[r] ", which is not a tracked script (stale row)"
            print "judged\t" judgedFiles
        }
    ' "${files[@]}"
}

# Read the judge's rows and count what they say. Every row opens with its KIND, and a verdict is
# `pass` or `fail`: anything else is refused BY NAME, because a `case` with a `*)` that quietly
# counts what it does not recognise is an unguarded table -- a row kind or a verdict the judge
# grows would pass unexamined. Splits with parameter expansion, never `cut` per field, and
# never `read` with a tab IFS, which collapses empty fields.
# Sets `starts` and `judgedFiles`; findings go through `Fail`.
ReadJudgement() {
    local row kind rest where line class verdict missing
    starts=0
    judgedFiles=""
    while IFS= read -r row; do
        [ -n "${row}" ] || continue
        kind="${row%%$'\t'*}"
        rest="${row#*$'\t'}"
        case "${kind}" in
            start)
                where="${rest%%$'\t'*}"; rest="${rest#*$'\t'}"
                line="${rest%%$'\t'*}"; rest="${rest#*$'\t'}"
                class="${rest%%$'\t'*}"; rest="${rest#*$'\t'}"
                verdict="${rest%%$'\t'*}"; missing="${rest#*$'\t'}"
                starts=$((starts + 1))
                case "${verdict}" in
                    pass) ;;
                    fail) Fail "${where}:${line}: starts the node (${class}) with ${missing}" ;;
                    *) Fail "${where}:${line}: a judge row whose verdict is '${verdict}', neither pass nor fail (unrecognised judge row)" ;;
                esac ;;
            fail) Fail "${rest}" ;;
            judged) judgedFiles="${rest}" ;;
            *) Fail "an unrecognised judge row, kind '${kind}': ${row}" ;;
        esac
    done
}

# `--read-rows`: judge rows from stdin and nothing else, so the self-test can hand the reader a
# row the real judge never prints -- which is the only way to watch it REFUSE one.
if [ "${1:-}" = "--read-rows" ]; then
    ReadJudgement
    echo "check-node-fixture-starts --read-rows: ${starts} start(s), ${Problems} finding(s)"
    [ "${Problems}" -eq 0 ] || exit 1
    exit 0
fi

# ---------------------------------------------------------------------------
if [ ! -f "${FastCachedTable}" ]; then
    echo "check-node-fixture-starts: no table at ${FastCachedTable}. A missing table is refused, not read" >&2
    echo "  as an empty one: two empty answers agree perfectly." >&2
    exit 2
fi
if ! awk -F'\t' '/^[ \t]*(#|$)/ { next } NF < 4 || $4 == "" || ($1 != "helper" && $1 != "exempt" && $1 != "census") { bad = 1; print "  row " NR ": " $0 > "/dev/stderr" } END { exit bad }' "${FastCachedTable}"; then
    echo "check-node-fixture-starts: the table has a row that is not <helper|exempt|census> TAB <path> TAB <what> TAB <reason>." >&2
    echo "  A row with no reason is refused rather than read as a permission." >&2
    exit 2
fi

subjects="$(Subjects)" || exit 2
handed="$(HandedTheNode)"
if [ -z "${handed}" ]; then
    echo "check-node-fixture-starts: src/tests/CMakeLists.txt hands the node binary to no script, which" >&2
    echo "  cannot be true of this tree. Refused rather than read as 'no fixture starts the node'." >&2
    exit 2
fi

judgement="$(JudgeAll "${subjects}" "${handed}")" || {
    echo "check-node-fixture-starts: awk could not judge the scripts. Refused rather than read as clean." >&2
    exit 2
}
ReadJudgement < <(printf '%s\n' "${judgement}")
# The judge ends by saying how many files it judged: fewer than were enumerated is a judge that
# stopped early, which must not read as one that judged them all.
subjectCount="$(printf '%s\n' "${subjects}" | grep -c .)"
if [ "${judgedFiles}" != "${subjectCount}" ]; then
    echo "check-node-fixture-starts: the judge reported ${judgedFiles:-no count} of ${subjectCount} script(s)." >&2
    echo "  Refused rather than read as clean." >&2
    exit 2
fi

# Only when nothing else was found: a census finding already names the script it could not
# read, and a refusal here would bury that in a vaguer sentence.
if [ "${starts}" -eq 0 ] && [ "${Problems}" -eq 0 ]; then
    echo "check-node-fixture-starts: no start was recognised anywhere, which cannot be true of a tree whose" >&2
    echo "  tests start the node. Refused rather than read as clean." >&2
    exit 2
fi

if [ "${Problems}" -ne 0 ]; then
    echo "check-node-fixture-starts: ${Problems} finding(s) over ${starts} start(s)." >&2
    echo "  A fixture names --cluster-dir (its own state, never the platform's) and turns off the fleet it" >&2
    echo "  does not test: --listen-raft= for a node running no consensus, --discovery= for one that runs it." >&2
    echo "  See this script's header for what the scan reads, and the exemptions table for helpers." >&2
    exit 1
fi
echo "check-node-fixture-starts: ${starts} start(s) judged, each naming its state directory and the fleet it turns off"
exit 0
