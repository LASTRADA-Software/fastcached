#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every git WRITE a script or test fixture runs goes through the scrub: `scratch_git`
# (`scripts/lib/git-scrub.sh`) in bash, `fastcached_scratch_git` (`scripts/lib/CheckCommon.cmake`)
# in a `cmake -P` script.
#
# ## Why this exists
#
# On 2026-10-02 a WSL `ctest -L hygiene` ran with `GIT_DIR` and `GIT_WORK_TREE` EXPORTED, so
# that WSL git could read a worktree whose `.git` pointer names a Windows path. Every self-test
# that stages a scratch repository inherited them, and its `git init` reinitialised the
# repository they named instead of the scratch tree -- writing `core.worktree` into the
# configuration all of this repository's worktrees share, after which git failed in every one
# of them. The scratch trees were never touched. `scripts/lib/git-scrub-variables.txt`
# names the variables; the helpers remove them from the one command they run.
#
# The helpers fix the sites that use them and say nothing about the next fixture, whose author
# writes `git init` because it is shorter and works on a machine where nothing is exported --
# which is every machine the author tests on. So this is the scan.
#
# ## What it looks for, stated as a pattern
#
# A git invocation at COMMAND position, spelled `git`, `"${GIT_EXECUTABLE}"` or
# `"${FASTCACHED_GIT}"`, after any number of `-C <dir>` / `-c <key=value>` options, whose
# subcommand is one of `WriteSubcommands` below. Command position is the start of a line or a
# `;`, `&`, `|`, `(`, `{` or `!` before it, or the words `if`, `then`, `do`, `else`, `elif`,
# `while`, `until`, `exec`, `command`, `env` or CMake's `COMMAND`, optionally followed by
# assignments or `env`'s `-u NAME` -- which is what keeps `echo "git add failed"` and
# `set(x "git init: ...")` out, and keeps `env -u GIT_DIR git init` in. Full-line comments are stripped
# first, because a comment is not a call site. Searched in every first-party `*.sh`, `*.ps1`
# and `*.cmake` under `scripts/` and `src/tests/`, and `src/tests/CMakeLists.txt`.
#
# ## What it does NOT cover, said plainly
#
# A subcommand held in a variable (`foreach(step IN ITEMS "init;-q" ...)` was one), git spelled
# through any other variable (`"$gitBin" init`), an `execute_process(COMMAND` whose subcommand
# sits on a LATER line than the git, and git reached from Python or C++. Each of those escapes
# the scan and is a false NEGATIVE -- a site that reads as cleared. A `git config --get` is a
# read and is still flagged, which is the false POSITIVE direction: somebody meets it and adds
# an exemption row. An `env -u GIT_DIR git init` written by hand is flagged too, on purpose:
# a pasted scrub carries its own copy of the list, which is what drifted before this existed.
#
# ## Exemptions carry a reason, and a stale row is refused
#
# A script that writes the repository it was ASKED to -- not a scratch one -- legitimately
# inherits the environment, and is exempt by a row of `check-scratch-git-exemptions.txt`
# (`<path><TAB><reason>`). A row that no longer matches anything is refused, because an
# exemption kept past its subject excuses the next site at that path.
#
# ## The self-test drives the incident itself
#
# Beyond the scan's verdicts, `--self-test` exports `GIT_DIR` and `GIT_WORK_TREE` at a SCRATCH
# repository and runs two of the self-tests that wrote the shared configuration that day -- one
# bash, one `cmake -P`, one per helper -- and asserts that repository's configuration and index
# are unchanged. A control first shows the same exported pair DOES rewrite that configuration
# through an unscrubbed `git init`, so the unchanged reading is a finding rather than an
# instrument that cannot see a write. Remove the `unset` from either helper and its case goes
# red.
#
# It also holds the list to its derivation: `git-scrub-variables.txt` must name everything the
# RUNNING git's `rev-parse --local-env-vars` prints, and the names it carries from outside that.
# And it exports `GIT_CONFIG_PARAMETERS` -- configuration every git command obeys -- injecting a
# clean filter whose command writes the victim when a fixture runs `add`, behind its own
# unscrubbed control: drop the name from the list and that case goes red beside the derivation
# case. Those controls spell git RAW, so the self-test scrubs its own environment first and then
# runs itself under an exported GIT_DIR and under GIT_INDEX_FILE alone, each aimed at a second
# scratch victim that must come back untouched.
set -uo pipefail

FastCachedRoot="${FASTCACHED_SCRATCH_GIT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
FastCachedExemptions="${FASTCACHED_SCRATCH_GIT_EXEMPTIONS:-${FastCachedRoot}/scripts/check-scratch-git-exemptions.txt}"
# shellcheck source=scripts/lib/third-party-roots.sh
. "${FastCachedRoot}/scripts/lib/third-party-roots.sh"

# The subcommands that write a repository, its index, its refs or its configuration. Reads
# (`ls-files`, `rev-parse`, `diff`, `log`, `show`, `cat-file`) are not here: under a hijacked
# `GIT_DIR` they answer about the wrong repository and fail a case loudly, which damages nothing.
WriteSubcommands="init|add|commit|config|update-index|update-ref|symbolic-ref|rm|mv|tag|checkout|switch|restore|reset|stash|worktree|apply|am|merge|rebase|branch|notes|cherry-pick|revert|clone|fetch|pull|gc|repack|prune"

# The pattern, built once from its parts so each part can be read on its own.
CommandPosition='(^|[;&|({!]|(^|[^[:alnum:]_])(if|then|do|else|elif|while|until|exec|command|env|COMMAND))'
# What may stand between that position and git and still leave git the command: an
# assignment (`GIT_DIR=x git init`) or `env`'s `-u NAME`. Without it a hand-pasted
# `env -u GIT_DIR git init` would read as no command at all.
CommandPrefix='([[:space:]]*(-u[[:space:]]+[A-Za-z_][A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*))*[[:space:]]*'
GitSpelling='(git|"?\$\{(GIT_EXECUTABLE|FASTCACHED_GIT)\}"?)'
GitOptions='([[:space:]]+-[Cc][[:space:]]+[^[:space:]]+)*'
WritePattern="${CommandPosition}${CommandPrefix}${GitSpelling}${GitOptions}[[:space:]]+(${WriteSubcommands})([[:space:]]|\$|\))"

# ---------------------------------------------------------------------------
if [ "${1:-}" = "--self-test" ]; then
    shift
    # Both overridable so a registration can hand over the build's own; `cmake -P` resolves a
    # bare `git` through PATH, so the default works wherever a shell does.
    SelfTestCmake="${FASTCACHED_CMAKE:-cmake}"
    SelfTestGit="${FASTCACHED_GIT_EXECUTABLE:-git}"
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --cmake) SelfTestCmake="$2"; shift 2 ;;
            --git) SelfTestGit="$2"; shift 2 ;;
            *) echo "check-scratch-git --self-test: unknown argument '$1'" >&2; exit 2 ;;
        esac
    done

    selfTestCases=0
    selfTestStatus=0
    me="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    realRoot="$(cd "${me%/*}/.." && pwd)"
    # shellcheck source=scripts/lib/git-scrub.sh
    . "${realRoot}/scripts/lib/git-scrub.sh"
    # The WHOLE self-test runs scrubbed, as repair-worktree-pointers.sh's does. Its controls
    # spell git raw on purpose and inherit everything they do not set themselves, so an exported
    # GIT_DIR -- or the GIT_INDEX_FILE git hands a pre-commit hook -- would aim a control's write at
    # whatever repository the caller is in. Every row that wants a variable exports it in its own
    # subshell, so the scrub takes nothing away from what a row tests; the staged-tree verdicts
    # then also run against the staged tree, which is what they judge.
    scrub_git_environment || { echo "check-scratch-git --self-test: the git scrub list could not be read, so the self-test was NOT run" >&2; exit 2; }
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    Pass() { echo "  ok    $1"; }
    Miss() {
        echo "  FAIL  $1" >&2
        [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/        /' >&2
        selfTestStatus=1
    }

    # ---- the scan's verdicts, over staged trees -----------------------------
    # This script is NOT staged: its fixtures below build the refused shape at run time, and
    # the REAL script is run against the staged tree through the root override.
    Stage() {
        rm -rf "$scratch/tree"
        mkdir -p "$scratch/tree/scripts/lib" "$scratch/tree/src/tests"
        cp "${realRoot}/scripts/lib/third-party-roots.sh" "$scratch/tree/scripts/lib/"
        cp "${realRoot}/scripts/lib/third-party-roots.txt" "$scratch/tree/scripts/lib/"
        : > "$scratch/tree/exemptions.txt"
        printf '%s\n' '# stages a repository the way every fixture must' \
            'scratch_git -C "$tree" init -q' > "$scratch/tree/scripts/ordinary.sh"
        scratch_git -C "$scratch/tree" init -q 2>/dev/null
    }

    # @param 1 what is being staged  @param 2 want-pass|want-fail|want-refuse
    Case() {
        local what="$1" want="$2" out got=0
        selfTestCases=$((selfTestCases + 1))
        scratch_git -C "$scratch/tree" add -A -f >/dev/null 2>&1
        out="$(FASTCACHED_SCRATCH_GIT_ROOT="$scratch/tree" \
               FASTCACHED_SCRATCH_GIT_EXEMPTIONS="$scratch/tree/exemptions.txt" \
               bash "$me" 2>&1)" || got=$?
        case "$want" in
            want-pass)   [ "$got" -eq 0 ] && { Pass "($want) $what"; return; } ;;
            want-fail)   [ "$got" -eq 1 ] && { Pass "($want) $what"; return; } ;;
            want-refuse) [ "$got" -eq 2 ] && { Pass "($want) $what"; return; } ;;
        esac
        Miss "($want, exit $got) $what" "$out"
    }

    # Plant one line in @p 1. The git word arrives as printf's ARGUMENT, so the refused shape
    # exists only in the staged file and never in this one -- which the real scan reads.
    # @param 1 file under the staged tree  @param 2 the line, with `%s` where git goes
    # @param 3 the git spelling
    Plant() {
        mkdir -p "$(dirname "$scratch/tree/$1")"
        # shellcheck disable=SC2059  # the format IS the fixture
        printf "$2\n" "$3" >> "$scratch/tree/$1"
    }

    Stage
    Case "a fixture spelling git as scratch_git passes" want-pass

    Stage
    Plant scripts/bare.sh '%s -C "$tree" init -q' git
    Case "a bare 'git -C <dir> init' is REFUSED" want-fail

    Stage
    Plant scripts/chained.sh '( cd "$tree" && %s init -q . && scratch_git add -A ) >/dev/null' git
    Case "a 'git init' after '&&' inside a subshell is REFUSED" want-fail

    Stage
    Plant scripts/commit.sh '%s -c user.email=t@t -c user.name=t commit -qm t' git
    Case "a commit behind two '-c' options is REFUSED" want-fail

    Stage
    Plant scripts/check-thing-selftest.cmake 'execute_process(COMMAND "%s" -C "${tree}" add -A OUTPUT_QUIET)' '${GIT_EXECUTABLE}'
    Case "a cmake 'execute_process(COMMAND \"\${GIT_EXECUTABLE}\" ... add' is REFUSED" want-fail

    Stage
    Plant src/tests/CMakeLists.txt 'execute_process(COMMAND "%s" init -q "${tree}")' '${FASTCACHED_GIT}'
    Case "the \${FASTCACHED_GIT} spelling in src/tests/CMakeLists.txt is REFUSED" want-fail

    Stage
    Plant scripts/pasted.sh 'if env -u GIT_DIR %s -C "$tp" init -q; then :; fi' git
    Case "a hand-pasted 'env -u GIT_DIR git init' is REFUSED, not credited as a scrub" want-fail

    # The exemption mechanism, over a planted site, and its stale direction.
    Stage
    Plant scripts/release.sh '%s tag -a "$version" -m "$version"' git
    printf 'scripts/release.sh\ttags the repository it was asked to\n' > "$scratch/tree/exemptions.txt"
    Case "the same shape passes once a row exempts its file" want-pass

    Stage
    printf 'scripts/gone.sh\ta row for a file that is not there\n' > "$scratch/tree/exemptions.txt"
    Case "an exemption row that matches nothing is REFUSED as stale" want-fail

    # ---- the must-NOT-catch half: each a NEAR MISS of the shape -------------
    Stage
    Plant scripts/documented.sh '# a bare %s init here wrote the shared config once' git
    Case "the shape inside a full-line COMMENT passes" want-pass

    Stage
    Plant scripts/message.sh 'echo "the %s init in $tree failed" >&2' git
    Case "the words inside an echoed MESSAGE pass" want-pass

    Stage
    Plant scripts/check-thing-selftest.cmake 'set(gitSetup "%s init: ${gitError}")' git
    Case "the words inside a CMake string pass" want-pass

    Stage
    Plant scripts/reader.sh 'tracked="$(%s -C "$root" ls-files -- "*.sh")"' git
    Case "a READ ('ls-files') passes" want-pass

    Stage
    Plant scripts/initialise.sh '%s -C "$tree" initialise-thing' git
    Case "a subcommand that merely STARTS with a write verb passes" want-pass

    # ---- the enumeration ----------------------------------------------------
    Stage
    rm -f "$scratch/tree/scripts/ordinary.sh"
    rm -rf "$scratch/tree/scripts/lib"
    Case "a tree with no script at all is REFUSED, not read as clean" want-refuse

    Stage
    rm -rf "$scratch/tree/.git"
    Case "a tree git cannot read is REFUSED, never walked instead" want-refuse

    # ---- the incident, driven ----------------------------------------------
    # A VICTIM repository standing in for the one an exported GIT_DIR named that day, and a
    # work tree elsewhere -- the shape that makes `git init` record `core.worktree`.
    victim="$scratch/victim"
    elsewhere="$scratch/elsewhere"
    mkdir -p "$victim" "$elsewhere"
    scratch_git init -q "$victim"
    victimConfig="$(cat "$victim/.git/config")"

    # Puts the victim back as it was read, so each case below is judged on its own run rather
    # than on what an earlier failing case left behind.
    ResetVictim() {
        printf '%s\n' "$victimConfig" > "$victim/.git/config"
        rm -f "$victim/.git/index"
    }

    # Asserts the victim was not written since the reading above.
    # @param 1 what ran  @param 2 what the victim stands for (default: the exported GIT_DIR)
    VictimUntouched() {
        local now whose="${2:-the exported GIT_DIR}"
        now="$(cat "$victim/.git/config")"
        if [ "$now" != "$victimConfig" ]; then
            Miss "$1 WROTE ${whose}'s configuration, which it must leave unchanged" "$(pipe_pair_into "$victimConfig"$'\n' "$now"$'\n' diff /dev/fd/3 -)"
            printf '%s\n' "$victimConfig" > "$victim/.git/config"
            return
        fi
        if [ -e "$victim/.git/index" ]; then
            Miss "$1 STAGED into ${whose}, whose index must stay absent"
            rm -f "$victim/.git/index"
            return
        fi
        Pass "$1 left ${whose}'s configuration unchanged and its index absent"
    }

    # The control. An UNSCRUBBED init under the exported pair must rewrite the victim's
    # configuration, or every reading of 'unchanged' below says nothing. `rawGit` is a variable
    # so that this deliberate write is not a call site the real scan refuses.
    selfTestCases=$((selfTestCases + 1))
    rawGit=git
    mkdir -p "$scratch/control"
    ( cd "$scratch/control" && GIT_DIR="$victim/.git" GIT_WORK_TREE="$elsewhere" "$rawGit" init -q . ) >/dev/null 2>&1
    if [ "$(cat "$victim/.git/config")" != "$victimConfig" ] \
        && grep -q 'worktree' "$victim/.git/config"; then
        Pass "control: an unscrubbed 'git init' under the exported pair DOES write core.worktree into it"
    else
        Miss "control: an unscrubbed 'git init' under the exported pair should have written core.worktree into the victim's configuration, and did not -- so no 'unchanged' below can be believed"
    fi
    printf '%s\n' "$victimConfig" > "$victim/.git/config"

    # The helper on its own: the victim untouched AND the scratch repository actually made, so
    # a helper that ran nothing does not pass.
    selfTestCases=$((selfTestCases + 1)); ResetVictim
    mkdir -p "$scratch/direct"
    ( export GIT_DIR="$victim/.git" GIT_WORK_TREE="$elsewhere"; cd "$scratch/direct" && scratch_git init -q . ) >/dev/null 2>&1
    if [ -d "$scratch/direct/.git" ]; then
        VictimUntouched "scratch_git init"
    else
        Miss "scratch_git init under the exported pair created no repository where it was asked to"
    fi

    # One bash self-test and one cmake -P self-test that wrote the shared configuration that
    # day, run whole under the exported pair. Their OWN verdicts are not asserted: their reads
    # still see the exported GIT_DIR and answer about the victim, which fails them loudly and
    # damages nothing. What is asserted is the victim -- and, first, that the self-test RAN to
    # the line naming itself, because one that never started leaves the victim untouched too.
    # @param 1 what ran  @param 2 the phrase its run must print  @param 3 its output
    DrivenUntouched() {
        if ! grep -Fq -- "$2" <<< "$3"; then
            Miss "$1 did not RUN to the point of printing '$2', so the victim being untouched says nothing" "$3"
            return
        fi
        VictimUntouched "$1"
    }

    selfTestCases=$((selfTestCases + 1)); ResetVictim
    drivenOut="$( export GIT_DIR="$victim/.git" GIT_WORK_TREE="$elsewhere"
                  bash "${realRoot}/scripts/check-workflow-walk-sole.sh" --self-test 2>&1 )"
    DrivenUntouched "check-workflow-walk-sole.sh --self-test (bash, scratch_git)" \
        "check-workflow-walk-sole --self-test: " "$drivenOut"

    # `-f` as well as `-x`: a bare `cmake` names this repository's `cmake/` DIRECTORY when run
    # from its root, and a directory is `-x`.
    selfTestCases=$((selfTestCases + 1)); ResetVictim
    if ! command -v "$SelfTestCmake" >/dev/null 2>&1 \
        && ! { [ -f "$SelfTestCmake" ] && [ -x "$SelfTestCmake" ]; }; then
        Miss "no cmake at '$SelfTestCmake', so the cmake -P half was NOT exercised -- pass --cmake <path>"
    else
        mkdir -p "$scratch/crypto-seam"
        drivenOut="$( export GIT_DIR="$victim/.git" GIT_WORK_TREE="$elsewhere"
                      "$SelfTestCmake" "-DFASTCACHED_SOURCE_DIR=${realRoot}" "-DGIT_EXECUTABLE=${SelfTestGit}" \
                          "-DFASTCACHED_SCRATCH_DIR=$scratch/crypto-seam" \
                          -P "${realRoot}/scripts/check-crypto-seam-selftest.cmake" 2>&1 )"
        DrivenUntouched "check-crypto-seam-selftest.cmake (cmake -P, fastcached_scratch_git)" \
            "crypto-seam-selftest: " "$drivenOut"
    fi

    # ---- the list, against the git that is running --------------------------
    # git-scrub-variables.txt is derived from `git rev-parse --local-env-vars`, so it must name
    # everything the RUNNING git prints: a git that adds a variable is red here rather than an
    # inherited name nobody noticed. GIT_DIR is asked for first, because an empty or failed
    # answer would otherwise read as "nothing missing".
    selfTestCases=$((selfTestCases + 1))
    localVarsStatus=0
    localVars="$( cd "$scratch" && "$SelfTestGit" rev-parse --local-env-vars 2>&1 )" || localVarsStatus=$?
    if [ "$localVarsStatus" -ne 0 ] || ! grep -qx 'GIT_DIR' <<< "$localVars"; then
        Miss "'$SelfTestGit rev-parse --local-env-vars' (exit $localVarsStatus) did not name GIT_DIR, so the list could not be checked against it" "$localVars"
    else
        unlisted=""
        while IFS= read -r name; do
            [ -n "$name" ] || continue
            case " ${ScratchGitScrubbed} " in
                *" ${name} "*) ;;
                *) unlisted="${unlisted:+${unlisted} }${name}" ;;
            esac
        done <<< "$localVars"
        if [ -n "$unlisted" ]; then
            Miss "git-scrub-variables.txt does not name what '$SelfTestGit rev-parse --local-env-vars' prints: ${unlisted} -- add each, since the scrub leaves it inherited"
        else
            Pass "git-scrub-variables.txt names all $(grep -c . <<< "$localVars") variable(s) the running git's --local-env-vars prints"
        fi
    fi

    # The names the list carries from OUTSIDE git's own answer, which the case above cannot see go.
    selfTestCases=$((selfTestCases + 1))
    unlisted=""
    for name in GIT_CONFIG GIT_CONFIG_GLOBAL GIT_CONFIG_SYSTEM GIT_NAMESPACE; do
        case " ${ScratchGitScrubbed} " in
            *" ${name} "*) ;;
            *) unlisted="${unlisted:+${unlisted} }${name}" ;;
        esac
    done
    if [ -n "$unlisted" ]; then
        Miss "git-scrub-variables.txt no longer names ${unlisted}, which its header says it carries beside git's own list"
    else
        Pass "git-scrub-variables.txt names the configuration-file trio and GIT_NAMESPACE"
    fi

    # ---- inherited configuration --------------------------------------------
    # GIT_CONFIG_PARAMETERS is how `git -c` reaches a child, so an exported one is configuration
    # every fixture command obeys. NOT through `core.worktree`: git reads that only from the
    # repository's own config file while it sets up, and an injected one is inert (measured,
    # git 2.51). What an injected key DOES change is what a fixture's write runs: here an
    # attributes file naming a clean FILTER for every path, so the fixture's `add` -- the
    # commonest write a fixture makes -- runs a command that appends to the victim's
    # configuration, the file the incident wrote. The command is a configuration STRING that git
    # hands to its own shell, so nothing executable is staged. Relative paths throughout: the
    # filter runs at the top of the work tree, which is also where `-C` leaves git, so no path is
    # spelled for a git that reads Windows paths.
    printf '* filter=injected\n' > "$scratch/injected-attributes"
    injected="'core.attributesfile=../injected-attributes' 'filter.injected.clean=cat && echo \"[injected]\" >> ../victim/.git/config'"

    # The control: the same injection, unscrubbed, must reach the victim -- otherwise the filter
    # never ran and "unchanged" below says nothing.
    selfTestCases=$((selfTestCases + 1)); ResetVictim
    mkdir -p "$scratch/params-control"
    scratch_git -C "$scratch/params-control" init -q >/dev/null 2>&1
    echo staged > "$scratch/params-control/staged.txt"
    ( export GIT_CONFIG_PARAMETERS="$injected"
      "$rawGit" -C "$scratch/params-control" add -A ) >/dev/null 2>&1
    if grep -q 'injected' "$victim/.git/config"; then
        Pass "control: an unscrubbed add under an exported GIT_CONFIG_PARAMETERS DOES run the injected filter into the victim"
    else
        Miss "control: an unscrubbed add under GIT_CONFIG_PARAMETERS=$injected should have run the injected filter and written the victim's configuration, and did not -- so the case below cannot be believed"
    fi
    printf '%s\n' "$victimConfig" > "$victim/.git/config"

    # The scrub: the victim untouched AND the file actually staged, so an add that failed for
    # another reason does not pass.
    selfTestCases=$((selfTestCases + 1)); ResetVictim
    mkdir -p "$scratch/params"
    scratch_git -C "$scratch/params" init -q >/dev/null 2>&1
    echo staged > "$scratch/params/staged.txt"
    ( export GIT_CONFIG_PARAMETERS="$injected"
      scratch_git -C "$scratch/params" add -A ) >/dev/null 2>&1
    if [ "$(scratch_git -C "$scratch/params" ls-files 2>/dev/null)" = "staged.txt" ]; then
        VictimUntouched "scratch_git add under an exported GIT_CONFIG_PARAMETERS" "the victim"
    else
        Miss "scratch_git add under an exported GIT_CONFIG_PARAMETERS staged nothing where it was asked to"
    fi

    # ---- this self-test, run under the environment it guards against ---------
    # The rows above export variables INSIDE their own subshells; nothing above asks what the
    # self-test as a whole does when its CALLER exported one -- and its raw-git controls are
    # exactly the unscrubbed writes it exists to refuse. So it runs itself, once with GIT_DIR
    # exported at a second scratch victim (the incident's variable) and once with GIT_INDEX_FILE
    # alone (what git exports to a pre-commit hook), and asserts that victim's configuration and
    # index untouched AND the nested run green. The nested run skips this section, or it would
    # never end.
    #
    # The two nested runs go IN PARALLEL, each against a victim of its own so neither can be
    # blamed for the other's write: run one after the other they were two of this self-test's
    # three passes, and the whole took longer than its TIMEOUT on the ARM64 leg. Each is started
    # as `env ... bash`, a process from the first instruction, so no subshell of this one exists
    # to inherit its EXIT trap -- which is the scratch tree's removal.
    if [ -z "${FASTCACHED_SCRATCH_GIT_SELFTEST_NESTED:-}" ]; then
        # @param 1 what is exported, as NAME  @param 2 the victim  @param 3 the value, under it
        # Sets `nestedPid`, in THIS shell: a pid printed through `$( )` would name a child of
        # that subshell, which this shell cannot `wait` for.
        StartNested() {
            local name="$1" victim="$2" value="$3"
            mkdir -p "$victim"
            scratch_git init -q "$victim"
            cat "$victim/.git/config" > "$victim.config-before"
            env "${name}=${victim}/${value}" FASTCACHED_SCRATCH_GIT_SELFTEST_NESTED=1 \
                bash "$me" --self-test --cmake "$SelfTestCmake" --git "$SelfTestGit" > "$victim.out" 2>&1 &
            nestedPid="$!"
        }

        # @param 1 what was exported, as NAME  @param 2 the victim  @param 3 the run's pid
        NestedUntouched() {
            local name="$1" victim2="$2" pid="$3" nestedOut nestedStatus=0 now victim2Config
            selfTestCases=$((selfTestCases + 1))
            wait "$pid" || nestedStatus=$?
            nestedOut="$(cat "$victim2.out")"
            victim2Config="$(cat "$victim2.config-before")"
            now="$(cat "$victim2/.git/config")"
            if [ "$now" != "$victim2Config" ]; then
                Miss "this self-test under an exported ${name} WROTE its victim's configuration" "$(pipe_pair_into "$victim2Config"$'\n' "$now"$'\n' diff /dev/fd/3 -)"
                printf '%s\n' "$victim2Config" > "$victim2/.git/config"
            elif [ -e "$victim2/.git/index" ]; then
                Miss "this self-test under an exported ${name} STAGED into its victim, whose index must stay absent"
            elif ! grep -Fq 'check-scratch-git --self-test: ' <<< "$nestedOut" \
                || ! grep -Fq 'case(s) ran' <<< "$nestedOut"; then
                Miss "this self-test under an exported ${name} did not run to its count line, so the victim being untouched says nothing" "$nestedOut"
            elif [ "$nestedStatus" -ne 0 ]; then
                Miss "this self-test under an exported ${name} left its victim untouched but did not pass (exit ${nestedStatus})" "$nestedOut"
            else
                Pass "this self-test under an exported ${name} passed and left its victim's configuration unchanged and its index absent"
            fi
            rm -f "$victim2/.git/index"
        }

        StartNested GIT_DIR "$scratch/victim2" .git
        gitDirPid="$nestedPid"
        StartNested GIT_INDEX_FILE "$scratch/victim3" .git/index
        indexFilePid="$nestedPid"
        NestedUntouched GIT_DIR "$scratch/victim2" "$gitDirPid"
        NestedUntouched GIT_INDEX_FILE "$scratch/victim3" "$indexFilePid"
    fi

    echo "check-scratch-git --self-test: ${selfTestCases} case(s) ran"
    if [ "$selfTestStatus" -ne 0 ]; then
        echo "check-scratch-git --self-test: FAILED" >&2
        exit 1
    fi
    echo "check-scratch-git --self-test: every verdict as it must be"
    exit 0
fi

Problems=0
Fail() { echo "  FAIL: $*" >&2; Problems=$((Problems + 1)); }

# Every first-party script and CMake file the scan reads, one per line, enumerated through git
# -- the one answer to which files are this project's.
Subjects() {
    local tracked firstParty declined
    tracked="$(git -C "${FastCachedRoot}" ls-files -- 'scripts/*.sh' 'scripts/*.ps1' 'scripts/*.cmake' \
        'src/tests/*.sh' 'src/tests/*.ps1' 'src/tests/*.cmake' 'src/tests/CMakeLists.txt')" || return 2
    if [ -z "${tracked}" ]; then
        echo "check-scratch-git: git ls-files named no script under scripts/ or src/tests/, which cannot be" >&2
        echo "  true. Refused rather than read as 'no fixture writes git unscrubbed'." >&2
        return 2
    fi
    if ! firstParty="$(first_party_paths "${FastCachedRoot}" "${tracked}")"; then
        echo "check-scratch-git: the third-party roots could not be read, so which files are this" >&2
        echo "  project's own is unknown. Refused." >&2
        return 2
    fi
    declined="$(third_party_paths "${FastCachedRoot}" "${tracked}")" || return 2
    FastCachedDeclined="$(third_party_declined_summary 'file(s)' "${declined}")"
    printf '%s\n' "${firstParty}"
}

# The unscrubbed git writes in @p 1, as `<line>:<text>`, or nothing.
UnscrubbedWrites() {
    sed -e 's/^[[:space:]]*#.*$//' "$1" | grep -nE -- "${WritePattern}" || true
}

SubjectList="$(Subjects)" || exit 2

ExemptPaths=""
if [ -r "${FastCachedExemptions}" ]; then
    ExemptPaths="$(sed -e 's/^[[:space:]]*#.*$//' -e '/^[[:space:]]*$/d' "${FastCachedExemptions}" | cut -f1)"
fi

Scanned=0
Flagged=0
Sites=0
ExemptUsed=""
while IFS= read -r subject; do
    [ -n "${subject}" ] || continue
    path="${FastCachedRoot}/${subject}"
    [ -r "${path}" ] || continue
    Scanned=$((Scanned + 1))
    hits="$(UnscrubbedWrites "${path}")"
    [ -n "${hits}" ] || continue
    if grep -Fqx -- "${subject}" <<< "${ExemptPaths}"; then
        ExemptUsed="${ExemptUsed}${subject}"$'\n'
        continue
    fi
    Flagged=$((Flagged + 1))
    Sites=$((Sites + $(grep -c . <<< "${hits}")))
    Fail "${subject} runs a git WRITE that inherits the caller's GIT_DIR, GIT_WORK_TREE and the rest of scripts/lib/git-scrub-variables.txt -- under an exported GIT_DIR it writes THAT repository, not the scratch one. Spell it scratch_git (source scripts/lib/git-scrub.sh) or, in cmake -P, through fastcached_scratch_git (scripts/lib/CheckCommon.cmake). If this script writes the repository it was ASKED to rather than one it created, add a row to scripts/check-scratch-git-exemptions.txt saying so."
    printf '%s\n' "${hits}" | sed 's/^/        /' >&2
done <<< "${SubjectList}"

while IFS= read -r row; do
    [ -n "${row}" ] || continue
    if ! grep -Fqx -- "${row}" <<< "${ExemptUsed}"; then
        Fail "the exemption row for '${row}' matched nothing: either the file no longer writes git unscrubbed, or it is no longer scanned. Delete the row rather than leaving it to excuse the next site at that path."
    fi
done <<< "${ExemptPaths}"

if [ "${Scanned}" -eq 0 ]; then
    echo "check-scratch-git: no file under scripts/ or src/tests/ could be read, which cannot be true." >&2
    echo "  Refused rather than read as 'no fixture writes git unscrubbed'." >&2
    exit 2
fi

echo "check-scratch-git: enumerated via git ls-files"
[ -z "${FastCachedDeclined:-}" ] || echo "check-scratch-git: ${FastCachedDeclined}"
exemptCount="$(grep -c . <<< "${ExemptPaths}" || true)"
echo "check-scratch-git: ${Scanned} file(s) scanned, ${Sites} unscrubbed git write(s) in ${Flagged} file(s), ${exemptCount:-0} file(s) exempt by row"
if [ "${Problems}" -ne 0 ]; then
    echo "check-scratch-git: ${Problems} problem(s); an unscrubbed fixture write is how the shared configuration got core.worktree" >&2
    exit 1
fi
echo "check-scratch-git: every fixture's git write goes through the scrub"
exit 0
