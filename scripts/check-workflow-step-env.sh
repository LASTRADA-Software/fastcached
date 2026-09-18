#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every name a workflow `run:` step's script READS from the environment is one that step can see (#1448, #1175).
#
# Usage:
#   bash scripts/check-workflow-step-env.sh [--plant] [--workflows <dir>]
#   bash scripts/check-workflow-step-env.sh --self-test
#
# ## Why
#
# A step's environment is its own: a name declared in the `env:` of the step next door is not in scope, and
# `${{ ... }}` is substituted before the shell ever sees the script, so nothing about the file's appearance says
# which names will exist. #1174 is what that cost -- `merge-group-report.yml` defined `EVENT` on its decide step and
# read it in the reporting step, which died on its first line under `set -u`, so the notifier opened no report in
# its entire life. Without `set -u` the name expands to EMPTY and a branch is silently taken the wrong way, which is
# worse, so this covers every `run:`, not only the ones turning on `-u`.
#
# The rule was first enforced over that one workflow, inside `check-merge-group-report.sh`, and so reached 3 of the
# tree's 135 `run:` steps at fa8fb0c3: exact about the file it knew and silent about the rest, which reads identically
# to complete coverage. So the files are a GLOB, every file it finds must be read, and the count of each is printed.
#
# ## What a step can see
#
#   * the workflow's `env:`, its job's `env:` (only its own job's), and its own `env:`;
#   * names its own script assigns (bash: `NAME=`, `NAME+=` in command position, a declarator such as `export` or
#     `local`, `read`, `for NAME in`, `mapfile`/`readarray`, `getopts`; PowerShell: `$env:NAME =`);
#   * the runner's and the shell's own vocabulary, which is an ALLOWLIST and fails CLOSED -- a name nobody has listed
#     is refused and gets added deliberately, where an open pattern would have admitted `EVENT`.
#
# A name an EARLIER step or an action exported through `$GITHUB_ENV` does reach a later step at run time, and is
# still refused unless the step names it in its own `env:` -- `NAME: ${{ env.NAME }}`, the spelling the tree already
# uses for `CLANG_TIDY_BIN`. That row is the only spelling that says, in the step, where the name comes from.
#
# ## The `${{ env.NAME }}` expressions themselves (#1460)
#
# That convention concentrates the risk in one spelling, so the RIGHT-HAND SIDE is read too. An expression naming
# nothing in scope is substituted with an EMPTY STRING, with no error and no warning, so `SCCACHE_PAHT:
# ${{ env.SCCACHE_PAHT }}` defines the name the shell half then finds and nothing anywhere objects -- #1174 through a
# second door.
#
# Read in a step's `run:`, `with:`, `if:`, `name:` and `env:` values. Satisfied by the workflow's or the job's
# `env:`, by a `$GITHUB_ENV` write in an EARLIER step of the same job, by an `ActionExports` row, and -- for every
# field except a step's own `env:` values -- by the step's own `env:`. That exception is the whole point rather than
# a detail: GitHub substitutes an `env:` value before the step's environment exists, so a typo that reads the name
# it is defining would otherwise satisfy itself.
#
# `ActionExports` holds only names some step READS, and a row is refused STALE in both directions -- its action used
# by nobody, or its name read by nobody. A `$GITHUB_ENV` write is any line NAMING `GITHUB_ENV`: every `NAME=` token
# on it is taken as written, which covers the bash `echo`/`printf` redirect, PowerShell's `>> $env:GITHUB_ENV` and
# `Add-Content`, each with a case. Job-level and workflow-level fields -- `runs-on`, a job's `env:` values, a job's
# `if:` -- are out of scope and unread, as is every context other than `env.`.
#
# ## Which shell reads the script
#
# The step's `shell:`, else its job's `defaults.run.shell`, else the workflow's, else the runner's default (pwsh on
# Windows, bash on Ubuntu, macOS or Linux) -- for `runs-on` as written, or, where it names the job's matrix, for EVERY
# combination of that matrix, which the shared walk computes (#1432). Combinations whose runners' defaults differ are
# REFUSED (`varying-shell`): one script cannot be held to two models. A step whose shell cannot be resolved, or names
# a shell `ShellModels` has no row for, is REFUSED rather than read by the wrong model:
#
#   * bash reads `$NAME` and `${NAME...}`, outside single quotes, quoted heredocs, and `\$`.
#   * PowerShell reads the environment only as `$env:NAME` or `${env:NAME}`, case-insensitively, outside single
#     quotes and single-quoted here-strings; a plain `$name` is a PowerShell variable and is not the environment.
#
# The models are deliberately NARROW -- a model more permissive than the shell waves through the one read that
# matters. The reader of the YAML is narrow the same way: a line it cannot place by indentation (a flow mapping, an
# alias, a quoted key, a second document) is REFUSED, never skipped.
#
# ## What says the reader read every step
#
# Nothing in the reader, which is why the count is not in it: every `run:` key outside a scalar is counted by a second
# walk that knows only which lines a scalar owns, both numbers are printed per file, and a key the reader did not place
# as a step's script (or a `defaults.run`) is refused by line. A step whose keys sit where the reader does not look
# would otherwise read as a step that reads nothing. The plant (`--plant`) answers the other half on the real files:
# every step the reader DID place is read by a model that sees a read at its end.
#
# A tracked composite action (`action.yml`/`action.yaml` whose `runs.using` is `composite`) has `run:` steps under the
# same rule and none of them is read here, so the unplanted run refuses one by name; git's listing is the file set.
#
# ## What it does NOT cover, so that nobody reads a pass as more than it is
#
# A name reached by `eval` or indirect expansion; one a sourced file defines; `${#arr[@]}`. And four stated blind
# spots in the expression half, each narrow and each the reason a wider reader was not written:
#
#   * a `$GITHUB_ENV` write whose names are on LATER lines (`cat >> "$GITHUB_ENV" <<EOF`) is not seen; the two
#     writes in this tree are both the `echo` form;
#   * a step `env:` VALUE written as a BLOCK SCALAR is read, but the `--plant` for expressions does not reach it;
#   * an expression inside a YAML comment is correctly unread, and one inside a `#` line of a `run:` BODY is read,
#     because GitHub substitutes that one before the shell ever sees it;
#   * a name a `${{ }}` expression supplies to the SHELL is still invisible to the shell half, which is the original
#     residual and is unchanged: the expression is substituted before the script exists.
#
# ## Where in a script a name has to be defined
#
# A definition is a LINE (#1461). A read is judged against the scopes, the allowlist, and what the script defines at
# or ABOVE the read's own line -- not against the whole script, which is what bash would have to be for the looser
# model to be right. Relaxed to the whole script inside a LOOP body, which runs again, and a FUNCTION body, which
# runs wherever it is called. A `trap` handler needs no relaxation and getting to that answer was the work: see the
# section on it beside `define()`. The grain is a line, so a read and an assignment on ONE line are not ordered
# against each other, and for PowerShell the relaxation is every brace-enclosed block. Both limits are stated there.
#
# bash 3.2 and a POSIX awk: this runs on macOS's `/bin/bash` and BSD awk.
#
# ## What the self-test costs, and why its TIMEOUT did not move (#1550)
#
# Its cost is PROCESS CREATIONS, which Windows charges for heavily and an x64-emulated
# `windows-11-arm` more heavily still: `workflow-step-env-selftest` took 77.97 s of its `TIMEOUT 120`
# on `Windows-cl-release-arm64` and 24-35 s on the x64 Windows legs (run 35340951724). These figures
# are pinned to the conditions they were taken under, not live values. Counted with strace over the
# process tree at a0a4d09d, bash 5.3 on Linux: 932 forks and 533 execs. A `mkdir` and a `cat` per
# case staged each workflow, and `Flatten` ran a `printf | tr` pipeline in a subshell on every scan.
# All three are now the shell's own work: 414 forks and 209 execs (bash 3.2.57: 1021 and 533 became
# 503 and 209). Git Bash on a Windows 11 x64 host went from 12.5-14.8 s to 6.7-7.3 s. What remains
# is what the cases exist to drive -- one `awk` per scan and the `git` a composite case stages
# through -- plus a subshell per case, kept because it is what stops one case's state reaching the
# next. `TIMEOUT 120` stays: a timeout is not a verdict about the tree (#1515).

set -uo pipefail

# Which files are this project's own is ONE answer, and an enumerator that does not ask takes vendored source
# as first-party (#1370). `CompositeActions` lists what git tracks, so it asks.
. "$(dirname "${BASH_SOURCE[0]}")/lib/third-party-roots.sh"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The two awk programs this check runs, as FILES (#1456). awk has no include directive, so the
# shared lexer is a second `-f`; `-f` cannot be mixed with an inline program, so this check's own
# reader moved to a file in the same change. Both are REFUSED when missing rather than letting
# awk report a syntax error about a program it could not read -- a reader that ran over no
# program is not a clean tree.
FastCachedShellLexAwk="$(dirname "${BASH_SOURCE[0]}")/lib/shell-lex.awk"
FastCachedWorkflowWalkAwk="$(dirname "${BASH_SOURCE[0]}")/lib/workflow-walk.awk"
FastCachedStepEnvAwk="$(dirname "${BASH_SOURCE[0]}")/check-workflow-step-env.awk"
for awkProgram in "$FastCachedShellLexAwk" "$FastCachedWorkflowWalkAwk" "$FastCachedStepEnvAwk"; do
    if [ ! -f "$awkProgram" ]; then
        echo "check-workflow-step-env: missing awk program ${awkProgram}; nothing was read, so this"
        echo "                         run judged no workflow -- refused rather than reported clean"
        exit 2
    fi
done
workflows_dir="${repo_root}/.github/workflows"

# The runner's vocabulary, on every runner.
RunnerProvided="RUNNER_TEMP RUNNER_OS RUNNER_ARCH RUNNER_TOOL_CACHE RUNNER_WORKSPACE RUNNER_DEBUG
GITHUB_OUTPUT GITHUB_ENV GITHUB_PATH GITHUB_STEP_SUMMARY GITHUB_WORKSPACE
GITHUB_REPOSITORY GITHUB_SHA GITHUB_REF GITHUB_REF_NAME GITHUB_EVENT_NAME
GITHUB_EVENT_PATH GITHUB_RUN_ID GITHUB_RUN_NUMBER GITHUB_ACTOR GITHUB_JOB
GITHUB_SERVER_URL GITHUB_API_URL GITHUB_HEAD_REF GITHUB_BASE_REF GITHUB_TOKEN"

# What a POSIX system and bash itself set, for a bash step. Both scans over `scripts/` read this list as code, and it
# is data: it spells `BASHPID` (bash 4.0+) and `SECONDS` (a clock reading), neither of which this script executes.
# bash32-scan: data-begin
# seconds-scan: data-begin
BashProvided="HOME PATH PWD SHELL USER TMPDIR LANG IFS OSTYPE HOSTNAME
BASH_VERSION BASH_SOURCE BASH_REMATCH BASHPID FUNCNAME LINENO RANDOM SECONDS PIPESTATUS OPTARG OPTIND"
# seconds-scan: data-end
# bash32-scan: data-end

# What Windows sets, for a PowerShell step, compared case-insensitively as Windows does. A row is added for a name the
# tree reads, never pre-emptively.
WindowsProvided="PROGRAMDATA"

# first word of a shell: value|model. `bash {0}`, `bash --noprofile --norc -eo pipefail {0}` and plain `bash` all
# start with the word. A row exists for a shell a workflow here uses; any other is refused by name, and a row for it
# comes with cases.
ShellModels="bash|bash
pwsh|pwsh"

# action|NAME -- a name an action EXPORTS, and only a name some step here reads through
# `${{ env.NAME }}`. One row per (action, name); the action is matched on the part before its
# `@`, so a version bump keeps the row.
#
# A row is refused STALE in BOTH directions, because the two go wrong separately: its action
# is used by no step any more, or its name is read by no expression any more. An opt-in list
# of exports would read identically to complete coverage, which is why this holds only what
# is READ -- a name nothing reads needs no row and would have to be kept correct for nothing.
# OVERRIDABLE by `FASTCACHED_STEPENV_ACTION_EXPORTS`, and the self-test sets it EMPTY: a
# synthetic workflow uses none of these actions and reads none of these names, so every
# case would trip the staleness check below on a tree the case says nothing about. A case
# that is ABOUT an export stages its own row.
ActionExports="${FASTCACHED_STEPENV_ACTION_EXPORTS-mozilla-actions/sccache-action|SCCACHE_PATH}"

# Whether the two WHOLE-TREE claims below are being made: that at least one
# `${{ env.NAME }}` expression was read, and that every `ActionExports` row still describes
# something.
#
# Both are claims about THIS repository's workflows and about no directory the scan is pointed
# at. A staged tree legitimately reads no expression and uses no action, so making either claim
# there is the guard firing on a tree it says nothing about. ON for the default directory, OFF
# for a `--workflows` one -- a SCOPE rather than an exemption.
#
# **Both, through one knob, because the second caller is what taught this.** The staleness half
# was first scoped by emptying the table inside this file's own self-test, which fixed the
# caller I could see: `check-merge-group-report.sh` also runs this check over a staged tree, and
# CI refused the real `sccache-action` row on a tree that uses no action at all. A rule stated
# in the files that obey it reaches no file that does not.
#
# A case sets it to 1 to drive either refusing direction, because a guard nobody has watched
# refuse is not known to be a guard.
JudgingTheRealTree=1

# kind|remedy -- the text a refusal of that kind ends with.
Remedies="undefined-expression|A \`\${{ env.NAME }}\` expression naming nothing in scope is replaced by an EMPTY string, with no error and no warning -- #1174 through a second door (#1460). Add the row to the workflow's or the job's \`env:\`, or write it to \`\$GITHUB_ENV\` from an EARLIER step of the same job, or give \`ActionExports\` a row for the action that exports it. A step's own \`env:\` satisfies that step's \`run:\`, \`with:\`, \`if:\` and \`name:\` -- but NOT another row of the same \`env:\` block, and not its own row: GitHub substitutes those values before the step's environment exists, so a typo'd \`NAME: \${{ env.NAME }}\` would otherwise satisfy itself, which is exactly the shape the convention concentrates.
stale-action-export|An \`ActionExports\` row describes nothing in this tree. Either no step \`uses:\` that action any more, or no \`\${{ env.NAME }}\` expression reads that name any more. A row kept past its subject is the \"forgot\" state wearing the vocabulary of a decision: it goes on satisfying reads that no longer exist and would satisfy a new one by accident. Delete the row, or restore whichever half went away.
undefined|A step's environment is its own: a sibling step's or another job's \`env:\` does not lend it one, so under \`set -u\` the step dies on that line and without it the name expands to EMPTY and a branch is silently taken the wrong way (#1174). Add the row to THIS step's \`env:\` (or its job's), or assign it in the script. A name an earlier step or an action exported is named the same way, as \`NAME: \${{ env.NAME }}\`.
unknown-shell|This check reads a script by the model of the shell that runs it, and has none for this one. Add a row to ShellModels only with a model of how that shell reads its environment, and cases for it.
unresolved-shell|The shell could not be derived: no \`shell:\` on the step, no \`defaults.run.shell\` above it, and a \`runs-on\` that is not a Windows, Ubuntu, macOS or Linux runner -- literally, or in some combination of the job's matrix once \`\${{ matrix.* }}\` is expanded for it (a key that combination lacks expands EMPTY, as GitHub expands it). Name the shell on the step.
varying-shell|The step names no shell, and its job's matrix puts it on runners whose DEFAULT shells differ, so one script would be read by two shells and there is no one model to hold it to. Name the shell on the step, or in the job's \`defaults.run.shell\`. This check does not evaluate a step's \`if:\`, so a step an \`if:\` confines to some legs still needs its shell named -- a script that must differ per runner is two steps, each with an \`if:\` on the matrix value AND a \`shell:\` of its own.
unreadable-matrix|The job's \`strategy.matrix\` is not one the shared model of a matrix (\`scripts/lib/workflow-walk.awk\`) can read -- an expression in its place, a flow mapping, an include or exclude row that is not a block mapping of plain scalars, a value carrying an expression, a list that does not close on its line, an axis with no values, an exclude naming no axis, two keys that differ only in case, a plain value YAML types as anything but a string, an integer or \`true\`/\`false\` (\`3.10\` is the number 3.1; quote it) -- so which runner, name and cache key each leg has cannot be told, and nothing that depends on them is judged. Write it as block mappings of plain-scalar lists and rows; if the construct is needed, teach the walk to read it, with a case.
unreadable-run|The \`run:\` value is not a string this check can read: an alias, a tag, a flow collection, or a quoted scalar that does not close on its own line, has text after its closing quote, or holds a YAML escape other than \`\\\\\"\`, \`\\\\\\\\\` or \`\\\\/\`. YAML quoting is not shell quoting, and a quote read as the shell would read it hides the reads inside it. Write the script as a block scalar (\`run: |\`), or as a plain or quoted string on one line.
unreadable-env|The \`env:\` is not a block mapping this check can read. Write one \`NAME: value\` row per line.
unreadable-yaml|This check places every line of a workflow by its indentation, and cannot place this one -- a flow mapping, an alias, a quoted key, or a second document -- so no step around it can be judged. Write it as a block mapping; if the construct is needed, teach the reader and add a case.
unplaced-run|Every \`run:\` key outside a scalar is counted without the reader and must be a script the reader placed in a step (or a \`defaults.run\`), so a step whose keys the reader stopped recognising is refused here rather than read as clean. If this is a step's script, the reader misplaced the step: teach it, with a case. If it is not one -- an action input or an \`env:\` row named \`run\` -- the reader has no place for it either: teach it where the key sits, with a case. Never rename a script key to get past this.
order|A step's script is read in ORDER, because bash is: the name is expanded where the line stands, so a read above the line that assigns it dies there under \`set -u\` and expands EMPTY without it, taking a branch the wrong way (#1174, #1461). Move the assignment above the read, or give the step's \`env:\` a row for it. A read inside a LOOP body or a FUNCTION body is judged against the whole script instead -- a loop body runs again and a function body runs where it is called -- so this is a read at the script's top level.
unreadable-construct|This check follows a step's loops and functions to know where ordering applies, and lost the one it was inside: the \`do\`/\`done\` pairs, or a function's braces, do not balance in the CODE the lexer left. A \`do\`, a \`done\` or a brace inside a string or a heredoc is masked and cannot cause this. Write the construct so it balances, or teach the reader the shape, with a case."

# The tables as awk takes them: a -v value may not hold a newline on BSD awk. Done by the shell
# into a named variable, because `Scan` flattens once per workflow and the self-test scans one per
# case: as `$(printf | tr)` that was a subshell, a pipeline and a `tr` each time (#1550).
# @param 1 The variable to assign. @param 2 The text. @param 3 What each newline becomes.
Flatten() { printf -v "$1" '%s' "${2//$'\n'/$3}"; }
Flatten bashNames "$RunnerProvided $BashProvided" ' '
Flatten windowsNames "$RunnerProvided $WindowsProvided" ' '
Flatten shellRows "$ShellModels" ' '
Flatten remedyRows "$Remedies" $'\036'

problems=0

# Scan one workflow file. Prints `REFUSE\t<line>\t<step>\t<kind>\t<detail>\t<remedy>` per refusal, in plant mode
# `PLANTED\t<line>\t<step>\t<seen 0|1>` per step a model read, and last `STEPS\t<steps>\t<bash>\t<pwsh>\t<run keys>\t<placed>`.
# @param 1 The workflow file.
# @param 2 1 to plant a read of an undefined name at the end of every step a model reads, else 0.
Scan() {
    # Flattened HERE rather than once at the top, because a self-test case stages its own
    # `ActionExports` and a value read before that would be the real row every time.
    local exportRows
    Flatten exportRows "$ActionExports" ' '
    awk -v bashNames="$bashNames" -v windowsNames="$windowsNames" -v shells="$shellRows" -v remedies="$remedyRows" \
        -v plant="$2" -v PlantName="FASTCACHED_PLANTED_READ" -v PlantExprName="FASTCACHED_PLANTED_EXPR" \
        -v exports="$exportRows" \
        -f "${FastCachedShellLexAwk}" -f "${FastCachedWorkflowWalkAwk}" \
        -f "${FastCachedStepEnvAwk}" "$1" "$1"
}

# Judge every workflow under @p dir; print one line per file and per refusal; return 0 clean, 1 refused.
# @param 1 The workflows directory.
# @param 2 `plant` to plant a read of an undefined name at the end of every run step, which passes only when every
#          step a model read refuses it: proof, on the real files, that each is read by a model that sees a read.
Judge() {
    local dir="$1" mode="${2:-plain}" plantFlag=0 file display status report files=0 steps=0 modelled=0 bashSteps=0 pwshSteps=0
    local runKeys=0 placedKeys=0
    local tag line step kind detail remedy scanned planted=0 unplanted=0
    local exprReads=0 exprNames="" usedActions="" plantedExpr=0 envRows=0 action name
    [ "$mode" = plant ] && plantFlag=1
    for file in "$dir"/*.yml "$dir"/*.yaml; do
        [ -e "$file" ] || continue
        files=$((files + 1))
        display="${file#"$repo_root"/}"
        report="$(Scan "$file" "$plantFlag")"
        status=$?
        scanned=""
        while IFS=$'\t' read -r tag line step kind detail remedy; do
            case "$tag" in
                STEPS)
                    # For a STEPS record the fields are the steps, those read as bash, those read as PowerShell, the
                    # `run:` keys counted without the reader, and those the reader placed.
                    scanned="$line"
                    steps=$((steps + line)); bashSteps=$((bashSteps + step)); pwshSteps=$((pwshSteps + kind))
                    runKeys=$((runKeys + detail)); placedKeys=$((placedKeys + remedy))
                    echo "workflow-step-env: ${display}: ${detail} run: key(s) counted, ${remedy} placed; ${line} run step(s), ${step} read as bash, ${kind} as PowerShell"
                    ;;
                PLANTED)
                    # For a PLANTED record the fourth field is whether the planted read was seen.
                    planted=$((planted + 1))
                    if [ "$kind" != "1" ]; then
                        echo "  FAIL: ${display}:${line} step \"${step}\": a read planted at the end of its script was NOT seen, so its reading ends inside a quote, a heredoc or a here-string and a real read there would pass unseen. Either the script really leaves one open, or the check misreads a construct in it -- then the check is what needs fixing, with a case."
                        problems=$((problems + 1))
                    fi
                    ;;
                EXPRREAD)
                    # One per `${{ env.NAME }}` a step carries, wherever it sits. Counted so a run
                    # that read NONE says so: this check went clean over the whole tree on the day
                    # the expression half was still inert, which is the shape of every green
                    # reading taken on the wrong object.
                    exprReads=$((exprReads + 1))
                    exprNames="${exprNames}${detail}
"
                    ;;
                USES)
                    # Every action a step names, version stripped. Only for the staleness check
                    # below: an `ActionExports` row whose action nothing uses any more.
                    usedActions="${usedActions}${detail}
"
                    ;;
                PLANTEDEXPR)
                    # Per step: whether the planted expression was seen, and how many `env:` rows
                    # that step has. A step with NO env row cannot carry the plant, so the count
                    # below is against the steps that have one rather than against every step.
                    if [ "${detail:-0}" != "0" ]; then
                        envRows=$((envRows + 1))
                        if [ "$kind" = "1" ]; then
                            plantedExpr=$((plantedExpr + 1))
                        else
                            echo "  FAIL: ${display}:${line} step \"${step}\": an expression planted into its \`env:\` rows was NOT seen, so the reader is not visiting that block and a typo of that kind \`\${{ env.X }}\` there would pass unread."
                            problems=$((problems + 1))
                        fi
                    fi
                    ;;
                REFUSE)
                    # A plant run answers one question, whether each step a model read refuses the planted read, so
                    # what the tree itself gets wrong is the unplanted run's to report.
                    if [ "$mode" = plant ]; then
                        unplanted=$((unplanted + 1))
                        continue
                    fi
                    echo "  FAIL: ${display}:${line} step \"${step}\": ${detail} -- ${remedy}"
                    problems=$((problems + 1))
                    ;;
            esac
        done <<< "$report"
        if [ "$status" -ne 0 ] || [ -z "$scanned" ]; then
            echo "  FAIL: ${display}: the scan did not finish (awk exited ${status}), so none of its steps was judged"
            problems=$((problems + 1))
        fi
    done
    modelled=$((bashSteps + pwshSteps))
    if [ "$files" -eq 0 ]; then
        echo "  FAIL: no workflow file under ${dir} -- a check that reads nothing reports clean, which is the state it exists to remove"
        return 1
    fi
    if [ "$steps" -eq 0 ]; then
        echo "  FAIL: ${files} workflow file(s) and not one \`run:\` step read -- the parser stopped understanding them"
        return 1
    fi
    # AN EXPRESSION READER THAT READ NOTHING is not a clean tree. This check has already gone
    # green over the whole tree with its expression half inert, on the day it was written, and
    # nothing in the output said so -- a count is what makes that state loud.
    if [ "$JudgingTheRealTree" -eq 1 ] && [ "$exprReads" -eq 0 ]; then
        echo "  FAIL: not one \`\${{ env.NAME }}\` expression was read across ${files} workflow file(s). This tree has dozens; reading none means the expression reader stopped seeing them, which reads exactly like a clean tree."
        problems=$((problems + 1))
    fi

    # `ActionExports` rows, STALE IN BOTH DIRECTIONS, and the two go wrong separately. Skipped
    # under `--plant`, whose run answers one question; and skipped when the table is empty, which
    # is how a self-test case stages a tree that must not be judged against the real row.
    if [ "$JudgingTheRealTree" -eq 1 ] && [ "$mode" != plant ] && [ -n "${ActionExports//[[:space:]]/}" ]; then
        while IFS='|' read -r action name; do
            [ -n "${action:-}" ] || continue
            if ! grep -Fxq "$action" <<< "$usedActions"; then
                echo "  FAIL: ActionExports row \`${action}|${name}\`: no step \`uses:\` that action any more. A row kept past its subject goes on satisfying reads that no longer exist, and would satisfy a new one by accident. Delete the row, or restore the step that used it."
                problems=$((problems + 1))
            fi
            if ! grep -Fxq "$name" <<< "$exprNames"; then
                echo "  FAIL: ActionExports row \`${action}|${name}\`: no \`\${{ env.${name} }}\` expression reads that name any more. This table holds only what is READ; a row for a name nothing reads has to be kept correct for nothing. Delete the row, or restore the read."
                problems=$((problems + 1))
            fi
        done <<< "$ActionExports"
    fi

    if [ "$mode" = plant ] && [ "$planted" -ne "$modelled" ]; then
        echo "  FAIL: the plant went into ${planted} of the ${modelled} run step(s) a model read"
        problems=$((problems + 1))
    fi
    if [ "$problems" -ne 0 ]; then
        echo "workflow-step-env: ${mode}: ${problems} problem(s) across ${files} workflow file(s) and ${steps} run step(s)"
        return 1
    fi
    if [ "$mode" = plant ]; then
        echo "workflow-step-env: plant: all ${modelled} run step(s) a model read in ${files} workflow file(s) refused the planted read (${bashSteps} bash, ${pwshSteps} PowerShell), and all ${plantedExpr} step(s) carrying an \`env:\` row saw a planted expression in it; ${unplanted} unplanted problem(s) left to the unplanted run"
    else
        echo "workflow-step-env: ${files} workflow file(s), ${runKeys} run: key(s) counted and ${placedKeys} placed, ${steps} run step(s) (${bashSteps} bash, ${pwshSteps} PowerShell), ${exprReads} \${{ env.* }} expression(s) read, every environment name each reads is one it can see"
    fi
    return 0
}

# A composite action's `runs.steps` are `run:` steps with the same environment rule, and this check reads none of
# them. So one is REFUSED by name while none is tracked, rather than left outside a pass that reads as complete; the
# file set is what git tracks, which is what a workflow can `uses: ./` -- a file on disk that is not tracked is not.
# A JavaScript or Docker action runs no shell and is counted, and one whose `runs.using` cannot be read is refused.
# @param 1 The repository root.
# @return 0 when no composite action is tracked, 1 otherwise.
CompositeActions() {
    local root="$1" listed status file using firstParty declined summary tracked=0 actions=0 refused=0
    # NUL-separated: a plain listing QUOTES a path holding a byte outside ASCII, and `*/action.yml` misses
    # `".../action.yml"`. pipefail keeps git's status rather than tr's.
    listed="$(git -C "$root" ls-files -z 2>&1 | tr '\0' '\n')"
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "  FAIL: git ls-files exited ${status} in ${root}, so no tracked composite action could be looked for: ${listed}"
        return 1
    fi
    # A vendored action is not this project's to judge, and a roots file that cannot be read is a REFUSAL:
    # read as nothing, every `action.yml` under vendor/ would be refused as one of ours (#1370).
    if ! firstParty="$(first_party_paths "$root" "$listed")"; then
        echo "  FAIL: the third-party roots of ${root} could not be read (the reader says why above), so a vendored composite action cannot be told from one of ours"
        return 1
    fi
    declined="$(third_party_paths "$root" "$listed")"
    listed="$firstParty"
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        tracked=$((tracked + 1))
        case "$file" in
            action.yml | action.yaml | */action.yml | */action.yaml) ;;
            *) continue ;;
        esac
        actions=$((actions + 1))
        using="$(awk -v sq="'" '
            { sub(/\r$/, "") }
            /^runs:[ \t]*(#.*)?$/ { inRuns = 1; next }
            /^[^ #]/ { inRuns = 0 }
            inRuns && /^[ ]+using:/ { v = $0; sub(/^[ ]+using:[ \t]*/, "", v); sub(/[ \t]+#.*$/, "", v); gsub(/"/, "", v); gsub(sq, "", v); print v; exit }
        ' "${root}/${file}" 2>/dev/null)"
        case "$using" in
            composite)
                echo "  FAIL: ${file}: a composite action, whose steps this check does not read -- its \`run:\` steps are held to the same rule as a workflow's and nothing here judges them. Teach this check to read \`runs.steps\` as a job's steps, with cases, before adding one."
                refused=$((refused + 1))
                ;;
            node[0-9]* | docker) ;;
            *)
                echo "  FAIL: ${file}: \`runs.using\` reads \`${using}\`, which is neither a composite action nor one that runs no shell, so whether its steps need judging cannot be told. Write \`using:\` under \`runs:\` on a line of its own, or teach this check the value."
                refused=$((refused + 1))
                ;;
        esac
    done <<< "$listed"
    if [ "$tracked" -eq 0 ]; then
        echo "  FAIL: git ls-files listed no tracked file in ${root} -- a listing of nothing is not a repository without actions"
        return 1
    fi
    summary="$(third_party_declined_summary "tracked file(s)" "$declined")"
    echo "workflow-step-env: actions: ${tracked} first-party tracked file(s) listed by git ls-files, ${actions} action file(s), ${refused} refused${summary:+ -- ${summary}}"
    [ "$refused" -eq 0 ]
}

# ---------------------------------------------------------------------------
SelfTest() {
    local tmp ran=0 failures=0
    # A case that is ABOUT an export stages its own `ActionExports`; the rest are judged with
    # the whole-tree claims OFF (`JudgingTheRealTree`), so the real row is simply not consulted.
    # Emptying it here as well would work and is deliberately NOT done: it would hide the scope
    # from the reader and leave the second caller -- `check-merge-group-report.sh` -- relying on
    # a fix made in this file.
    ActionExports=""
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/workflow-step-env-selftest.XXXXXX")" || { echo "cannot create a scratch directory"; exit 2; }
    trap 'rm -rf -- "$tmp"' EXIT
    # Every case stages its workflow into ONE directory, overwriting the last, and `nofile` judges
    # one that stays empty. A directory per case cost a `mkdir` each, and the staging a `cat`: a
    # process creation apiece, which Windows charges tens of milliseconds for and an x64-emulated
    # windows-11-arm more (#1550).
    mkdir -p "$tmp/case" "$tmp/empty" || { echo "cannot create the case directories"; exit 2; }

    # Write stdin to a file, byte for byte, without a `cat`. Nothing staged here holds a NUL byte,
    # which is the one thing `read -d ''` cannot carry.
    # @param 1 The file to write.
    WriteStdinTo() {
        local body=""
        IFS= read -r -d '' body || true
        printf '%s' "$body" > "$1"
    }

    # Judge the workflow on stdin, alone in a directory.
    # @param 1 Case name. @param 2 Expected status (0 or 1). @param 3 Text the output must hold.
    # @param 4 Mode (plain or plant). @param 5 `nofile` to judge an empty directory and read nothing from stdin.
    # @param 6 1 to require that at least one `${{ env.NAME }}` expression was read, else 0.
    Case() {
        local name="$1" want="$2" expect="$3" mode="${4:-plain}" out got dir="$tmp/case"
        # OFF by default here for the reason `JudgingTheRealTree` records: a staged tree reads no
        # expression and uses no action, and the cases that drive either guard pass 1 as their
        # sixth argument.
        local JudgingTheRealTree="${6:-0}"
        if [ "${5:-}" = nofile ]; then
            dir="$tmp/empty"
        else
            WriteStdinTo "$dir/wf.yml"
        fi
        out="$(problems=0; Judge "$dir" "$mode" 2>&1)"
        got=$?
        ran=$((ran + 1))
        if [ "$got" != "$want" ]; then
            failures=$((failures + 1))
            printf '  %s: exited %s, wanted %s\n%s\n' "$name" "$got" "$want" "$out"
        elif [[ "$out" != *"$expect"* ]]; then
            failures=$((failures + 1))
            printf '  %s: the right side for the wrong reason, no `%s` in:\n%s\n' "$name" "$expect" "$out"
        fi
    }

    # ---- refused ------------------------------------------------------------------------------------------------

    # #1174's own shape: the decide step defines EVENT, the reporting step reads it.
    Case siblingStepEnv 1 'step "Open or update one report per unreported failure": reads $EVENT' <<'WF'
jobs:
  report:
    runs-on: ubuntu-latest
    steps:
      - name: "Decide"
        env:
          EVENT: ${{ github.event.workflow_run.event }}
        run: echo "$EVENT"
      - name: "Open or update one report per unreported failure"
        run: |
          [[ "$EVENT" == push ]] && export FASTCACHED_REPORT_ONLY_IF_NEW=1
          scripts/ci-report-issue.sh t b
WF

    Case otherJobsEnv 1 'step "reads": reads $ONLY_A' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    env:
      ONLY_A: 1
    steps:
      - run: echo "$ONLY_A"
  b:
    runs-on: ubuntu-latest
    steps:
      - name: "reads"
        run: echo "$ONLY_A"
WF

    Case foldedRun 1 'reads $FOLDED_UNDEFINED' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "folded"
        run: >-
          cmake -S . -B build
          -DX="$FOLDED_UNDEFINED"
WF

    Case pwshEnvRead 1 'reads $env:PWSH_UNDEFINED' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - name: "pwsh"
        shell: pwsh
        run: |
          $x = $env:PWSH_UNDEFINED
WF

    # An export through $GITHUB_ENV reaches a later step at run time; the step still names it in its own env.
    Case githubEnvExportNeedsARow 1 'step "later": reads $TOOL_BIN' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "write"
        run: echo "TOOL_BIN=/opt/tool" >> "$GITHUB_ENV"
      - name: "later"
        run: echo "$TOOL_BIN"
WF

    Case actionExportNeedsARow 1 'reads $env:SCCACHE_PATH' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - uses: mozilla-actions/sccache-action@v0.0.11
      - name: "after the action"
        run: '& $env:SCCACHE_PATH --show-stats'
WF

    Case unreadableRun 1 'is not a string this check can read' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "alias"
        run: *script
WF

    Case unresolvedShell 1 'The shell could not be derived' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.os }}
    steps:
      - name: "which shell"
        run: echo hi
WF

    # ---- a job MATRIX (#1432) -- the runner, and so the default shell, per combination -----------------------------
    #
    # `runs-on` may be `${{ matrix.runner }}`, and the shared walk computes the combinations. Each refusing case here
    # has an accepting twin below, because a model that refused every matrix would pass all of these.

    # Two runners whose default shells differ: one script, two shells, no one model to hold it to.
    Case matrixShellsDisagree 1 'whose default shell is bash, and `windows-2025` for (runner=windows-2025), whose default shell is pwsh' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        runner: [ubuntu-24.04, windows-2025]
    steps:
      - name: "which shell"
        run: echo hi
WF

    # An include row that fits no base combination is a combination of ITS OWN keys, so it has no `runner` and
    # `${{ matrix.runner }}` expands EMPTY for it -- as GitHub expands it -- which names no runner at all.
    Case matrixRowWithNoRunner 1 'which is `` for (os=b)' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        os: [a]
        runner: [ubuntu-24.04]
        include:
          - os: b
    steps:
      - name: "a leg with no runner"
        run: echo hi
WF

    Case matrixUnreadable 1 'is not a block mapping of axes' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}
    steps:
      - name: "on whichever runner"
        run: echo hi
WF

    # A matrix that cannot be read is refused on its own line, and its steps are still JUDGED when `runs-on` does not
    # depend on the legs: a literal runner answers for every one of them, so this refuses the read, not the shell.
    Case matrixUnreadableLiteralRunner 1 'step "on a literal runner": reads $STILL_READ' <<'WF'
jobs:
  a:
    runs-on: ubuntu-24.04
    strategy:
      matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}
    steps:
      - name: "on a literal runner"
        run: echo "$STILL_READ"
WF

    # The shape that motivated the model: its runners agree, so the step is READ -- and a read it cannot see is still
    # refused. Without this, the accepting twin below would pass as well for a step nobody judged.
    Case matrixStepStillJudged 1 'step "on every leg": reads $MATRIX_UNDEFINED' <<'WF'
jobs:
  linux:
    name: "Linux-${{ matrix.preset }}${{ matrix.suffix }}"
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        preset: [clang-release, gcc-release]
        runner: [ubuntu-24.04]
        include:
          - preset: gcc-release
            runner: ubuntu-24.04-arm
            suffix: "-arm64"
    steps:
      - name: "on every leg"
        run: echo "$MATRIX_UNDEFINED"
WF

    Case unknownShell 1 'shell `python`' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "python"
        shell: python
        run: print("hi")
WF

    Case unreadableEnv 1 'is not a block mapping' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    env: { A: 1 }
    steps:
      - run: echo "$A"
WF

    Case flowMappingStep 1 'a step written as `{ run: echo "$HIDDEN" }`' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - { run: echo "$HIDDEN" }
      - run: echo visible
WF

    Case quotedRunKey 1 '`"run": echo "$HIDDEN"`' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "quoted key"
        "run": echo "$HIDDEN"
      - run: echo visible
WF

    Case secondDocument 1 'a document marker after the first document' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: echo visible
---
jobs:
  b:
    runs-on: ubuntu-latest
    steps:
      - run: echo "$IN_THE_SECOND"
WF

    # An apostrophe inside double quotes is a character, not the start of a string that would hide the read.
    Case apostropheInDoubleQuotes 1 'reads $NOT_HIDDEN' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          echo "it's $NOT_HIDDEN"
WF

    # An unquoted heredoc expands, and a quote inside it opens no string.
    Case unquotedHeredoc 1 'reads $IN_HEREDOC' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          cat > body.md <<BODY
          nobody's told: ${IN_HEREDOC}
          BODY
WF

    # `NAME=` after a command word is an argument, not an assignment.
    Case argumentIsNotAssignment 1 'reads $AS_ARGUMENT' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          echo AS_ARGUMENT=1
          echo "$AS_ARGUMENT"
WF

    # A bash step on a Windows runner is read as bash, so a PowerShell-looking variable is a read.
    Case explicitBashOnWindows 1 'reads $local' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - shell: bash
        run: echo "$local"
WF

    Case jobDefaultsShell 1 'reads $fromDefaults' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    defaults:
      run:
        shell: bash
    steps:
      - run: echo "$fromDefaults"
WF

    # On an Ubuntu runner the default shell is bash.
    Case ubuntuDefaultIsBash 1 'reads $stats' <<'WF'
jobs:
  a:
    runs-on: ubuntu-24.04
    steps:
      - run: echo $stats
WF

    # A plain scalar continues on its more-indented lines, and a flow list names the runner.
    Case inlineRunContinues 1 'reads $ON_THE_NEXT_LINE' <<'WF'
jobs:
  a:
    runs-on: [self-hosted, linux]
    steps:
      - run: echo one
          "$ON_THE_NEXT_LINE"
WF

    # More than one space after the dash moves every key of the step, and the step is still read.
    Case wideDashStep 1 'step "wide": reads $AFTER_A_WIDE_DASH' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      -   name: "wide"
          run: echo "$AFTER_A_WIDE_DASH"
WF

    # A job may name its shell after its steps; the steps are still read by that shell.
    Case defaultsAfterSteps 1 'reads $BASH_ON_WINDOWS' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - run: echo "$BASH_ON_WINDOWS"
    defaults:
      run:
        shell: bash
WF

    # An apostrophe in a trailing comment opens no string, in either shell.
    Case bashTrailingComment 1 'reads $AFTER_THE_COMMENT' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          echo hi # do not, won't
          echo "$AFTER_THE_COMMENT"
WF

    Case pwshTrailingComment 1 'reads $env:AFTER_THE_COMMENT' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - run: |
          Write-Host hi # won't
          Write-Host $env:AFTER_THE_COMMENT
WF

    # A prefix assignment belongs to the command it prefixes, not to the lines after it.
    Case prefixAssignment 1 'reads $CC' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          CC=clang make
          echo "$CC"
WF

    # A line starting with `#` inside an unquoted heredoc is text that expands, not a comment.
    Case headingInHeredoc 1 'reads $UNDER_A_HEADING' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          cat > body.md <<BODY
          ## Failed on ${UNDER_A_HEADING}
          BODY
WF

    # A heredoc line is text written into a file, so `NAME=$NAME` in it assigns nothing.
    Case assignmentInHeredoc 1 'reads $WRITTEN_NOT_ASSIGNED' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          cat >> "$GITHUB_ENV" <<EOF
          WRITTEN_NOT_ASSIGNED=$WRITTEN_NOT_ASSIGNED
          EOF
WF

    # A word inside quotes is not a command, so `read NAME` in a string assigns nothing.
    Case readInsideQuotes 1 'reads $QUOTED_READ' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          echo "please read QUOTED_READ first"
          echo "$QUOTED_READ"
WF

    # A YAML-quoted script followed by a comment: the YAML quotes are not shell quotes.
    Case yamlQuotedWithComment 1 'reads $YAML_QUOTED' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: 'echo $YAML_QUOTED' # a note
WF

    # A doubled apostrophe is one apostrophe, so the shell sees a single-quoted `$SINGLE` and a double-quoted read.
    Case yamlDoubledApostrophe 1 'reads $DOUBLE -- ' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: 'echo ''$SINGLE'' "$DOUBLE"'
WF

    # An escaped quote is a quote, and the `#` after it is inside the shell string -- not a comment, and not the end.
    Case yamlEscapedQuote 1 'reads $INSIDE_ESCAPED_QUOTES' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: "echo \"a # $INSIDE_ESCAPED_QUOTES\" # note"
WF

    # A quoted scalar this check cannot read on one line is refused, never read by a model that mistakes its quotes.
    Case quotedRunAcrossLines 1 'is a quoted scalar this check cannot read on one line' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: 'echo one
          $HIDDEN_BY_THE_YAML_QUOTE'
WF

    Case quotedRunOtherEscape 1 'is a quoted scalar this check cannot read on one line' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: "echo \t$AFTER_A_TAB"
WF

    Case quotedRunTextAfterQuote 1 'is a quoted scalar this check cannot read on one line' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: 'echo one' $AFTER_THE_QUOTE
WF

    # A comment ending the way a here-string opens opens nothing, so the next line is code.
    Case pwshOpenerInAComment 1 'reads $env:AFTER_THE_OPENER' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - run: |
          Write-Host hi # a here-string opens with @'
          Write-Host $env:AFTER_THE_OPENER
WF

    # A single-quoted awk program is not the shell, so an assignment in it defines nothing.
    Case assignmentInSingleQuotes 1 'reads $x' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          awk '{ x=1 }' file
          echo "$x"
WF

    # A `run:` key the reader places in no step is counted anyway, and refused by line.
    Case unplacedRunKey 1 '`run: echo "$NOT_A_SCRIPT"` is a `run:` key the reader placed in no step' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - uses: some/action@v1
        with:
          run: echo "$NOT_A_SCRIPT"
      - run: echo visible
WF

    # ---- ORDER (#1461) -- a read judged at its own line -----------------------------------------------------------

    # The shape the ticket opened on. The refusal is reported at the READ, and names the assignment it is above.
    Case orderTopLevelRead 1 ':7 step "reads above its own assignment": reads $TARGET here, and this script assigns it further down, at line 8' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads above its own assignment"
        run: |
          echo "$TARGET"
          TARGET=release
          echo "$TARGET"
WF

    # A DOUBLE-quoted trap handler is expanded where `trap` is CALLED, so ordering is already the right answer for
    # it -- the pair of this case and `orderTrapSingleQuoted` below is why no trap relaxation exists.
    Case orderTrapDoubleQuoted 1 ':7 step "a double-quoted trap handler expands where trap is called": reads $reaper here, and this script assigns it further down, at line 8' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a double-quoted trap handler expands where trap is called"
        run: |
          trap "echo $reaper" EXIT
          reaper=1
WF

    # PowerShell reaches the same judgement through its own scanner, and the name is folded as Windows folds it.
    Case orderPwshRead 1 ':8 step "a PowerShell read above its own assignment": reads $env:LATER here, and this script assigns it further down, at line 9' <<'WF'
jobs:
  j:
    runs-on: windows-2022
    steps:
      - name: "a PowerShell read above its own assignment"
        shell: pwsh
        run: |
          Write-Host $env:LATER
          $env:LATER = "x"
WF

    # Losing the construct is a REFUSAL, not a fall back to judging the script as if it had none: a reader that
    # cannot say where a loop ends cannot say which reads are ordered, and the looser answer is the silent one.
    Case constructDoneWithNoDo 1 'a `done` with no `do`' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a done with no do"
        run: |
          echo one
          done
WF

    Case constructFunctionNeverCloses 1 'a function whose braces never close' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a function whose braces never close"
        run: |
          emit() {
            echo hello
WF

    # ---- EXPRESSIONS (#1460) -- a `${{ env.NAME }}` naming nothing is substituted EMPTY --------------------------
    #
    # One case per field that can carry one, because the fields do not share a code path and a
    # reader that lost one of them would keep passing every case about the others.

    Case exprInRun 1 ':9 step "reads an expression in its script": reads ${{ env.NOPE }} and nothing in scope supplies it' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads an expression in its script"
        env:
          KNOWN: 1
        run: |
          echo "${{ env.NOPE }} $KNOWN"
WF

    Case exprInWith 1 'step "reads an expression in a with: value": reads ${{ env.NOPE }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads an expression in a with: value"
        uses: actions/cache@v4
        with:
          path: ${{ env.NOPE }}
          key: fixed
      - run: echo ok
WF

    # `if:` is the field no step in this tree reads an expression in, so this case is the only
    # thing that says the arm works at all.
    Case exprInIf 1 'step "reads an expression in its if:": reads ${{ env.NOPE }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads an expression in its if:"
        if: ${{ env.NOPE == '1' }}
        run: echo ok
WF

    # A step `name:`, which #1460 did not list and which this tree reads six times -- a typo
    # there is cosmetic rather than functional, and it is the same silent empty substitution.
    Case exprInName 1 'reads ${{ env.NOPE }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "install ${{ env.NOPE }}"
        run: echo ok
WF

    # THE motivating shape. `NAME: ${{ env.NAME }}` is the convention this check itself
    # established for bringing an exported name into a step, so a typo in it defines the name
    # the shell half then finds -- and the row that filled it was never read until now.
    Case exprInOwnEnvValue 1 'own `env:` block, where its own `env:` is not yet in force' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a typo in the convention"
        env:
          SCCACHE_PAHT: ${{ env.SCCACHE_PAHT }}
        run: echo "$SCCACHE_PAHT"
WF

    # A `$GITHUB_ENV` write reaches only LATER steps of the same job, so a write BELOW the read
    # supplies nothing -- and the expression is substituted before any script runs, so a write
    # in the SAME step supplies nothing either.
    Case exprWriteInLaterStep 1 'step "reads it first": reads ${{ env.LATER }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads it first"
        run: echo "${{ env.LATER }}"
      - name: "writes it after"
        run: echo "LATER=1" >> "$GITHUB_ENV"
WF

    Case exprWriteInAnotherJob 1 'reads ${{ env.ELSEWHERE }}' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: echo "ELSEWHERE=1" >> "$GITHUB_ENV"
  b:
    runs-on: ubuntu-latest
    steps:
      - name: "another job cannot see it"
        run: echo "${{ env.ELSEWHERE }}"
WF

    Case noRunStepAtAll 1 'not one `run:` step read' < /dev/null
    Case noWorkflowFile 1 'no workflow file under' plain nofile

    # ---- accepted -------------------------------------------------------------------------------------------------

    Case scopesThatReach 0 '3 run step(s), 3 read as bash' <<'WF'
---
env:
  WORKFLOW_WIDE: 1
jobs:
  a:
    runs-on: ubuntu-latest
    env:
      JOB_WIDE: 1
    steps:
      - name: "own env"
        if: ${{ github.event_name == 'push' &&
          github.ref == 'refs/heads/master' }}
        env:
          OWN: 1
        run: echo "$OWN $JOB_WIDE $WORKFLOW_WIDE $RUNNER_TEMP"
      - name: "an export"
        run: echo "TOOL_BIN=/opt/tool" >> "$GITHUB_ENV"
      - name: "named by its own env row"
        env:
          TOOL_BIN: ${{ env.TOOL_BIN }}
        run: echo "$TOOL_BIN"
WF

    # Job and workflow keys that follow the steps still reach them.
    Case keysAfterSteps 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  a:
    steps:
      - run: echo "$JOB_AFTER $WORKFLOW_AFTER"
    env:
      JOB_AFTER: 1
    runs-on: ubuntu-latest
env:
  WORKFLOW_AFTER: 1
WF

    # Each `run:` key is counted without the reader, so a step the reader stops placing is refused even when nothing
    # it reads is wrong; and a line in a script that looks like a key is text, not a key.
    Case runKeysCounted 0 '2 run: key(s) counted, 2 placed; 2 run step(s)' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: |
          cat > body.md <<BODY
          - run: ${RUNNER_TEMP}
          run: text
          BODY
      -   name: "wide"
          run: echo clean
WF

    # A comment may be all that follows the colon; the block below is still the value.
    Case commentAfterTheColon 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    env: # the rows below
      FROM_JOB: 1
    steps: # one
      - run: echo "$FROM_JOB"
WF

    # A workflow's bash is the runner's, not macOS's 3.2, so the staged script names what a workflow may use.
    # bash32-scan: data-begin
    Case bashAssignments 0 '1 read as bash' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    env:
      KEPT: a,b
    steps:
      - run: |
          set -euo pipefail
          bash scripts/x.sh || { c=$?; [ "$c" = 77 ] && echo skipped; exit "$c"; }
          IFS=',' read -ra before <<< "$KEPT"
          for label in "${before[@]}"; do echo "$label"; done
          mapfile -t lines < <(printf 'a\n')
          while getopts "a:" opt; do echo "$opt $OPTARG"; done
          declare -A seen; seen[x]=1
          arr=(); arr+=(1)
          if true; then flag=1; fi
          echo "${lines[@]} ${seen[x]} ${arr[@]} $flag ${#arr[@]}"
          read -r -d '' text <<'EOF' || true
          $NOT_A_READ inside a quoted heredoc
          EOF
          echo "$text \$ESCAPED"
          awk '{ print $NF }' file
          jq -n '
            $ENV.X
          '
WF
    # bash32-scan: data-end

    # This fixture already mirrored the real convention, `sccache-action` and all, so it stages
    # the row that supplies the name: the fixture table #1460 asks for, so that a synthetic case
    # is never judged against the real one. It therefore also covers an action export satisfying
    # a read in a step`s own `env:` VALUES, which nothing else here drives.
    #
    # Set and reset explicitly rather than as an assignment prefix on the call: bash keeps a
    # prefix assignment to a FUNCTION after the call outside POSIX mode, so the row would leak
    # into every case below it and satisfy reads none of them stages.
    ActionExports="mozilla-actions/sccache-action|SCCACHE_PATH"
    Case pwshModel 0 '0 read as bash, 2 as PowerShell' <<'WF'
env:
  FROM_WORKFLOW: 1
jobs:
  w:
    runs-on: windows-2025
    steps:
      - uses: mozilla-actions/sccache-action@v0.0.11
      - name: "runner default on Windows is pwsh"
        env:
          SCCACHE_PATH: ${{ env.SCCACHE_PATH }}
        run: |
          $stats = & $env:SCCACHE_PATH --show-stats | ConvertFrom-Json
          if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
          $env:MINE = "x"
          Write-Host $env:mine $Env:ProgramData ${env:FROM_WORKFLOW}
          $literal = '$env:NOT_A_READ'
          $here = @'
          $env:NOT_A_READ_EITHER
          '@
      - shell: pwsh
        run: echo "done"
WF
    ActionExports=""

    Case workflowDefaultsShell 0 '0 read as bash, 1 as PowerShell' <<'WF'
defaults:
  run:
    shell: pwsh
jobs:
  a:
    runs-on: ubuntu-latest
    needs:
      - other
    steps:
      - run: |
          $local = 1
          Write-Host $local
WF

    # ---- a job MATRIX (#1432) -- what must NOT be refused ---------------------------------------------------------

    # #1432's shape: the include row would overwrite the ORIGINAL `runner`, so it is a third combination, on an arm64
    # image -- and every combination's default shell is bash.
    Case matrixShellsAgree 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  linux:
    name: "Linux-${{ matrix.preset }}${{ matrix.suffix }}"
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        preset: [clang-release, gcc-release]
        runner: [ubuntu-24.04]
        include:
          - preset: gcc-release
            runner: ubuntu-24.04-arm
            suffix: "-arm64"
    steps:
      - name: "on every leg"
        run: echo "$RUNNER_TEMP"
WF

    # The remedy the refusal above names, followed: a shell named on the step is the shell, whatever the runners say.
    Case matrixShellsDisagreeNamed 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        runner: [ubuntu-24.04, windows-2025]
    steps:
      - name: "which shell"
        shell: bash
        run: echo hi
WF

    # An include row naming only keys that are not axes MERGES into every combination rather than adding one -- a
    # model that appended it would give that combination no runner and refuse the step, as the case above does.
    Case matrixIncludeMerges 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  a:
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        runner: [ubuntu-24.04, macos-15]
        include:
          - note: every leg
    steps:
      - run: echo hi
WF

    # A matrix written AFTER the steps still reaches them: the second pass starts the job from the combinations the
    # first pass computed, as it already does for `runs-on` and the job's `env:`.
    Case matrixAfterSteps 0 '1 run step(s), 0 read as bash, 1 as PowerShell' <<'WF'
jobs:
  w:
    steps:
      - run: Write-Host hi
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [windows-2025, windows-11-arm]
WF

    # ---- ORDER (#1461) -- what the ordering must NOT refuse -------------------------------------------------------
    #
    # Every refusing case above has its twin here, the same script with the order or the quoting that makes it
    # legitimate: a model that refused everything would pass the refusing half on its own.

    # The first half of the remedy: move the assignment above the read.
    Case orderAssignThenRead 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "the assignment above the read"
        run: |
          TARGET=release
          echo "$TARGET"
WF

    # The second half of it, tested because a remedy nobody has followed is not known to work: the same script as
    # `orderTopLevelRead`, with the row the refusal asks for.
    Case orderRemedyStepEnv 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "the step env carries it"
        env:
          TARGET: release
        run: |
          echo "$TARGET"
          TARGET=other
WF

    # A loop body runs AGAIN, so the assignment at its end reaches the read at its start.
    Case orderLoopBody 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a loop body sees what its own next turn assigns"
        run: |
          for path in a b
          do
            echo "$previous"
            previous="$path"
          done
WF

    # A function body runs where it is CALLED, which is below the assignment its caller makes.
    Case orderFunctionBody 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a function body sees what its caller assigns before the call"
        run: |
          emit() {
            echo "$subject"
          }
          subject=one
          emit
WF

    # The twin of `orderTrapDoubleQuoted`: single quotes are not expanded, so the lexer masks the `$` and there is
    # no read in the handler to order at all. This is the whole of why `trap` needs no relaxation.
    Case orderTrapSingleQuoted 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "a single-quoted trap handler is not expanded, so no read is seen in it"
        run: |
          trap 'echo "$reaper"' EXIT
          reaper=1
WF

    # The grain, pinned as a PASSING case so that the limit stays a decision: a read and an assignment on ONE line
    # are not ordered against each other. Delete this case and the limit becomes an accident.
    Case orderOneLineLoopGrain 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "the grain is a line"
        run: |
          for x in a b; do echo "$seen"; seen=1; done
WF

    # And PowerShell's limit, pinned the same way: every brace-enclosed block is a construct there.
    Case orderPwshBraceBlock 0 '1 run step(s), 0 read as bash, 1 as PowerShell' <<'WF'
jobs:
  j:
    runs-on: windows-2022
    steps:
      - name: "a PowerShell brace block is a construct"
        shell: pwsh
        run: |
          foreach ($i in 1..2) {
            Write-Host $env:LATER
            $env:LATER = "x"
          }
WF

    # ---- EXPRESSIONS (#1460) -- what must NOT be refused ----------------------------------------------------------

    # All four fields, from all three scopes at once. The twin of the four refusing cases above:
    # a reader that refused everything would pass those and fail this.
    Case exprFromScopes 0 '2 run step(s), 2 read as bash' <<'WF'
env:
  FROM_WORKFLOW: 1
jobs:
  j:
    runs-on: ubuntu-latest
    env:
      FROM_JOB: 1
    steps:
      - name: "install ${{ env.FROM_WORKFLOW }}"
        if: ${{ env.FROM_JOB == '1' }}
        env:
          OWN: 1
        run: |
          echo "${{ env.FROM_WORKFLOW }} ${{ env.FROM_JOB }} ${{ env.OWN }} $OWN"
      - name: "a with: value from a scope"
        uses: actions/cache@v4
        with:
          path: ${{ env.FROM_WORKFLOW }}
          key: fixed
      - run: echo ok
WF

    # The legitimate form of the convention: a step `env:` VALUE may read a workflow or job name,
    # just not one from its own block. Without this case the rule above could be implemented as
    # "refuse every expression in a step env value" and every test would still pass.
    Case exprOwnEnvFromScope 0 '1 run step(s), 1 read as bash' <<'WF'
env:
  FROM_WORKFLOW: 1
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "the convention, spelled right"
        env:
          MINE: ${{ env.FROM_WORKFLOW }}
        run: echo "$MINE"
WF

    # A write in an EARLIER step of the same job, in the one spelling this tree uses.
    Case exprFromEarlierWrite 0 '2 run step(s), 2 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "writes it"
        run: echo "TIDY_BIN=/usr/bin/x" >> "$GITHUB_ENV"
      - name: "reads it after"
        env:
          TIDY: ${{ env.TIDY_BIN }}
        run: echo "$TIDY"
WF

    # The PowerShell spellings, one case each, because the one rule that covers them -- a line
    # naming GITHUB_ENV writes every `NAME=` on it -- is worth nothing if a spelling never
    # reaches it.
    Case exprFromPwshRedirectWrite 0 '0 read as bash, 2 as PowerShell' <<'WF'
jobs:
  j:
    runs-on: windows-2025
    steps:
      - name: "writes it, pwsh"
        run: |
          "MADE=1" >> $env:GITHUB_ENV
      - name: "reads it after"
        env:
          MINE: ${{ env.MADE }}
        run: Write-Host $env:MINE
WF

    Case exprFromAddContentWrite 0 '0 read as bash, 2 as PowerShell' <<'WF'
jobs:
  j:
    runs-on: windows-2025
    steps:
      - name: "writes it with Add-Content"
        run: Add-Content -Path $env:GITHUB_ENV -Value "ADDED=1"
      - name: "reads it after"
        env:
          MINE: ${{ env.ADDED }}
        run: Write-Host $env:MINE
WF

    # ---- EXPRESSIONS (#1460) -- the table, and the count that says the reader ran ---------------------------------
    #
    # An `ActionExports` row goes stale in TWO directions and they fail separately: its action
    # stops being used, or its name stops being read. Each is its own case, and each has the
    # coherent tree as its twin -- a check that refused every row would pass both refusals.

    # Both pass 1 as their sixth argument: staleness is a whole-tree claim, so it is not made
    # over a staged directory unless the case asks for it.
    ActionExports="some/retired-action|EXPORTED_NAME"
    Case exprStaleExportAction 1 'no step `uses:` that action any more' plain '' 1 <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads the name, but nothing uses the action"
        env:
          MINE: ${{ env.EXPORTED_NAME }}
        run: echo "$MINE"
WF
    ActionExports=""

    # This fixture carries a SATISFIED read as well, so the zero-read guard -- which the same
    # knob turns on -- cannot be what refuses it. Without that read the case would exit 1 for
    # two reasons and the expected text would still match, which is the right side for the
    # wrong reason.
    ActionExports="some/live-action|UNREAD_NAME"
    Case exprStaleExportName 1 'expression reads that name any more' plain '' 1 <<'WF'
env:
  SUPPLIED: 1
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - uses: some/live-action@v1
      - run: echo "${{ env.SUPPLIED }}"
WF
    ActionExports=""

    # The twin of both: the action IS used and the name IS read, so the row describes something
    # and the read it supplies is accepted. This is also the only case that drives an export
    # satisfying a read in a step`s `run:` rather than in its `env:`.
    ActionExports="some/live-action|EXPORTED_NAME"
    Case exprFromActionExport 0 '2 run step(s), 2 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - uses: some/live-action@v1
      - name: "reads what the action exported"
        run: echo "${{ env.EXPORTED_NAME }}"
      - run: echo ok
WF
    ActionExports=""

    # An action export reaches the steps AFTER it and no earlier: the same rule as a
    # `$GITHUB_ENV` write, and for the same reason -- the action has not run yet.
    ActionExports="some/live-action|EXPORTED_NAME"
    Case exprExportReachesOnlyLaterSteps 1 'reads ${{ env.EXPORTED_NAME }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "reads it before the action runs"
        run: echo "${{ env.EXPORTED_NAME }}"
      - uses: some/live-action@v1
WF
    ActionExports=""

    # A step cannot read what IT exports or writes, and these two cases are what says so. The
    # ones above put the read in one step and the source in another, so they discriminate WHICH
    # STEP and not WHEN IN THE STEP -- a neuter that adopted an export or a write before judging
    # reddened neither of them, which is how these came to be written.
    ActionExports="some/live-action|EXPORTED_NAME"
    Case exprExportNotInItsOwnStep 1 'reads ${{ env.EXPORTED_NAME }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - uses: some/live-action@v1
        with:
          path: ${{ env.EXPORTED_NAME }}
      - run: echo ok
WF
    ActionExports=""

    Case exprWriteNotInItsOwnStep 1 'reads ${{ env.SELF }}' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - name: "writes it and reads it in one step"
        env:
          MINE: ${{ env.SELF }}
        run: echo "SELF=1" >> "$GITHUB_ENV"
WF

    # Reading NO expression at all is a refusal for the repository`s own workflows, which carry
    # dozens, and ordinary for a staged tree -- so the guard is driven here explicitly, in both
    # directions, over the same workflow. Without the refusing arm the guard is untested; without
    # the accepting one, every other case here would have to carry an expression it is not about.
    Case exprNoneReadRefused 1 'not one `${{ env.NAME }}` expression was read' plain '' 1 <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - run: echo ok
WF

    Case exprNoneReadAllowed 0 '1 run step(s), 1 read as bash' <<'WF'
jobs:
  j:
    runs-on: ubuntu-latest
    steps:
      - run: echo ok
WF

    # ---- the plant ----------------------------------------------------------------------------------------------

    # Two of the jobs name their runner and shell after their steps, which the plant must reach as well.
    Case plantSeenEverywhere 0 'all 5 run step(s) a model read in 1 workflow file(s) refused the planted read (3 bash, 2 PowerShell)' plant <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: echo "$UNDEFINED_BUT_UNPLANTED"
      - run: |
          cat <<'EOF'
          text
          EOF
  w:
    runs-on: windows-2025
    steps:
      - run: Write-Host hi
  laterRunner:
    steps:
      - run: Write-Host $env:FROM_LATER
    env:
      FROM_LATER: 1
    runs-on: windows-2025
  laterShell:
    steps:
      - run: echo "$FROM_LATER"
    runs-on: windows-2025
    env:
      FROM_LATER: 1
    defaults:
      run:
        shell: bash
WF

    # A body that never closes its quote hides everything after it: the plant must say so.
    Case plantHiddenByAnOpenQuote 1 'was NOT seen' plant <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "open quote"
        run: |
          echo 'never closed
WF

    # ---- composite actions --------------------------------------------------------------------------------------
    # Each case is a scratch repository, because the file set is what git TRACKS: an action on disk that is not
    # tracked is not one a workflow can use, and a directory walk would read it.

    # Write stdin to a path in a case's repository, and track it unless told not to.
    # @param 1 Case name. @param 2 Path in the repository. @param 3 `untracked` to leave it out of the index.
    Stage() {
        local path="$tmp/$1/$2"
        if ! { { [ -d "${path%/*}" ] || mkdir -p "${path%/*}"; } && WriteStdinTo "$path" \
            && { [ -d "$tmp/$1/.git" ] || git -C "$tmp/$1" init -q >/dev/null 2>&1; } \
            && { [ "${3:-}" = untracked ] || git -C "$tmp/$1" add -- "$2" >/dev/null 2>&1; }; }; then
            failures=$((failures + 1))
            echo "  $1: could not stage $2 in a scratch repository"
        fi
    }
    # Ask for the tracked composite actions of a case's directory. No repository above the scratch directory is
    # consulted, so a case staging none is not one.
    # @param 1 Case name. @param 2 Expected status (0 or 1). @param 3 Text the output must hold.
    # @param 4 `script` to run the copy of this script staged in the case's repository, as ctest runs it, rather than
    #          the function alone.
    ActionCase() {
        local name="$1" want="$2" expect="$3" how="${4:-function}" out got
        [ -d "$tmp/$name" ] || mkdir -p "$tmp/$name"
        # A case tree is a repository, so it owns the one answer to what is third-party. Staged here rather than per
        # case, because a case that forgot it would refuse for the wrong reason; `rootsUnreadable` is the case that
        # deliberately has none.
        if [ ! -e "$tmp/$name/scripts/lib/third-party-roots.txt" ] && [ "$name" != rootsUnreadable ]; then
            { [ -d "$tmp/$name/scripts/lib" ] || mkdir -p "$tmp/$name/scripts/lib"; } && printf 'vendor/endo
' > "$tmp/$name/scripts/lib/third-party-roots.txt"
        fi
        if [ "$how" = script ]; then
            out="$(GIT_CEILING_DIRECTORIES="$tmp" bash "$tmp/$name/scripts/check-workflow-step-env.sh" 2>&1)"
        else
            out="$(GIT_CEILING_DIRECTORIES="$tmp" CompositeActions "$tmp/$name" 2>&1)"
        fi
        got=$?
        ran=$((ran + 1))
        if [ "$got" != "$want" ]; then
            failures=$((failures + 1))
            printf '  %s: exited %s, wanted %s\n%s\n' "$name" "$got" "$want" "$out"
        elif [[ "$out" != *"$expect"* ]]; then
            failures=$((failures + 1))
            printf '  %s: the right side for the wrong reason, no `%s` in:\n%s\n' "$name" "$expect" "$out"
        fi
    }

    Stage compositeActionTracked README.md <<< "readme"
    Stage compositeActionTracked .github/actions/build/action.yml <<'ACTION'
name: build
runs:
  using: "composite"
  steps:
    - shell: bash
      run: echo "$NOBODY_JUDGES_THIS"
ACTION
    ActionCase compositeActionTracked 1 '.github/actions/build/action.yml: a composite action'

    # A path git would QUOTE in a plain listing.
    local unquoted=".github/actions/b"$'\303\274'"hne/action.yml"
    Stage quotedPath "$unquoted" <<'ACTION'
runs:
  using: composite
ACTION
    ActionCase quotedPath 1 "${unquoted}: a composite action"

    Stage untrackedComposite README.md <<< "readme"
    Stage untrackedComposite action.yaml <<'ACTION'
runs:
  using: node20 # a JavaScript action runs no shell
  main: index.js
ACTION
    Stage untrackedComposite .github/actions/local/action.yml untracked <<'ACTION'
runs:
  using: composite
  steps: []
ACTION
    ActionCase untrackedComposite 0 '2 first-party tracked file(s) listed by git ls-files, 1 action file(s), 0 refused'

    Stage unreadableUsing action.yml <<'ACTION'
runs: { using: composite, steps: [] }
ACTION
    ActionCase unreadableUsing 1 '`runs.using` reads ``'

    # A composite action under a third-party root is not this project's to judge: DECLINED, and named.
    Stage vendoredComposite README.md <<< "readme"
    Stage vendoredComposite vendor/endo/.github/actions/build/action.yml <<'ACTION'
runs:
  using: composite
  steps: []
ACTION
    ActionCase vendoredComposite 0 'declined 1 third-party tracked file(s)'

    # And the roots file is load-bearing: read as nothing, the case above would be refused as one of ours.
    Stage rootsUnreadable README.md <<< "readme"
    Stage rootsUnreadable vendor/endo/.github/actions/build/action.yml <<'ACTION'
runs:
  using: composite
ACTION
    ActionCase rootsUnreadable 1 'third-party roots of'

    Stage nothingTracked notes.txt untracked <<< "not tracked"
    ActionCase nothingTracked 1 'listed no tracked file'

    ActionCase notARepository 1 'git ls-files exited'

    # The unplanted run asks, not only the function: a clean workflow beside a tracked composite action is refused.
    Stage askedByTheRun scripts/check-workflow-step-env.sh < "${BASH_SOURCE[0]}"
    Stage askedByTheRun scripts/lib/third-party-roots.sh < "$(dirname "${BASH_SOURCE[0]}")/lib/third-party-roots.sh"
    # The two awk programs, because since #1456 the reader is FILES rather than an inline string
    # and a staged tree that omits them runs no reader at all. The check refuses that by name, so
    # this case failed loudly on the extraction rather than passing over a scan of nothing --
    # which is the whole reason that refusal is there.
    Stage askedByTheRun scripts/lib/shell-lex.awk < "${FastCachedShellLexAwk}"
    Stage askedByTheRun scripts/lib/workflow-walk.awk < "${FastCachedWorkflowWalkAwk}"
    Stage askedByTheRun scripts/check-workflow-step-env.awk < "${FastCachedStepEnvAwk}"
    Stage askedByTheRun .github/workflows/wf.yml <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - uses: ./.github/actions/setup
      - run: echo clean
WF
    Stage askedByTheRun .github/actions/setup/action.yml <<'ACTION'
runs:
  using: composite
ACTION
    ActionCase askedByTheRun 1 '.github/actions/setup/action.yml: a composite action' script

    if [ "$failures" -ne 0 ]; then
        echo "check-workflow-step-env --self-test: ${ran} case(s) ran, ${failures} did not judge as they must"
        exit 1
    fi
    echo "check-workflow-step-env --self-test: ${ran} case(s) ran, every verdict as it must be"
    exit 0
}

# The self-test runs after the loop, so a --workflows written after --self-test is still read -- and the ctest
# registration passes --self-test straight after the script, which is how check-selftest-registered knows it is run.
dir=""
mode="plain"
self_test="no"
while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) self_test="yes"; shift ;;
        --plant) mode="plant"; shift ;;
        --workflows) [ $# -ge 2 ] || { echo "--workflows needs a directory"; exit 2; }; dir="$2"; JudgingTheRealTree=0; shift 2 ;;
        *) echo "usage: bash scripts/check-workflow-step-env.sh [--plant] [--workflows <dir>] | --self-test"; exit 2 ;;
    esac
done

if [ "$self_test" = "yes" ]; then
    SelfTest
fi
Judge "${dir:-$workflows_dir}" "$mode"
status=$?
if [ -n "$dir" ]; then
    echo "workflow-step-env: actions: not asked -- --workflows names a directory, not a repository"
elif [ "$mode" = plant ]; then
    echo "workflow-step-env: actions: not asked by the plant run -- a tracked composite action is the unplanted run's to refuse"
else
    CompositeActions "$repo_root" || status=1
fi
exit "$status"
