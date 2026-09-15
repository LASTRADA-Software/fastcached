#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every name a workflow `run:` step's script READS from the environment is one that step can see (#1448, #1175).
#
# Usage:
#   bash scripts/check-workflow-step-env.sh [--workflows <dir>]
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
# tree's 135 `run:` steps: exact about the file it knew and silent about the other five, which reads identically to
# complete coverage. So the files are a GLOB, every file it finds must be read, and the count of each is printed.
#
# ## What a step can see
#
#   * the workflow's `env:`, its job's `env:` (only its own job's), and its own `env:`;
#   * names its own script assigns (bash: `NAME=`, `NAME+=` in command position, a declarator such as `export` or
#     `local`, `read`, `for NAME in`, `mapfile`/`readarray`, `getopts`; PowerShell: `$env:NAME =`);
#   * names an EARLIER step of the same job exported: `echo "NAME=..." >> "$GITHUB_ENV"` in a bash step, or an
#     action named in `ActionExports` below, whose source was read to say what it exports;
#   * the runner's and the operating system's own vocabulary, which is an ALLOWLIST per shell family and fails
#     CLOSED -- a name nobody has listed is refused and gets added deliberately, where an open pattern would have
#     admitted `EVENT`.
#
# ## Which shell reads the script
#
# The step's `shell:`, else its job's `defaults.run.shell`, else the workflow's, else the runner's default for a
# literal `runs-on` (pwsh on Windows, bash elsewhere). A step whose shell cannot be resolved that way, or names a
# shell `ShellModels` has no row for, is REFUSED rather than read by the wrong model:
#
#   * bash reads `$NAME` and `${NAME...}`, outside single quotes, quoted heredocs, and `\$`.
#   * PowerShell reads the environment only as `$env:NAME` or `${env:NAME}`, case-insensitively, outside single
#     quotes and single-quoted here-strings; a plain `$name` is a PowerShell variable and is not the environment.
#
# The models are deliberately NARROW -- a model more permissive than the shell waves through the one read that
# matters -- and every rule in them is a self-test case in both directions.
#
# ## What it does NOT cover, so that nobody reads a pass as more than it is
#
# A name reached by `eval` or indirect expansion; one a sourced file defines; one supplied through a `${{ }}`
# expression (substituted, so invisible here); `${#arr[@]}`; a name exported to `$GITHUB_ENV` in any spelling but
# `echo`/`printf` of `NAME=` (a later read of it is refused, which fails closed); an action's exports it has no row
# for (likewise refused); and composite actions' own steps.
#
# bash 3.2 and a POSIX awk: this runs on macOS's `/bin/bash` and BSD awk.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workflows_dir="${repo_root}/.github/workflows"

# The runner's own vocabulary for a bash step, and the names bash sets itself. Both scans over `scripts/` read this
# list as code, and it is data: it spells `BASHPID` (bash 4.0+) and `SECONDS` (a clock reading), neither of which
# this script executes.
# bash32-scan: data-begin
# seconds-scan: data-begin
PosixProvided="HOME PATH PWD SHELL USER TMPDIR LANG IFS OSTYPE HOSTNAME
RUNNER_TEMP RUNNER_OS RUNNER_ARCH RUNNER_TOOL_CACHE RUNNER_WORKSPACE RUNNER_DEBUG
GITHUB_OUTPUT GITHUB_ENV GITHUB_PATH GITHUB_STEP_SUMMARY GITHUB_WORKSPACE
GITHUB_REPOSITORY GITHUB_SHA GITHUB_REF GITHUB_REF_NAME GITHUB_EVENT_NAME
GITHUB_EVENT_PATH GITHUB_RUN_ID GITHUB_RUN_NUMBER GITHUB_ACTOR GITHUB_JOB
GITHUB_SERVER_URL GITHUB_API_URL GITHUB_HEAD_REF GITHUB_BASE_REF GITHUB_TOKEN
BASH_VERSION BASH_SOURCE BASH_REMATCH BASHPID FUNCNAME LINENO RANDOM SECONDS PIPESTATUS OPTARG OPTIND"
# seconds-scan: data-end
# bash32-scan: data-end

# The runner's vocabulary for a PowerShell step, compared case-insensitively as Windows does, plus what Windows sets
# for every process. A row is added for a name the tree reads, never pre-emptively.
WindowsProvided="RUNNER_TEMP RUNNER_OS RUNNER_ARCH RUNNER_TOOL_CACHE RUNNER_WORKSPACE RUNNER_DEBUG
GITHUB_OUTPUT GITHUB_ENV GITHUB_PATH GITHUB_STEP_SUMMARY GITHUB_WORKSPACE
GITHUB_REPOSITORY GITHUB_SHA GITHUB_REF GITHUB_REF_NAME GITHUB_EVENT_NAME
GITHUB_EVENT_PATH GITHUB_RUN_ID GITHUB_RUN_NUMBER GITHUB_ACTOR GITHUB_JOB
GITHUB_SERVER_URL GITHUB_API_URL GITHUB_HEAD_REF GITHUB_BASE_REF GITHUB_TOKEN
PATH TEMP TMP USERPROFILE LOCALAPPDATA APPDATA PROGRAMDATA PROGRAMFILES SYSTEMROOT WINDIR COMPUTERNAME"

# action|names it exports to the later steps of its job|where that was read. Keyed on the action without its
# `@ref`; a row is a claim about the action's SOURCE, so it names the file and the refs it was read at.
ActionExports="mozilla-actions/sccache-action|SCCACHE_PATH|core.exportVariable('SCCACHE_PATH', ...) in src/setup.ts at v0.0.6 and v0.0.11"

# first word of a shell: value|model. `bash {0}`, `bash --noprofile --norc -eo pipefail {0}` and plain `bash` all
# start with the word; a shell with no row is refused by name.
ShellModels="bash|bash
sh|bash
pwsh|pwsh
powershell|pwsh"

# kind|remedy -- the text a refusal of that kind ends with.
Remedies="undefined|A step's environment is its own: a sibling step's or another job's \`env:\` does not lend it one, so under \`set -u\` the step dies on that line and without it the name expands to EMPTY and a branch is silently taken the wrong way (#1174). Add the row to THIS step's \`env:\` (or its job's), or assign it in the script.
unknown-shell|This check reads a script by the model of the shell that runs it, and has none for this one. Add a row to ShellModels only with a model of how that shell reads its environment, and cases for it.
unresolved-shell|The shell could not be derived: no \`shell:\` on the step, no \`defaults.run.shell\` above it, and a \`runs-on\` that is not a literal Windows, Ubuntu or macOS runner. Name the shell on the step.
unreadable-run|The \`run:\` value is not a string this check can read (an alias, a tag, or a flow collection). Write the script as a block scalar (\`run: |\`) or an inline string.
unreadable-env|The \`env:\` is not a block mapping this check can read. Write one \`NAME: value\` row per line."

problems=0

# Scan one workflow file; print `STEPS\t<count>` and one `REFUSE\t<line>\t<step>\t<kind>\t<detail>` per refusal.
# @param 1 The workflow file.
Scan() {
    awk \
        -v posix="$(printf '%s' "$PosixProvided" | tr '\n' ' ')" \
        -v windows="$(printf '%s' "$WindowsProvided" | tr '\n' ' ')" \
        -v actions="$(printf '%s' "$ActionExports" | tr '\n' '\036')" \
        -v shells="$(printf '%s' "$ShellModels" | tr '\n' ' ')" \
        -v plant="${2:-0}" -v PlantName="FASTCACHED_PLANTED_READ" '
    function indentOf(s,   n) { n = match(s, /[^ ]/); return n ? n - 1 : length(s) }
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function unquote(t,   sq, first, last) {
        sq = sprintf("%c", 39)
        first = substr(t, 1, 1); last = substr(t, length(t), 1)
        if (length(t) >= 2 && first == last && (first == "\"" || first == sq)) return substr(t, 2, length(t) - 2)
        return t
    }
    function refuse(line, kind, detail) {
        printf "REFUSE\t%d\t%s\t%s\t%s\n", line, stepName, kind, detail
    }

    # ---- the text a shell would expand, with what it would not expand blanked -----------------------------------
    # One lexer state per body, carried across lines, because a quoted string may span them. Inside single quotes
    # every `$` becomes `_`, so nothing there reads a name while the words, the quotes and a heredoc delimiter stay
    # where they were -- `read -d '' name` still has a value before the name.
    function bashVisible(s,   out, i, c, n, heredocAt, t, quoted) {
        if (heredocEnd != "") {
            if (trim(s) == heredocEnd) { heredocEnd = ""; return "" }
            if (heredocQuoted) return ""
            # An unquoted heredoc expands, but a quote in it is a character, not a string.
            gsub(/\\\$/, "", s)
            return s
        }
        out = ""; n = length(s); heredocAt = 0
        for (i = 1; i <= n; i++) {
            c = substr(s, i, 1)
            if (lexState == "single") { if (c == sq) lexState = ""; out = out (c == "$" ? "_" : c); continue }
            if (c == "\\") { i++; continue }
            if (lexState == "" && c == sq) { lexState = "single"; out = out c; continue }
            if (c == "\"") lexState = (lexState == "double") ? "" : "double"
            if (lexState == "" && substr(s, i, 2) == "<<" && substr(s, i + 2, 1) != "<" && (i == 1 || substr(s, i - 1, 1) != "<"))
                if (!heredocAt) heredocAt = length(out) + 1
            out = out c
        }
        # A heredoc started on this line, outside any quote. Quoted, its body is not expanded at all; either way it
        # ends at the delimiter alone on a line.
        if (heredocAt) {
            t = substr(out, heredocAt + 2)
            sub(/^-?[ \t]*/, "", t)
            quoted = (substr(t, 1, 1) == sq || substr(t, 1, 1) == "\"")
            if (quoted) t = substr(t, 2)
            if (match(t, /^[A-Za-z_][A-Za-z0-9_]*/)) { heredocEnd = substr(t, 1, RLENGTH); heredocQuoted = quoted }
        }
        return out
    }
    function pwshVisible(s,   out, i, c, n, t) {
        t = trim(s)
        if (hereString == "single") { if (substr(t, 1, 2) == sq "@") hereString = ""; return "" }
        if (hereString == "double") { if (substr(t, 1, 2) == "\"@") { hereString = ""; return "" } return s }
        if (lexState == "" && substr(t, length(t) - 1) == "@" sq) { hereString = "single"; return substr(s, 1, length(s) - 2) }
        if (lexState == "" && substr(t, length(t) - 1) == "@\"") { hereString = "double"; return substr(s, 1, length(s) - 2) }
        out = ""; n = length(s)
        for (i = 1; i <= n; i++) {
            c = substr(s, i, 1)
            if (lexState == "single") { if (c == sq) lexState = ""; out = out (c == "$" ? "_" : c); continue }
            if (c == "`") { i++; continue }
            if (lexState == "" && c == sq) { lexState = "single"; out = out c; continue }
            if (c == "\"") lexState = (lexState == "double") ? "" : "double"
            out = out c
        }
        return out
    }

    # ---- bash ----------------------------------------------------------------------------------------------------
    function bashDefs(s,   t, rest, name) {
        t = s
        sub(/^[ \t]+/, "", t)
        if (t ~ /^(export|readonly|local|declare|typeset)[ \t]/) {
            sub(/^(export|readonly|local|declare|typeset)[ \t]+(-[A-Za-z]+[ \t]+)*/, "", t)
            while (match(t, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                defs[substr(t, RSTART, RLENGTH)] = 1
                rest = substr(t, RSTART + RLENGTH)
                sub(/^(\+?=([^ \t]*))/, "", rest)
                if (rest !~ /^[ \t]/) break
                sub(/^[ \t]+/, "", rest)
                t = rest
            }
        }
        # An assignment in COMMAND position only: at the start, or after `;`, `&&`, `||`, `|`, `{`, `(`, `!`,
        # `then`, `do` or `else`. A word followed by `=` anywhere else is an argument, such as `--port=1`.
        rest = s
        while (match(rest, /(^|[;&|({!][ \t]*|(^|[ \t;])(then|do|else)[ \t]+)[ \t]*[A-Za-z_][A-Za-z0-9_]*\+?=/)) {
            name = substr(rest, RSTART, RLENGTH)
            sub(/\+?=$/, "", name)
            sub(/^.*[^A-Za-z0-9_]/, "", name)
            defs[name] = 1
            rest = substr(rest, RSTART + RLENGTH)
        }
        # bash32-scan: data-begin
        if (match(s, /(mapfile|readarray)[ \t]+(-[A-Za-z]+([ \t]+[^ \t-][^ \t]*)?[ \t]+)*[A-Za-z_][A-Za-z0-9_]*/)) {
        # bash32-scan: data-end
            rest = substr(s, RSTART, RLENGTH)
            if (match(rest, /[A-Za-z_][A-Za-z0-9_]*$/)) defs[substr(rest, RSTART, RLENGTH)] = 1
        }
        if (match(s, /(^|[^A-Za-z0-9_])for[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+in([ \t]|$)/)) {
            rest = substr(s, RSTART, RLENGTH)
            sub(/^[^A-Za-z0-9_]*for[ \t]+/, "", rest)
            if (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) defs[substr(rest, RSTART, RLENGTH)] = 1
        }
        if (match(s, /(^|[^A-Za-z0-9_])getopts[ \t]+[^ \t]+[ \t]+[A-Za-z_][A-Za-z0-9_]*/)) {
            rest = substr(s, RSTART, RLENGTH)
            if (match(rest, /[A-Za-z_][A-Za-z0-9_]*$/)) defs[substr(rest, RSTART, RLENGTH)] = 1
        }
        rest = s
        while (match(rest, /(^|[^A-Za-z0-9_])read[ \t]+/)) {
            rest = substr(rest, RSTART + RLENGTH)
            while (match(rest, /^-[A-Za-z]+[ \t]+/)) {
                t = substr(rest, 1, RLENGTH)
                rest = substr(rest, RLENGTH + 1)
                # -a takes the ARRAY it reads into, which is a name; -d, -n, -N, -p, -t and -u take a value, which is
                # not one.
                if (t ~ /a[ \t]+$/ && match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) defs[substr(rest, 1, RLENGTH)] = 1
                if (t ~ /[adnNptu][ \t]+$/ && match(rest, /^[^ \t]+[ \t]*/)) rest = substr(rest, RLENGTH + 1)
            }
            while (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                defs[substr(rest, RSTART, RLENGTH)] = 1
                rest = substr(rest, RSTART + RLENGTH)
                if (rest !~ /^[ \t]/) break
                sub(/^[ \t]+/, "", rest)
            }
        }
    }
    function bashRefs(s,   name) {
        gsub(/\$\{\{[^}]*\}\}/, "", s)
        while (match(s, /\$\{?[A-Za-z_][A-Za-z0-9_]*/)) {
            name = substr(s, RSTART, RLENGTH)
            sub(/^\$\{?/, "", name)
            refs[name] = 1
            s = substr(s, RSTART + RLENGTH)
        }
    }
    function githubEnvWrites(s,   t) {
        if (s !~ />>[ \t]*"?\$\{?GITHUB_ENV/) return
        t = s
        if (match(t, envWriteRe)) {
            t = substr(t, RSTART, RLENGTH)
            sub(/=$/, "", t); sub(/^.*[^A-Za-z0-9_]/, "", t)
            exported[t] = 1
        }
    }

    # ---- PowerShell ----------------------------------------------------------------------------------------------
    function pwshScan(s,   t, name) {
        gsub(/\$\{\{[^}]*\}\}/, "", s)
        t = s
        while (match(t, /\$\{?[Ee][Nn][Vv]:[A-Za-z_][A-Za-z0-9_]*/)) {
            name = toupper(substr(t, RSTART, RLENGTH))
            sub(/^\$\{?ENV:/, "", name)
            rest = substr(t, RSTART + RLENGTH)
            if (rest ~ /^\}?[ \t]*=[^=]/) defs[name] = 1
            else refs[name] = 1
            t = rest
        }
    }

    # ---- one step --------------------------------------------------------------------------------------------------
    function resolveShell(   s, model, n, i, row) {
        s = stepShell != "" ? stepShell : (jobShell != "" ? jobShell : workflowShell)
        if (s == "") {
            if (runsOn ~ /\$\{\{/) return "?"
            if (tolower(runsOn) ~ /windows/) s = "pwsh"
            else if (tolower(runsOn) ~ /(ubuntu|macos)/) s = "bash"
            else return "?"
        }
        s = trim(unquote(trim(s)))
        sub(/[ \t].*$/, "", s)
        return (s in shellModel) ? shellModel[s] : "!" s
    }
    function flush(   model, i, name, missing, n, line) {
        if (!inStep) return
        if (hasRun) {
            scanned++
            model = resolveShell()
            if (model == "?") refuse(stepLine, "unresolved-shell", "runs-on `" runsOn "`")
            else if (substr(model, 1, 1) == "!") refuse(stepLine, "unknown-shell", "shell `" substr(model, 2) "`")
            else {
                split("", defs); split("", refs); lexState = ""; heredocEnd = ""; hereString = ""
                models[model]++
                # The plant is one more line, read LAST and by the same lexer, so a body that leaves a quote, a
                # heredoc or a here-string open hides it -- which is what the plant exists to catch.
                if (plant) body[nbody++] = (model == "bash") ? ": \"$" PlantName "\"" : "$null = $env:" PlantName
                for (i = 0; i < nbody; i++) {
                    if (body[i] ~ /^[ \t]*#/) continue
                    line = (model == "bash") ? bashVisible(body[i]) : pwshVisible(body[i])
                    if (model == "bash") { bashDefs(line); bashRefs(line) } else pwshScan(line)
                }
                missing = ""; n = 0
                if (plant) {
                    printf "PLANTED\t%d\t%s\t%d\n", stepLine, stepName, ((PlantName in refs) || (toupper(PlantName) in refs))
                    delete refs[PlantName]; delete refs[toupper(PlantName)]
                }
                for (name in refs) {
                    if (model == "bash") {
                        if ((name in stepEnv) || (name in jobEnv) || (name in workflowEnv) || (name in defs) || (name in posixSet) || (name in exported)) continue
                    } else {
                        if ((name in stepEnvU) || (name in jobEnvU) || (name in workflowEnvU) || (name in defs) || (name in windowsSet) || (toupperKeys(name))) continue
                    }
                    missing = missing (n++ ? ", " : "") "$" (model == "pwsh" ? "env:" : "") name
                }
                if (n) refuse(stepLine, "undefined", "reads " missing)
                if (model == "bash") for (i = 0; i < nbody; i++) githubEnvWrites(body[i])
            }
        }
        if (stepUses != "") {
            name = stepUses; sub(/@.*$/, "", name)
            if (name in actionExports) {
                n = split(actionExports[name], parts, " ")
                for (i = 1; i <= n; i++) if (parts[i] != "") { exported[parts[i]] = 1; exportedU[toupper(parts[i])] = 1 }
            }
        }
        inStep = 0
    }
    function toupperKeys(name) { return (name in exportedU) }
    function resetStep() {
        split("", stepEnv); split("", stepEnvU); split("", body)
        nbody = 0; hasRun = 0; inStep = 1; stepName = "(unnamed)"; stepLine = NR; stepShell = ""; stepUses = ""
    }
    function resetJob() {
        split("", jobEnv); split("", jobEnvU); split("", exported); split("", exportedU)
        jobShell = ""; runsOn = ""; inSteps = 0; itemIndent = -1; jobKeyIndent = -1; stepName = "(job)"
    }
    function addEnv(scope, name) {
        if (scope == "workflow") { workflowEnv[name] = 1; workflowEnvU[toupper(name)] = 1 }
        else if (scope == "job") { jobEnv[name] = 1; jobEnvU[toupper(name)] = 1 }
        else { stepEnv[name] = 1; stepEnvU[toupper(name)] = 1 }
    }

    BEGIN {
        sq = sprintf("%c", 39)
        envWriteRe = "(echo|printf)[ \t]+(-[a-z]+[ \t]+)?[\"" sq "]?[A-Za-z_][A-Za-z0-9_]*="
        quoteStartRe = "^[\"" sq "]"
        n = split(posix, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") posixSet[p[i]] = 1
        n = split(windows, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") windowsSet[toupper(p[i])] = 1
        n = split(shells, p, " "); for (i = 1; i <= n; i++) if (split(p[i], q, "|") == 2) shellModel[q[1]] = q[2]
        n = split(actions, p, "\036"); for (i = 1; i <= n; i++) if (split(p[i], q, "|") >= 2) actionExports[q[1]] = q[2]
        inStep = 0; inJobs = 0; jobIndent = -1; blockOwner = -1; inEnv = 0; inDefaults = 0; defaultsRun = -1
        workflowShell = ""; resetJob(); stepName = "(workflow)"
    }

    {
        raw = $0
        sub(/\r$/, "", raw)
        ind = indentOf(raw)
        blank = (raw ~ /^[ \t]*$/)

        # A block scalar owns every blank or more-indented line after its key.
        if (blockOwner >= 0) {
            if (blank || ind > blockOwner) {
                if (blockKind == "run") { t = raw; if (!blank) t = substr(raw, blockOwner + 1); body[nbody++] = t }
                next
            }
            blockOwner = -1
        }
        if (blank || raw ~ /^[ \t]*#/) next

        line = raw
        isItem = (line ~ /^[ ]*-([ \t]|$)/)

        # Leaving a context, by indentation.
        if (inEnv && ind <= envIndent && !(isItem && ind == envIndent)) inEnv = 0
        if (inDefaults && ind <= defaultsIndent) { inDefaults = 0; defaultsRun = -1 }
        if (inRunsOnList && ind <= runsOnIndent && !(isItem && ind == runsOnIndent)) inRunsOnList = 0
        if (inSteps && ind <= stepsIndent && !(isItem && (itemIndent < 0 || ind == itemIndent))) { flush(); inSteps = 0 }
        if (inJobs && ind == 0) { flush(); inJobs = 0 }

        if (isItem) {
            if (inRunsOnList) { t = line; sub(/^[ ]*-[ \t]*/, "", t); runsOn = runsOn " " unquote(trim(t)); next }
            if (inSteps && (itemIndent < 0 || ind == itemIndent)) {
                if (itemIndent < 0) itemIndent = ind
                flush(); resetStep()
            }
            sub(/-/, " ", line)
            ind = indentOf(line)
            if (line ~ /^[ \t]*$/) next
        }

        if (!match(line, /^[ ]*[A-Za-z_][A-Za-z0-9_.-]*[ \t]*:([ \t]|$)/)) next
        key = substr(line, RSTART, RLENGTH)
        sub(/^[ ]*/, "", key); sub(/[ \t]*:[ \t]*$/, "", key)
        value = substr(line, RSTART + RLENGTH)
        value = trim(value)
        if (value !~ quoteStartRe) sub(/[ \t]+#.*$/, "", value)

        if (inEnv && ind > envIndent) { addEnv(envScope, key); if (value ~ /^[|>]/) { blockOwner = ind; blockKind = "skip" } ; next }

        stepKey = (inStep && inSteps && ind == itemIndent + 2)
        if (value ~ /^[|>][-+0-9]*$/) {
            blockOwner = ind
            blockKind = (stepKey && key == "run") ? "run" : "skip"
            if (blockKind == "run") hasRun = 1
            next
        }

        if (ind == 0) {
            if (key == "env") { if (value != "" && value != "{}") refuse(NR, "unreadable-env", "workflow `env: " value "`"); else { inEnv = 1; envIndent = 0; envScope = "workflow" } }
            else if (key == "defaults") { inDefaults = 1; defaultsIndent = 0; defaultsScope = "workflow" }
            else if (key == "jobs") { inJobs = 1; jobIndent = -1 }
            next
        }

        if (inDefaults) {
            if (key == "run" && value == "") { defaultsRun = ind; next }
            if (defaultsRun >= 0 && ind > defaultsRun && key == "shell") {
                if (defaultsScope == "workflow") workflowShell = unquote(value); else jobShell = unquote(value)
            }
            next
        }

        if (inJobs) {
            if (jobIndent < 0) jobIndent = ind
            if (ind == jobIndent) { flush(); resetJob(); jobName = key; next }
            if (jobKeyIndent < 0 && ind > jobIndent) jobKeyIndent = ind
            if (ind == jobKeyIndent) {
                if (key == "env") { if (value != "" && value != "{}") refuse(NR, "unreadable-env", "job `env: " value "`"); else { inEnv = 1; envIndent = ind; envScope = "job" } }
                else if (key == "defaults") { inDefaults = 1; defaultsIndent = ind; defaultsScope = "job" }
                else if (key == "runs-on") { if (value == "") { inRunsOnList = 1; runsOnIndent = ind; runsOn = "" } else runsOn = unquote(value) }
                else if (key == "steps") { inSteps = 1; stepsIndent = ind; itemIndent = -1 }
                next
            }
            if (stepKey) {
                if (key == "name") { t = unquote(value); if (t != "") stepName = t }
                else if (key == "shell") stepShell = unquote(value)
                else if (key == "uses") stepUses = unquote(value)
                else if (key == "env") { if (value != "" && value != "{}") refuse(NR, "unreadable-env", "step `env: " value "`"); else { inEnv = 1; envIndent = ind; envScope = "step" } }
                else if (key == "run") {
                    hasRun = 1
                    if (value ~ /^[*&!\[{]/) { refuse(NR, "unreadable-run", "`run: " value "`"); hasRun = 0 }
                    else {
                        body[nbody++] = unquote(value)
                        # A plain scalar continues on more-indented lines.
                        blockOwner = ind; blockKind = "run"
                    }
                }
            }
        }
    }

    END { flush(); printf "STEPS\t%d\t%d\t%d\n", scanned, models["bash"], models["pwsh"] }
    ' "$1"
}

# Judge every workflow under @p dir; print one line per file and per refusal; return 0 clean, 1 refused.
# @param 1 The workflows directory.
# @param 2 `plant` to plant a read of an undefined name into every run step, which passes only when every step
#          refuses it: proof, on the real files, that each step is read by a model that sees a read.
Judge() {
    local dir="$1" mode="${2:-plain}" file report status files=0 steps=0 bashSteps=0 pwshSteps=0 counts
    local tag line step kind detail remedy planted=0 plantedSeen=0 unplanted=0 display
    for file in "$dir"/*.yml "$dir"/*.yaml; do
        [ -e "$file" ] || continue
        files=$((files + 1))
        display="${file#"$repo_root"/}"
        report="$(Scan "$file" "$([ "$mode" = plant ] && echo 1 || echo 0)")"
        status=$?
        counts="$(awk -F'\t' '$1 == "STEPS" { print $2, $3, $4 }' <<< "$report")"
        if [ "$status" -ne 0 ] || [ -z "$counts" ]; then
            echo "  FAIL: ${display}: the scan did not finish (awk exited ${status}), so none of its steps was judged"
            problems=$((problems + 1))
            continue
        fi
        set -- $counts
        steps=$((steps + $1)); bashSteps=$((bashSteps + $2)); pwshSteps=$((pwshSteps + $3))
        echo "workflow-step-env: ${display}: $1 run step(s), $2 read as bash, $3 as PowerShell"
        while IFS="$(printf '\t')" read -r tag line step kind detail; do
            case "$tag" in
                PLANTED)
                    # For a PLANTED record the fourth field is whether the planted read was seen.
                    planted=$((planted + 1))
                    if [ "$kind" = "1" ]; then
                        plantedSeen=$((plantedSeen + 1))
                    else
                        echo "  FAIL: ${display}:${line} step \"${step}\": a read planted at the end of its script was NOT seen -- the reading of this step ends inside a quote, a heredoc or a here-string, so a real read there would pass unseen"
                        problems=$((problems + 1))
                    fi
                    ;;
                REFUSE)
                    if [ "$mode" = plant ] && [ "$kind" = undefined ]; then
                        unplanted=$((unplanted + 1))
                        continue
                    fi
                    remedy="$(awk -F'|' -v k="$kind" '$1 == k { sub(/^[^|]*\|/, ""); print }' <<< "$Remedies")"
                    echo "  FAIL: ${display}:${line} step \"${step}\": ${detail} -- ${remedy}"
                    problems=$((problems + 1))
                    ;;
            esac
        done <<< "$report"
    done
    if [ "$files" -eq 0 ]; then
        echo "  FAIL: no workflow file under ${dir} -- a check that reads nothing reports clean, which is the state it exists to remove"
        return 1
    fi
    if [ "$steps" -eq 0 ]; then
        echo "  FAIL: ${files} workflow file(s) and not one \`run:\` step read -- the parser stopped understanding them"
        return 1
    fi
    if [ "$mode" = plant ]; then
        if [ "$planted" -ne "$steps" ]; then
            echo "  FAIL: the plant went into ${planted} of ${steps} run step(s); the others were read by no model"
            problems=$((problems + 1))
        fi
        if [ "$problems" -ne 0 ]; then
            echo "workflow-step-env: plant: ${problems} problem(s) -- ${plantedSeen} of ${steps} run step(s) refused the planted read"
            return 1
        fi
        echo "workflow-step-env: plant: all ${steps} run step(s) in ${files} workflow file(s) refused the planted read (${bashSteps} bash, ${pwshSteps} PowerShell); ${unplanted} unplanted problem(s) left to the unplanted run"
        return 0
    fi
    if [ "$problems" -ne 0 ]; then
        echo "workflow-step-env: ${problems} problem(s) across ${files} workflow file(s) and ${steps} run step(s)"
        return 1
    fi
    echo "workflow-step-env: ${files} workflow file(s), ${steps} run step(s) (${bashSteps} bash, ${pwshSteps} PowerShell), every environment name each reads is one it can see"
    return 0
}

# ---------------------------------------------------------------------------
SelfTest() {
    local tmp ran=0 failures=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/workflow-step-env-selftest.XXXXXX")" || { echo "cannot create a scratch directory"; exit 2; }
    trap 'rm -rf -- "$tmp"' EXIT

    # Judge the workflow on stdin, alone in a directory of its own.
    # @param 1 Case name. @param 2 Expected status (0 or 1). @param 3 Text the output must hold. @param 4 Mode.
    Case() {
        local name="$1" want="$2" expect="$3" mode="${4:-plain}" out got
        mkdir -p "$tmp/$name"
        cat > "$tmp/$name/wf.yml"
        out="$(problems=0; Judge "$tmp/$name" "$mode" 2>&1)"
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

    Case githubEnvBeforeItsWrite 1 'step "early": reads $TOOL_BIN' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - name: "early"
        run: echo "$TOOL_BIN"
      - name: "write"
        run: echo "TOOL_BIN=/opt/tool" >> "$GITHUB_ENV"
WF

    Case githubEnvOtherJob 1 'step "b reads": reads $TOOL_BIN' <<'WF'
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: echo "TOOL_BIN=/opt/tool" >> "$GITHUB_ENV"
  b:
    runs-on: ubuntu-latest
    steps:
      - name: "b reads"
        run: echo "$TOOL_BIN"
WF

    Case actionExportBeforeTheAction 1 'reads $env:SCCACHE_PATH' <<'WF'
jobs:
  w:
    runs-on: windows-2025
    steps:
      - name: "too early"
        run: '& $env:SCCACHE_PATH --show-stats'
      - uses: mozilla-actions/sccache-action@v0.0.11
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
          make AS_ARGUMENT=1 install
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
    runs-on: [self-hosted, linux, ubuntu]
    steps:
      - run: echo one
          "$ON_THE_NEXT_LINE"
WF

    Case noRunStepAtAll 1 'not one `run:` step read' < /dev/null

    # A directory with no workflow file in it is not a clean tree.
    mkdir -p "$tmp/noWorkflowFile"
    local out status
    out="$(problems=0; Judge "$tmp/noWorkflowFile" 2>&1)"
    status=$?
    ran=$((ran + 1))
    if [ "$status" -ne 1 ] || [[ "$out" != *"no workflow file under"* ]]; then
        failures=$((failures + 1)); printf '  noWorkflowFile: exited %s\n%s\n' "$status" "$out"
    fi

    # ---- accepted -------------------------------------------------------------------------------------------------

    Case scopesThatReach 0 '3 run step(s), 3 read as bash' <<'WF'
env:
  WORKFLOW_WIDE: 1
jobs:
  a:
    runs-on: ubuntu-latest
    env:
      JOB_WIDE: 1
    steps:
      - name: "own env"
        env:
          OWN: 1
        run: echo "$OWN $JOB_WIDE $WORKFLOW_WIDE $RUNNER_TEMP"
      - name: "a later step sees what an earlier one exported"
        run: echo "TOOL_BIN=/opt/tool" >> "$GITHUB_ENV"
      - run: echo "$TOOL_BIN"
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

    Case pwshModel 0 '0 read as bash, 2 as PowerShell' <<'WF'
env:
  FROM_WORKFLOW: 1
jobs:
  w:
    runs-on: windows-2025
    steps:
      - uses: mozilla-actions/sccache-action@v0.0.11
      - name: "runner default on Windows is pwsh"
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

    # ---- the plant ----------------------------------------------------------------------------------------------

    Case plantSeenEverywhere 0 'all 3 run step(s) in 1 workflow file(s) refused the planted read (2 bash, 1 PowerShell)' plant <<'WF'
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
        --workflows) [ $# -ge 2 ] || { echo "--workflows needs a directory"; exit 2; }; dir="$2"; shift 2 ;;
        *) echo "usage: bash scripts/check-workflow-step-env.sh [--plant] [--workflows <dir>] | --self-test"; exit 2 ;;
    esac
done

if [ "$self_test" = "yes" ]; then
    SelfTest
fi
Judge "${dir:-$workflows_dir}" "$mode"
exit $?
