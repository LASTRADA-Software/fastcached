#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the checks whose SUBJECT is documentation, without a build tree.
#
# Usage:
#   scripts/doc-subject-checks.sh [--source-dir DIR] [--list] [--self-test]
#
#   --list       print the derived set and prove every row is runnable, but run
#                nothing. This is what `ctest -R doc-subject-checks-derivation`
#                drives against the real tree.
#   --self-test  drive every verdict against generated trees and exit.
#
# ## The failure this exists for
#
# Every check whose subject is documentation was skipped on exactly the pull
# requests that change documentation (#687). `scripts/ci-scope.sh` classifies a
# `docs/**`, `.agent/**` or `*.md` change as `code=false`, 36 job conditions in
# `.github/workflows/build.yml` gate their work on `code != 'false'`, and so a
# docs-only pull request runs no `ctest` at all. That is exactly right for the
# compiler jobs and exactly backwards for the handful of checks that exist to
# catch prose drifting away from code -- prose drifts by being EDITED, and a
# prose-only edit is precisely the change they never see.
#
# It is not hypothetical. Both live findings #462 fixed came from doc-side hand
# edits, and the check it added would have been green-by-absence on the very next
# docs pull request.
#
# ## Where the set comes from, and why not from here
#
# The set is the ctest tests LABELLED `docs-subject` in `src/tests/CMakeLists.txt`,
# READ out of that file rather than restated here. A second copy of the list would
# not be a cross-check, it would be a second thing to be wrong -- and it is the
# copy that decides what runs on a docs-only pull request, so a drift would leave
# exactly the intended checks unrun while this script printed a confident count.
# The invocation is read from the same place: the `-D` arguments and the `-P`
# script come out of each test's own `add_test()` block, so a check that grows an
# argument does not silently start running with the wrong one.
#
# The verdict rules are read from there too. `FASTCACHED_SCRIPT_CHECK_FAILED` is
# this tree's one spelling of "a `cmake -P` check failed" -- the verdict is the
# OUTPUT rather than the exit status alone, because the pattern also has to hear a
# check that merely WARNS, and a warning exits 0 on every CMake -- and a check's own
# `SKIP_REGULAR_EXPRESSION`, when it has one, is honoured. Restating either would
# make this script's verdicts drift from ctest's for the same checks.
#
# ## What it refuses, and why each refusal is not a skip
#
# Skipped, absent, unstarted and failed are four states, and an instrument that
# collapses them is #687 one level up -- a doc-check step that runs zero checks
# and reports green.
#
#   * a `docs-subject` label on nothing at all -> REFUSED. A scan matching
#     nothing is the ticket's own acceptance clause.
#   * a selected check whose command still contains an unsubstituted `${...}`
#     after `CMAKE_COMMAND` and `CMAKE_SOURCE_DIR` -> REFUSED BY NAME. Those are
#     build-tree facts (a binary directory, the resolved compiler), so the check
#     is not runnable from a bare checkout. Refusing rather than skipping is what
#     makes the label a decision instead of a wish: label a check this cannot
#     run and the docs job goes red until somebody decides.
#   * a selected check whose script is missing -> REFUSED.
#   * the fail-regex spelling missing from the CMakeLists -> REFUSED, because
#     without it every verdict here would be the exit status, which is 0 for a
#     failing 3.28 `-P` script.
#   * every selected check SKIPPED -> REFUSED. Absence of the negative is not the
#     positive; a run in which nothing failed because nothing ran is not a pass.
#
# ## One check in #687's table is deliberately not labelled
#
# `compile-cache-caveat` takes `FASTCACHED_WORK_DIR`, `FASTCACHED_CXX_COMPILER`,
# `FASTCACHED_MAKE_PROGRAM`, `FASTCACHED_GENERATOR` and `FASTCACHED_MSVC_LIKE` --
# all build-tree facts -- and it CONFIGURES a throwaway project, which is why its
# ctest timeout is 600 s rather than 60. It cannot run from a bare checkout and it
# is not cheap enough to want to. That gap is stated rather than papered over: the
# caveat text it pins is not covered on a docs-only pull request, and the refusal
# above is what stops somebody closing that gap by labelling it and getting a
# silent skip instead.
#
# ## bash 3.2
#
# Registered in the default ctest set, so it runs on macOS's 2007 `/bin/bash`. No
# `mapfile`, no `declare -A`, no `${var^^}`, no `local -n`.

set -euo pipefail

SelfName="doc-subject-checks"
DocSubjectLabel="docs-subject"

# The runners a doc-subject check may be spelled with. A TABLE, because this set
# is read by the resolver AND rendered into the refusal it prints, and a set
# stated twice is how the resolver came to know two spellings while the message
# went on claiming one. Adding a third is adding a word here.
DocSubjectRunners="-P bash"
CMakeListsRelative="src/tests/CMakeLists.txt"
FailRegexVariable="FASTCACHED_SCRIPT_CHECK_FAILED"

Fatal() { echo "${SelfName}: $*" >&2; exit 1; }
Say()   { echo "${SelfName}: $*" >&2; }

# ---------------------------------------------------------------------------
# Extract, from a CMakeLists, one line per registered test:
#
#   name <TAB> labels <TAB> skipRegex <TAB> arg <TAB> arg ...
#
# Indentation is not used as a discriminator anywhere here; the shapes are
# `add_test(`/`set_tests_properties(` blocks closed by a line whose first
# non-blank character is `)`, which is how every registration in this tree is
# written and is robust to a reflow.
#
# TWO passes over the file, because a registration may name a variable the same
# file defines: `sccache-backend-caveat` is invoked with
# `-DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSccacheCaveatBudgetSeconds}`, and
# that variable is deliberately ONE value there serving both the argument and the
# ctest TIMEOUT, so the check's printed headroom is headroom against the number
# ctest enforces. Resolving it here keeps that single source of truth rather than
# putting a second copy of 180 in this script. A single forward pass would work
# today only because the `set()` happens to sit above the `add_test()`, which is
# an accident of layout and not a property.
#
# A `${...}` this table does NOT define is left alone on purpose, so it reaches
# the refusal downstream: `${CMAKE_CURRENT_BINARY_DIR}` is a build-tree fact and
# a check needing one is not runnable from a bare checkout.
EmitRegisteredTests() {
    awk '
        NR == FNR {
            if ($0 ~ /^[ \t]*set\([A-Za-z_][A-Za-z0-9_]*[ \t]/) {
                line = $0
                sub(/^[ \t]*set\(/, "", line)
                sub(/\)[ \t]*$/, "", line)
                key = line
                sub(/[ \t].*$/, "", key)
                value = substr(line, length(key) + 1)
                sub(/^[ \t]+/, "", value); sub(/[ \t]+$/, "", value)
                sub(/^"/, "", value); sub(/"$/, "", value)
                vars[key] = value
            }
            next
        }
        function resolve(token,   key, placeholder) {
            for (key in vars) {
                placeholder = "${" key "}"
                while (index(token, placeholder) > 0) {
                    sub(/\$\{[A-Za-z_][A-Za-z0-9_]*\}/, vars[key], token)
                    if (index(token, placeholder) == 0) break
                }
            }
            return token
        }
        function unquote(s) {
            sub(/^[ \t]*/, "", s); sub(/[ \t]*$/, "", s)
            sub(/^"/, "", s); sub(/"$/, "", s)
            return s
        }
        function flushTest() {
            if (name != "") tests[name] = args
            name = ""; args = ""; inAdd = 0
        }
        /^[ \t]*add_test\(/            { flushTest(); inAdd = 1; next }
        inAdd && /^[ \t]*\)/           { flushTest(); next }
        inAdd && /^[ \t]*NAME[ \t]/    { name = unquote(substr($0, index($0, "NAME") + 4)); next }
        inAdd && /^[ \t]*COMMAND[ \t]/ { line = substr($0, index($0, "COMMAND") + 7)
                                         n = split(line, tok, /[ \t]+/)
                                         for (i = 1; i <= n; i++)
                                             if (tok[i] != "") args = args (args == "" ? "" : "\t") resolve(unquote(tok[i]))
                                         next }
        inAdd                          { line = $0
                                         n = split(line, tok, /[ \t]+/)
                                         for (i = 1; i <= n; i++)
                                             if (tok[i] != "") args = args (args == "" ? "" : "\t") resolve(unquote(tok[i]))
                                         next }

        /^[ \t]*set_tests_properties\(/ { propName = $0
                                          sub(/^[ \t]*set_tests_properties\([ \t]*/, "", propName)
                                          sub(/[ \t]+PROPERTIES.*$/, "", propName)
                                          propName = unquote(propName)
                                          inProps = 1
                                          labels[propName] = ""
                                          skips[propName] = ""
                                          next }
        inProps && /^[ \t]*\)/          { inProps = 0; propName = ""; next }
        inProps && /^[ \t]*LABELS[ \t]/ { labels[propName] = unquote(substr($0, index($0, "LABELS") + 6)); next }
        inProps && /^[ \t]*SKIP_REGULAR_EXPRESSION[ \t]/ {
                                          skips[propName] = unquote(substr($0, index($0, "SKIP_REGULAR_EXPRESSION") + 23)); next }

        END {
            flushTest()
            for (t in tests)
                print t "\t" (t in labels ? labels[t] : "") "\t" (t in skips ? skips[t] : "") "\t" tests[t]
        }
    ' "$1" "$1" | LC_ALL=C sort
}

# The one spelling of "this check failed", read rather than restated.
ReadFailRegex() {
    awk -v want="$FailRegexVariable" '
        $0 ~ ("^[ \t]*set\\(" want "[ \t]") {
            line = $0
            sub("^[ \t]*set\\(" want "[ \t]+", "", line)
            sub(/\)[ \t]*$/, "", line)
            sub(/^"/, "", line); sub(/"$/, "", line)
            print line
            exit
        }
    ' "$1"
}

# ---------------------------------------------------------------------------
Run() {
    local sourceDir="$1" listOnly="$2"
    local cmakeLists="${sourceDir}/${CMakeListsRelative}"

    [[ -f "$cmakeLists" ]] || Fatal "no $CMakeListsRelative under '$sourceDir'; the set of doc-subject checks has no source"

    local failRegex
    failRegex="$(ReadFailRegex "$cmakeLists")"
    [[ -n "$failRegex" ]] \
        || Fatal "$cmakeLists defines no ${FailRegexVariable}; without it a failing check would be judged by its exit status, which a \`cmake -P\` script leaves at 0 on CMake 3.28"
    Say "verdict pattern read from $CMakeListsRelative: /${failRegex}/"

    local cmake="${CMAKE_COMMAND:-cmake}"
    # Demanded up front rather than at the first check that needs it, for the
    # reason the script-existence scan below is: a refusal before anything runs
    # beats a run whose first check happens to be the broken one. The sentence
    # says "some of" because that stopped being "all of" when the resolver
    # learned `bash` -- a message that describes the tool's own scope wrongly is
    # how a reader concludes the wrong thing about what did not run.
    command -v "$cmake" >/dev/null || Fatal "no cmake on PATH, and some ${DocSubjectLabel} checks are \`cmake -P\` scripts; refusing rather than silently doing nothing"

    local selected=0 passed=0 failed=0 skipped=0
    local names="" line name labels skipRegex rest

    while IFS= read -r line; do
        name="${line%%$'\t'*}"
        rest="${line#*$'\t'}"
        labels="${rest%%$'\t'*}"
        rest="${rest#*$'\t'}"
        skipRegex="${rest%%$'\t'*}"
        rest="${rest#*$'\t'}"

        # `;`-separated, as CMake spells a list, and matched on the whole token
        # so `docs-subject-extra` cannot be mistaken for `docs-subject`.
        case ";${labels};" in
            *";${DocSubjectLabel};"*) ;;
            *) continue ;;
        esac

        selected=$((selected + 1))
        names="${names}${name} "

        local args=()
        local token
        while [[ -n "$rest" ]]; do
            token="${rest%%$'\t'*}"
            if [[ "$token" == "$rest" ]]; then rest=""; else rest="${rest#*$'\t'}"; fi
            [[ -n "$token" ]] || continue
            token="${token//\$\{CMAKE_COMMAND\}/$cmake}"
            token="${token//\$\{CMAKE_SOURCE_DIR\}/$sourceDir}"
            case "$token" in
                *'${'*)
                    Fatal "'$name' is labelled ${DocSubjectLabel} but its command still contains '$token' after substitution: that is a build-tree fact, so the check cannot run from a bare checkout. Refused rather than skipped -- a skip here is a check nobody notices stopped running."
                    ;;
            esac
            args+=("$token")
        done

        [[ ${#args[@]} -gt 0 ]] || Fatal "'$name' is labelled ${DocSubjectLabel} and has no COMMAND this could parse"

        # Which script this check RUNS. Named before anything executes, so a
        # missing one is a refusal rather than a run whose first check happens to
        # be the broken one.
        #
        # TWO spellings, `-P <script>` and `bash <script>`, because a doc-subject
        # check is not necessarily a `cmake -P` one. The set was `cmake -P` only
        # and this refusal said so -- which locked out a check whose SUBJECT is
        # prose from the very set that exists to run on prose changes, and #687's
        # whole point is that such a check is skipped on exactly the change it
        # exists to catch. "Write it in CMake instead" is not a neutral
        # alternative: a markdown table is dense with `[#780](...)`, and a
        # `file(STRINGS)` reader merges list elements on brackets -- the hazard
        # `.agent/rules/build-and-toolchain.md` records against six readers.
        local i=0 script="" runner=""
        while [[ $i -lt ${#args[@]} ]]; do
            for runner in $DocSubjectRunners; do
                case "${args[$i]}" in
                    "$runner"|*/"$runner")
                        script="${args[$((i + 1))]:-}"
                        [[ -n "$script" ]] || Fatal "'$name' has a ${args[$i]} with no script after it"
                        [[ -f "$script" ]] || Fatal "'$name' names '$script', which does not exist"
                        ;;
                esac
            done
            i=$((i + 1))
        done
        [[ -n "$script" ]] || Fatal "'$name' is labelled ${DocSubjectLabel} but its command runs none of: ${DocSubjectRunners} -- so nothing here can say what it would execute"

        if [[ "$listOnly" == "yes" ]]; then
            echo "  runnable: $name  ->  $script"
            continue
        fi

        local out status=0
        out="$("${args[@]}" 2>&1)" || status=$?

        # Skip is asked first, matching ctest, so a check that reports a missing
        # prerequisite is not also read as a failure. `[[ =~ ]]` and never
        # `printf | grep -q`: `grep -q` exits at its first match, the producer
        # dies of SIGPIPE, and `pipefail` then reports the PRODUCER's status --
        # a false answer on the SUCCESS path, which this tree has a file about.
        if [[ -n "$skipRegex" ]] && [[ "$out" =~ $skipRegex ]]; then
            skipped=$((skipped + 1))
            echo "  SKIPPED  $name"
            printf '%s\n' "$out" | sed 's/^/      /'
            continue
        fi
        if [[ "$status" -ne 0 ]] || [[ "$out" =~ $failRegex ]]; then
            failed=$((failed + 1))
            echo "  FAILED   $name (exit $status)"
            printf '%s\n' "$out" | sed 's/^/      /'
            continue
        fi
        passed=$((passed + 1))
        echo "  passed   $name"
    done < <(EmitRegisteredTests "$cmakeLists")

    # A scan matching nothing is #687's own acceptance clause, and it is the
    # state this whole script would otherwise report as success.
    [[ "$selected" -gt 0 ]] \
        || Fatal "no test in $CMakeListsRelative carries the '${DocSubjectLabel}' label, so this ran NOTHING. A doc-check step that runs zero checks reports green, which is the defect #687 is about."

    if [[ "$listOnly" == "yes" ]]; then
        Say "$selected check(s) labelled ${DocSubjectLabel}, all runnable from a bare checkout: ${names% }"
        return 0
    fi

    Say "$selected check(s) labelled ${DocSubjectLabel}: passed=$passed failed=$failed skipped=$skipped"

    if [[ "$failed" -eq 0 && "$passed" -eq 0 ]]; then
        Fatal "every selected check SKIPPED and none ran; that is not a clean result, it is no result"
    fi
    if [[ "$failed" -gt 0 ]]; then
        Fatal "$failed doc-subject check(s) failed"
    fi
    echo "${SelfName}: $passed doc-subject check(s) passed${skipped:+ ($skipped skipped)}"
}

# ---------------------------------------------------------------------------
# Self-test: every verdict and every refusal, against generated trees.
#
# Generated rather than produced by editing a copy of the real tree: an edit that
# matches no anchor changes nothing and reports success, which is how three
# separate tools in this repository reported work they had not done. A generated
# fragment cannot fail that way, and each case additionally asserts that its
# staged tree DIFFERS from the baseline, so a knob that does nothing is a failure
# rather than a pass.
SelfTest() {
    local scratch status=0
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    local baseline="${scratch}/baseline.txt"

    # Stage a tree: $1 = directory, $2 = the CMakeLists body, and the check
    # scripts named by it are written by the caller.
    StageTree() {
        mkdir -p "$1/src/tests" "$1/scripts"
        printf '%s\n' "$2" > "$1/src/tests/CMakeLists.txt"
    }

    WriteCheck() {
        # $1 = tree, $2 = script name, $3 = body
        printf '%s\n' "$3" > "$1/scripts/$2"
    }

    # $1 = "skip" to give both labelled checks a SKIP_REGULAR_EXPRESSION.
    #
    # A PARAMETER rather than a `sed` injection, for two reasons of very
    # different strength -- stated separately, because a reader cannot recover
    # which was measured and which was not.
    #
    # REASONED, not measured: `\n` in a sed REPLACEMENT is a GNU extension and
    # POSIX leaves it undefined, so BSD sed -- macOS, where this runs in the
    # default ctest set -- is not obliged to produce a newline. This host has
    # only GNU sed, which honours it even under `--posix` (checked), so the
    # failure could NOT be reproduced here and no claim is made that it happens.
    # The same family as the `sed` range trap `check-tidy-sweep-scope.sh`
    # records, met at the replacement side.
    #
    # MEASURED, and the durable half either way: generating beats editing,
    # because an edit that matches no anchor changes nothing and reports
    # success. `AssertStagedDiffers` now refuses any staged tree identical to
    # the baseline, so a case that stages nothing fails instead of reporting on
    # the baseline while claiming to report on a break. That guard holds however
    # the host's sed behaves, which is why it is the part worth keeping.
    GoodCMakeLists() {
        local skipLine=""
        [[ "${1:-}" == "skip" ]] && skipLine='    SKIP_REGULAR_EXPRESSION "SKIP: "'
        SKIPLINE="$skipLine" awk '{ if ($0 == "@SKIP@") { if (length(ENVIRON["SKIPLINE"])) print ENVIRON["SKIPLINE"] } else print }' <<'CML'
set(FASTCACHED_SCRIPT_CHECK_FAILED "CMake Error|CMake Warning")
add_test(
    NAME "alpha-docs"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/alpha.cmake"
)
set_tests_properties("alpha-docs" PROPERTIES
    FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}"
    LABELS "hygiene;docs-subject"
@SKIP@
    TIMEOUT 60
)
add_test(
    NAME "beta-docs"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/beta.cmake"
)
set_tests_properties("beta-docs" PROPERTIES
    FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}"
    LABELS "hygiene;docs-subject"
@SKIP@
    TIMEOUT 60
)
add_test(
    NAME "gamma-code"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/gamma.cmake"
)
set_tests_properties("gamma-code" PROPERTIES
    FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}"
    LABELS "hygiene"
    TIMEOUT 60
)
CML
    }

    local quiet='cmake_minimum_required(VERSION 3.28)
message(STATUS "all good")'
    local noisy='cmake_minimum_required(VERSION 3.28)
message(FATAL_ERROR "the prose and the table disagree")'
    local skipper='cmake_minimum_required(VERSION 3.28)
message(STATUS "SKIP: no toolchain here")'

    # A failing check the way the CONTRACT allows one to present: the text on
    # stdout and an exit status of ZERO. Not a contrivance -- a check that merely
    # WARNS exits 0 on every CMake while printing `CMake Warning`, and a check that
    # shells out to another `cmake -P` without reading `RESULT_VARIABLE` exits 0
    # with its child's error on the output. That is why this tree reads verdicts
    # from output and why `FASTCACHED_SCRIPT_CHECK_FAILED` exists.
    #
    # It is here because the `noisy` fixture above CANNOT test that rule: measured
    # across six CMake versions (#565), `FATAL_ERROR` exits **1** on all of them,
    # so a verdict judged by the exit status alone passes every case built on
    # `noisy`. This comment attributed the exit-0 shape to CMake 3.28 until that
    # measurement; the fixture was right and only its reason was wrong.
    # Found by deleting the output half of the verdict and watching this
    # self-test stay GREEN -- the mode under test was not the mode in use, which
    # is the defect `.agent/rules/testing.md` records against
    # `check-catch-skip-return-code`. Both fixtures stay: one is what a real
    # check does on this host, the other is what the rule is about.
    local silentlyBad='cmake_minimum_required(VERSION 3.28)
message(STATUS "CMake Error: the prose and the table disagree")'

    # And the mirror, equally untested until the same mutation asked: a non-zero
    # exit with nothing in the output a pattern could match. The verdict is a
    # disjunction, and a case for one arm proves half of it.
    local quietlyBad='cmake_minimum_required(VERSION 3.28)
message(STATUS "nothing to see")
cmake_language(EXIT 3)'

    # A staged tree that is byte-identical to the baseline stages NOTHING, and
    # the case then reports on the baseline while claiming to report on a break.
    # Asserted for every tree that is supposed to differ, because a generator or
    # an editor that quietly did nothing is the failure mode all of these share.
    AssertStagedDiffers() {
        local tree="$1" what="$2"
        if diff -q "${scratch}/t-baseline/src/tests/CMakeLists.txt" \
                   "${tree}/src/tests/CMakeLists.txt" >/dev/null 2>&1; then
            echo "  FAIL  '$what' staged a CMakeLists identical to the baseline; the case stages nothing" >&2
            status=1
        fi
    }

    # Capture, THEN match -- never `Case ... | grep -q`.
    #
    # `grep -q` exits at its FIRST match, which closes the pipe; the producer dies
    # of SIGPIPE; `set -o pipefail` then takes the PRODUCER's status and the
    # pipeline reports failure -- **on the success path**, and only when the
    # producer has not already finished writing. That makes it RACY rather than
    # deterministic: it passes on a quiet box (0 failures in 40 runs, measured
    # here) and fails under load, which is how it survived review and reached CI.
    #
    # Measured in this very file: run 33919403106's `clang-asan-ubsan` leg printed
    #
    #     ok    (want-fail) a check printing the failure text and exiting 0 is FAILED
    #     FAIL  a 3.28-shaped failure was read as a pass
    #
    # -- the case's own verdict says the exit status was RIGHT and the message
    # assertion on the next line is what broke. That contradiction is the
    # signature, and it is worth knowing because the two lines look like they
    # disagree about the same thing and do not.
    #
    # The trap is written out in this file's own header for a different construct,
    # and ten call sites below it committed it anyway. A rule stated in the file
    # that obeys it is not learned by the file that does not -- including when
    # they are the same file.
    Expect() {
        # $1 = needle, $2 = failure message, $3.. = Case arguments
        local needle="$1" failMessage="$2"
        shift 2
        local captured
        captured="$(Case "$@")"
        case "$captured" in
            *"$needle"*) ;;
            *) echo "  FAIL  ${failMessage}" >&2; status=1 ;;
        esac
    }

    Case() {
        # $1 = description, $2 = want-pass|want-fail, $3 = tree
        local what="$1" want="$2" tree="$3" out got=0
        # `bash "$0"`, never a bare `"$0"`: these check scripts are mode 644 in
        # git and ctest runs them as `bash <path>`, so a bare `"$0"` exits 126
        # having run nothing -- and every `want-fail` case then passes because
        # the shell refused rather than because the rule fired. Measured in
        # `check-gated-jobs.sh --self-test`, where it made eight negative cases
        # green while testing nothing.
        out="$(bash "$0" --source-dir "$tree" 2>&1)" || got=$?
        if [[ "$want" == "want-pass" && "$got" -eq 0 ]] || [[ "$want" == "want-fail" && "$got" -ne 0 ]]; then
            # Narration on stderr, the captured output on stdout: every call site
            # pipes or redirects stdout to assert on what the run printed, so an
            # `ok` line written there is a verdict nobody ever sees.
            echo "  ok    ($want) $what" >&2
        else
            echo "  FAIL  ($want, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            status=1
        fi
        printf '%s\n' "$out"
    }

    local tree

    # 1. The baseline. Every other case is evidence only if this passes.
    tree="${scratch}/t-baseline"
    StageTree "$tree" "$(GoodCMakeLists)"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$quiet"
    WriteCheck "$tree" gamma.cmake "$noisy"
    Case "two labelled checks pass; an unlabelled failing one is not run" want-pass "$tree" > "$baseline"
    grep -q "passed=2 failed=0 skipped=0" "$baseline" \
        || { echo "  FAIL  the baseline did not report passed=2" >&2; status=1; }
    grep -q "gamma-code" "$baseline" \
        && { echo "  FAIL  an unlabelled check was run" >&2; status=1; }

    # 2. A labelled check that fails, LOUDLY and with a non-zero status. The
    #    output-not-status rule is proved by the `exits 0` fixture further down;
    #    this case is the ordinary one.
    tree="${scratch}/t-failing"
    StageTree "$tree" "$(GoodCMakeLists)"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$noisy"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "FAILED   beta-docs" "the failing check was not named" "a labelled check emitting CMake Error is FAILED" want-fail "$tree"

    # 2b. The same failure the way the CONTRACT allows one to arrive: the text
    #     printed, the status 0 -- a check that merely WARNS, or one that shells
    #     out to another `cmake -P` without reading RESULT_VARIABLE. This is the
    #     case the verdict's output half exists for, and the only one that can go
    #     red when that half is deleted. (It said "the way CMake 3.28 delivers it"
    #     until #565 measured FATAL_ERROR at exit 1 everywhere.)
    tree="${scratch}/t-silently-bad"
    StageTree "$tree" "$(GoodCMakeLists)"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$silentlyBad"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "FAILED   beta-docs" "a failure that prints and exits 0 was read as a pass" "a check printing the failure text and exiting 0 is FAILED (a warning, or a nested cmake -P)" want-fail "$tree"

    # 2c. And the other arm: a non-zero exit with clean output.
    tree="${scratch}/t-quietly-bad"
    StageTree "$tree" "$(GoodCMakeLists)"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$quietlyBad"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "FAILED   beta-docs (exit 3)" "a non-zero exit was read as a pass" "a check exiting non-zero with clean output is FAILED" want-fail "$tree"

    # 3. No labelled checks at all. #687's own acceptance clause.
    tree="${scratch}/t-nolabel"
    StageTree "$tree" "$(GoodCMakeLists | sed 's/;docs-subject//')"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$quiet"
    WriteCheck "$tree" gamma.cmake "$quiet"
    AssertStagedDiffers "$tree" "no labelled checks"
    Expect "ran NOTHING" "the empty-scan refusal did not say so" "a label matching nothing is REFUSED, not reported clean" want-fail "$tree"

    # 4. A labelled check whose command needs a build tree.
    tree="${scratch}/t-buildtree"
    StageTree "$tree" "$(GoodCMakeLists | sed 's|"-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"|"-DW=${CMAKE_CURRENT_BINARY_DIR}"|')"
    AssertStagedDiffers "$tree" "a build-tree argument"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$quiet"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "CMAKE_CURRENT_BINARY_DIR" "the refusal did not name the argument" "an unsubstitutable build-tree argument is REFUSED BY NAME, not skipped" want-fail "$tree"

    # 5. A labelled check whose script is missing.
    tree="${scratch}/t-missing"
    StageTree "$tree" "$(GoodCMakeLists)"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "beta.cmake" "the refusal did not name the missing script" "a labelled check whose script does not exist is REFUSED" want-fail "$tree"

    # 6. The fail-regex spelling gone. Without it every verdict would be the exit
    #    status, which is 0 for a failing 3.28 `-P` script -- so this case is the
    #    one where a wrong answer looks exactly like a right one.
    tree="${scratch}/t-noregex"
    StageTree "$tree" "$(GoodCMakeLists | grep -v FASTCACHED_SCRIPT_CHECK_FAILED)"
    AssertStagedDiffers "$tree" "no fail regex"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$noisy"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "defines no FASTCACHED_SCRIPT_CHECK_FAILED" "the missing-regex refusal did not say so" "a CMakeLists defining no fail regex is REFUSED" want-fail "$tree"

    # 7. Skipped is its own state, distinct from passed -- and a run in which
    #    EVERY check skipped is refused, because nothing ran.
    tree="${scratch}/t-oneskip"
    StageTree "$tree" "$(GoodCMakeLists skip)"
    AssertStagedDiffers "$tree" "a check with a skip regex"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$skipper"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "passed=1 failed=0 skipped=1" "the skip was not counted separately" "a check matching its SKIP_REGULAR_EXPRESSION is SKIPPED, not passed and not failed" want-pass "$tree"

    tree="${scratch}/t-allskip"
    StageTree "$tree" "$(GoodCMakeLists skip)"
    AssertStagedDiffers "$tree" "every check with a skip regex"
    WriteCheck "$tree" alpha.cmake "$skipper"
    WriteCheck "$tree" beta.cmake "$skipper"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "not a clean result" "the all-skipped refusal did not say so" "every check skipping is REFUSED: nothing failed because nothing ran" want-fail "$tree"

    # 8. A label that merely starts with the right text must not select.
    tree="${scratch}/t-prefix"
    StageTree "$tree" "$(GoodCMakeLists | sed 's/;docs-subject/;docs-subject-later/')"
    AssertStagedDiffers "$tree" "a neighbouring label"
    WriteCheck "$tree" alpha.cmake "$quiet"
    WriteCheck "$tree" beta.cmake "$quiet"
    WriteCheck "$tree" gamma.cmake "$quiet"
    Expect "ran NOTHING" "a neighbouring label was matched" "'docs-subject-later' is not 'docs-subject'" want-fail "$tree"

    # 9. A doc-subject check that is NOT a `cmake -P` script. The set was
    #    `cmake -P` only, which locked out a check whose subject is prose from the
    #    set that runs on prose changes -- #687 arriving one level down, inside
    #    #687's own machinery. A `.sh` check reports by EXIT STATUS rather than by
    #    the output pattern, so both arms are asserted: the disjunction's other
    #    half is what the `quietlyBad` fixture above exists for.
    local bashCML='set(FASTCACHED_SCRIPT_CHECK_FAILED "CMake Error|CMake Warning")
add_test(
    NAME "delta-docs"
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/delta.sh"
)
set_tests_properties("delta-docs" PROPERTIES
    LABELS "hygiene;docs-subject"
    TIMEOUT 60
)'

    tree="${scratch}/t-bash-pass"
    StageTree "$tree" "$bashCML"
    WriteCheck "$tree" delta.sh '#!/bin/bash
echo "table totals: 10 table(s), 3 figure(s) asserted"'
    Expect "passed=1 failed=0 skipped=0" "a bash-command doc-subject check did not run" \
        "a doc-subject check spelled \`bash <script>\` runs and passes" want-pass "$tree"

    tree="${scratch}/t-bash-fail"
    StageTree "$tree" "$bashCML"
    WriteCheck "$tree" delta.sh '#!/bin/bash
echo "the prose says 11 collapses, the table has 12"
exit 1'
    Expect "FAILED   delta-docs (exit 1)" "a failing bash-command check was read as a pass" \
        "a \`bash <script>\` check failing by EXIT STATUS is FAILED" want-fail "$tree"

    # ... and the refusal stays a refusal for a command that is neither. Widening
    # the resolver must not turn "this cannot say what it would execute" into a
    # shrug -- that is the state #687 refuses, and it is the reason the widening
    # names two spellings rather than dropping the requirement.
    tree="${scratch}/t-bash-neither"
    StageTree "$tree" 'set(FASTCACHED_SCRIPT_CHECK_FAILED "CMake Error|CMake Warning")
add_test(
    NAME "epsilon-docs"
    COMMAND echo "nothing to see"
)
set_tests_properties("epsilon-docs" PROPERTIES
    LABELS "hygiene;docs-subject"
    TIMEOUT 60
)'
    Expect "runs none of: -P bash" "the neither-spelling refusal did not name the runner set" \
        "a labelled check running none of the known runners is REFUSED" want-fail "$tree"

    if [[ "$status" -ne 0 ]]; then
        echo "${SelfName}: self-test FAILED" >&2
        exit 1
    fi
    echo "${SelfName}: self-test passed"
}

# ---------------------------------------------------------------------------
sourceDir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
listOnly="no"
selfTest="no"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir) [[ $# -ge 2 ]] || Fatal "--source-dir needs a directory"; sourceDir="$2"; shift 2 ;;
        --list)       listOnly="yes"; shift ;;
        --self-test)  selfTest="yes"; shift ;;
        *)            Fatal "unknown argument '$1'" ;;
    esac
done

if [[ "$selfTest" == "yes" ]]; then
    SelfTest
    exit 0
fi

Run "$sourceDir" "$listOnly"
