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
# uses for `CLANG_TIDY_BIN`. That row is the only spelling that says, in the step, where the name comes from, and it
# asks for no model of every `$GITHUB_ENV` write and every action's exports, which would grow a row per action.
#
# ## Which shell reads the script
#
# The step's `shell:`, else its job's `defaults.run.shell`, else the workflow's, else the runner's default for a
# literal `runs-on` (pwsh on Windows, bash on Ubuntu, macOS or Linux). A step whose shell cannot be resolved that way,
# or names a shell `ShellModels` has no row for, is REFUSED rather than read by the wrong model:
#
#   * bash reads `$NAME` and `${NAME...}`, outside single quotes, quoted heredocs, and `\$`.
#   * PowerShell reads the environment only as `$env:NAME` or `${env:NAME}`, case-insensitively, outside single
#     quotes and single-quoted here-strings; a plain `$name` is a PowerShell variable and is not the environment.
#
# The models are deliberately NARROW -- a model more permissive than the shell waves through the one read that
# matters. The reader of the YAML is narrow the same way: a line it cannot place by indentation (a flow mapping, an
# alias, a quoted key, a second document) is REFUSED, never skipped.
#
# ## What it does NOT cover, so that nobody reads a pass as more than it is
#
# A name reached by `eval` or indirect expansion; one a sourced file defines; one supplied through a `${{ }}`
# expression (substituted, so invisible here); `${#arr[@]}`; and composite actions' own steps.
#
# bash 3.2 and a POSIX awk: this runs on macOS's `/bin/bash` and BSD awk.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

# kind|remedy -- the text a refusal of that kind ends with.
Remedies="undefined|A step's environment is its own: a sibling step's or another job's \`env:\` does not lend it one, so under \`set -u\` the step dies on that line and without it the name expands to EMPTY and a branch is silently taken the wrong way (#1174). Add the row to THIS step's \`env:\` (or its job's), or assign it in the script. A name an earlier step or an action exported is named the same way, as \`NAME: \${{ env.NAME }}\`.
unknown-shell|This check reads a script by the model of the shell that runs it, and has none for this one. Add a row to ShellModels only with a model of how that shell reads its environment, and cases for it.
unresolved-shell|The shell could not be derived: no \`shell:\` on the step, no \`defaults.run.shell\` above it, and a \`runs-on\` that is not a literal Windows, Ubuntu, macOS or Linux runner. Name the shell on the step.
unreadable-run|The \`run:\` value is not a string this check can read (an alias, a tag, or a flow collection). Write the script as a block scalar (\`run: |\`) or an inline string.
unreadable-env|The \`env:\` is not a block mapping this check can read. Write one \`NAME: value\` row per line.
unreadable-yaml|This check places every line of a workflow by its indentation, and cannot place this one -- a flow mapping, an alias, a quoted key, or a second document -- so no step around it can be judged. Write it as a block mapping; if the construct is needed, teach the reader and add a case."

# The tables as awk takes them: a -v value may not hold a newline on BSD awk.
Flatten() { printf '%s' "$1" | tr '\n' "$2"; }
bashNames="$(Flatten "$RunnerProvided $BashProvided" ' ')"
windowsNames="$(Flatten "$RunnerProvided $WindowsProvided" ' ')"
shellRows="$(Flatten "$ShellModels" ' ')"
remedyRows="$(Flatten "$Remedies" $'\036')"

problems=0

# Scan one workflow file. Prints `REFUSE\t<line>\t<step>\t<kind>\t<detail>\t<remedy>` per refusal, in plant mode
# `PLANTED\t<line>\t<step>\t<seen 0|1>` per step a model read, and last `STEPS\t<steps>\t<bash>\t<pwsh>`.
# @param 1 The workflow file.
# @param 2 1 to plant a read of an undefined name at the end of every step a model reads, else 0.
Scan() {
    awk -v bashNames="$bashNames" -v windowsNames="$windowsNames" -v shells="$shellRows" -v remedies="$remedyRows" \
        -v plant="$2" -v PlantName="FASTCACHED_PLANTED_READ" '
    function indentOf(s,   n) { n = match(s, /[^ ]/); return n ? n - 1 : length(s) }
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function unquote(t,   first, last) {
        first = substr(t, 1, 1); last = substr(t, length(t), 1)
        if (length(t) >= 2 && first == last && (first == "\"" || first == sq)) return substr(t, 2, length(t) - 2)
        return t
    }
    function refuse(at, kind, detail) {
        if (pass < 2) return
        printf "REFUSE\t%d\t%s\t%s\t%s\t%s\n", at, stepName, kind, detail, remedy[kind]
    }

    # ---- the text a shell would expand ------------------------------------------------------------------------------
    # One lexer state per body, carried across lines, because a quoted string may span them. Inside single quotes
    # every `$` becomes `_`, so nothing there reads a name while the words, the quotes and a heredoc delimiter stay
    # where they were -- `read -d '' name` still has a value before the name. @p esc is the escape character
    # (a backslash in bash, a backtick in PowerShell); @p heredocs says whether `<<` opens one.
    function lex(s, esc, heredocs,   out, i, c, n, heredocAt, t, quoted) {
        out = ""; n = length(s); heredocAt = 0
        for (i = 1; i <= n; i++) {
            c = substr(s, i, 1)
            if (lexState == "single") { if (c == sq) lexState = ""; out = out (c == "$" ? "_" : c); continue }
            if (c == esc) { i++; continue }
            # A `#` starting a word outside any quote begins a comment in both shells, so the rest of the line is
            # neither a read nor a quote -- an apostrophe in `# do not` must not open a string that hides the step.
            if (lexState == "" && c == "#" && (i == 1 || substr(s, i - 1, 1) ~ /[ \t;]/)) break
            if (lexState == "" && c == sq) lexState = "single"
            else if (c == "\"") lexState = (lexState == "double") ? "" : "double"
            else if (heredocs && lexState == "" && substr(s, i, 2) == "<<" && substr(s, i + 2, 1) != "<" && (i == 1 || substr(s, i - 1, 1) != "<"))
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
    function bashVisible(s) {
        if (heredocEnd != "") {
            if (trim(s) == heredocEnd) { heredocEnd = ""; return "" }
            if (heredocQuoted) return ""
            # An unquoted heredoc expands, but a quote in it is a character, not a string.
            gsub(/\\\$/, "", s)
            return s
        }
        return lex(s, "\\", 1)
    }
    function pwshVisible(s,   t) {
        t = trim(s)
        if (hereString == "single") { if (substr(t, 1, 2) == sq "@") hereString = ""; return "" }
        if (hereString == "double") { if (substr(t, 1, 2) == "\"@") { hereString = ""; return "" } return s }
        if (lexState == "" && substr(t, length(t) - 1) == "@" sq) { hereString = "single"; return substr(s, 1, length(s) - 2) }
        if (lexState == "" && substr(t, length(t) - 1) == "@\"") { hereString = "double"; return substr(s, 1, length(s) - 2) }
        return lex(s, "`", 0)
    }

    # ---- bash ----------------------------------------------------------------------------------------------------
    # The position just past the shell word @p r starts with: quotes, `$( )`, `( )` and `${ }` keep it going.
    function wordEnd(r,   i, n, c, q, depth) {
        n = length(r); q = ""; depth = 0
        for (i = 1; i <= n; i++) {
            c = substr(r, i, 1)
            if (q != "") { if (c == q) q = ""; continue }
            if (c == "\"" || c == sq) { q = c; continue }
            if (c == "(" || c == "{") { depth++; continue }
            if ((c == ")" || c == "}") && depth > 0) { depth--; continue }
            if (depth == 0 && (c == " " || c == "\t" || c == ";" || c == "&" || c == "|" || c == ")" || c == "}")) return i
        }
        return n + 1
    }
    function bashDefs(s,   t, rest, name, nchain) {
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
        # And only when nothing but a command separator follows the run of assignments: `CC=clang make` hands CC to
        # make alone, and a later `$CC` still reads an unset name.
        rest = s
        while (match(rest, /(^|[;&|({!][ \t]*|(^|[ \t;])(then|do|else)[ \t]+)[ \t]*[A-Za-z_][A-Za-z0-9_]*\+?=/)) {
            name = substr(rest, RSTART, RLENGTH)
            sub(/\+?=$/, "", name)
            sub(/^.*[^A-Za-z0-9_]/, "", name)
            split("", chain); nchain = 1; chain[1] = name
            rest = substr(rest, RSTART + RLENGTH)
            while (1) {
                rest = substr(rest, wordEnd(rest))
                sub(/^[ \t]+/, "", rest)
                if (!match(rest, /^[A-Za-z_][A-Za-z0-9_]*\+?=/)) break
                name = substr(rest, 1, RLENGTH)
                sub(/\+?=$/, "", name)
                chain[++nchain] = name
                rest = substr(rest, RLENGTH + 1)
            }
            if (rest == "" || rest ~ /^[;&|)}#]/) for (name in chain) defs[chain[name]] = 1
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

    # ---- PowerShell ----------------------------------------------------------------------------------------------
    function pwshScan(s,   name, rest) {
        gsub(/\$\{\{[^}]*\}\}/, "", s)
        while (match(s, /\$\{?[Ee][Nn][Vv]:[A-Za-z_][A-Za-z0-9_]*/)) {
            name = toupper(substr(s, RSTART, RLENGTH))
            sub(/^\$\{?ENV:/, "", name)
            rest = substr(s, RSTART + RLENGTH)
            if (rest ~ /^\}?[ \t]*=[^=]/) defs[name] = 1
            else refs[name] = 1
            s = rest
        }
    }

    # ---- one step --------------------------------------------------------------------------------------------------
    function resolveShell(   s) {
        s = stepShell != "" ? stepShell : (jobShell != "" ? jobShell : workflowShell)
        if (s == "") {
            if (runsOn ~ /\$\{\{/) return "?"
            if (tolower(runsOn) ~ /windows/) s = "pwsh"
            else if (tolower(runsOn) ~ /(ubuntu|macos|linux)/) s = "bash"
            else return "?"
        }
        sub(/[ \t].*$/, "", s)
        return (s in shellModel) ? shellModel[s] : "!" s
    }
    # The names a step of @p model can see: every scope, its own assignments, and its shell family allowlist.
    # PowerShell compares them upper-cased, as Windows does.
    function admit(src, fold,   k) { for (k in src) visible[fold ? toupper(k) : k] = 1 }
    function flush(   model, i, name, missing, n, fold, t) {
        if (!inStep) return
        inStep = 0
        if (pass < 2) return
        if (!hasRun) return
        scanned++
        model = resolveShell()
        if (model == "?") { refuse(stepLine, "unresolved-shell", "runs-on `" runsOn "`"); return }
        if (substr(model, 1, 1) == "!") { refuse(stepLine, "unknown-shell", "shell `" substr(model, 2) "`"); return }
        split("", defs); split("", refs); split("", visible); lexState = ""; heredocEnd = ""; hereString = ""
        models[model]++
        fold = (model == "pwsh")
        # The plant is one more line, read LAST and by the same lexer, so a body that leaves a quote, a heredoc or a
        # here-string open hides it -- which is what the plant exists to catch.
        if (plant) body[nbody++] = fold ? "$null = $env:" PlantName : ": \"$" PlantName "\""
        for (i = 0; i < nbody; i++) {
            if (body[i] ~ /^[ \t]*#/) continue
            if (fold) pwshScan(pwshVisible(body[i]))
            else { t = bashVisible(body[i]); bashDefs(t); bashRefs(t) }
        }
        if (plant) {
            printf "PLANTED\t%d\t%s\t%d\n", stepLine, stepName, (PlantName in refs)
            delete refs[PlantName]
        }
        admit(stepEnv, fold); admit(jobEnv, fold); admit(workflowEnv, fold); admit(defs, fold)
        if (fold) admit(windowsSet, fold); else admit(bashSet, fold)
        missing = ""; n = 0
        for (name in refs) if (!(name in visible)) missing = missing (n++ ? ", " : "") (fold ? "$env:" : "$") name
        if (n) refuse(stepLine, "undefined", "reads " missing)
    }
    function resetStep() {
        split("", stepEnv); split("", body)
        nbody = 0; hasRun = 0; inStep = 1; stepName = "(unnamed)"; stepLine = FNR; stepShell = ""; stepKeyIndent = -1
    }
    # The `env:`, `defaults` and `runs-on` may follow its `steps:`, so the second pass starts each job from what the
    # first pass found anywhere in that job.
    function resetJob(   k, kp) {
        split("", jobEnv)
        jobShell = ""; runsOn = ""; inSteps = 0; itemIndent = -1; jobKeyIndent = -1; stepName = "(job)"; jobStart = FNR
        if (pass < 2) return
        for (k in jobEnvAt) { split(k, kp, SUBSEP); if (kp[1] == jobStart) jobEnv[kp[2]] = 1 }
        if (jobStart in jobShellAt) jobShell = jobShellAt[jobStart]
        if (jobStart in runsOnAt) runsOn = runsOnAt[jobStart]
    }
    # Every state the reader carries, reset at the start of each pass. The workflow scope is not: `env:` and
    # `defaults` may follow `jobs:`, and the first pass is what the second one reads them from.
    function resetAll() {
        inStep = 0; inJobs = 0; jobIndent = -1; blockOwner = -1; inEnv = 0; inDefaults = 0; defaultsRun = -1
        inRunsOnList = 0; seenContent = 0; resetJob(); stepName = "(workflow)"
    }
    # An `env:` key opens a block mapping of @p scope, or is refused when it carries anything else.
    function openEnv(scope) {
        if (value != "" && value != "{}") { refuse(FNR, "unreadable-env", scope " `env: " value "`"); return }
        inEnv = 1; envIndent = ind; envScope = scope
    }
    function addEnv(name) {
        if (envScope == "workflow") workflowEnv[name] = 1
        else if (envScope == "job") { jobEnv[name] = 1; jobEnvAt[jobStart, name] = 1 }
        else stepEnv[name] = 1
    }

    BEGIN {
        sq = sprintf("%c", 39)
        n = split(bashNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") bashSet[p[i]] = 1
        n = split(windowsNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") windowsSet[p[i]] = 1
        n = split(shells, p, " "); for (i = 1; i <= n; i++) if (split(p[i], q, "|") == 2) shellModel[q[1]] = q[2]
        n = split(remedies, p, "\036"); for (i = 1; i <= n; i++) if ((k = index(p[i], "|")) > 0) remedy[substr(p[i], 1, k - 1)] = substr(p[i], k + 1)
        pass = 0; workflowShell = ""; resetAll()
    }

    FNR == 1 { pass++; resetAll() }

    {
        line = $0
        sub(/\r$/, "", line)
        ind = indentOf(line)
        blank = (line ~ /^[ \t]*$/)

        # A block scalar, or a plain scalar continuing, owns every blank or more-indented line after its key.
        if (blockOwner >= 0) {
            if (blank || ind > blockOwner) {
                if (blockKind == "run") body[nbody++] = blank ? line : substr(line, blockOwner + 1)
                next
            }
            blockOwner = -1
        }
        if (blank || line ~ /^[ \t]*#/) next
        if (line ~ /^(---|\.\.\.)([ \t]|$)/) {
            if (seenContent) refuse(FNR, "unreadable-yaml", "a document marker after the first document")
            next
        }
        seenContent = 1

        isItem = (line ~ /^[ ]*-([ \t]|$)/)

        # Leaving a context, by indentation.
        if (inEnv && ind <= envIndent && !(isItem && ind == envIndent)) inEnv = 0
        if (inDefaults && ind <= defaultsIndent) { inDefaults = 0; defaultsRun = -1 }
        if (inRunsOnList && ind <= runsOnIndent && !(isItem && ind == runsOnIndent)) inRunsOnList = 0
        if (inSteps && ind <= stepsIndent && !(isItem && (itemIndent < 0 || ind == itemIndent))) { flush(); inSteps = 0 }
        if (inJobs && ind == 0) { flush(); inJobs = 0 }

        stepItem = 0
        if (isItem) {
            if (inRunsOnList) { t = line; sub(/^[ ]*-[ \t]*/, "", t); runsOn = runsOn " " unquote(trim(t)); runsOnAt[jobStart] = runsOn; next }
            if (inSteps && (itemIndent < 0 || ind == itemIndent)) {
                if (itemIndent < 0) itemIndent = ind
                flush(); resetStep(); stepItem = 1
            }
            sub(/-/, " ", line)
            ind = indentOf(line)
            if (line ~ /^[ \t]*$/) next
            # The keys of a step sit where its first key does, however many spaces follow the dash.
            if (stepItem) stepKeyIndent = ind
        }

        if (!match(line, /^[ ]*[A-Za-z_][A-Za-z0-9_.-]*[ \t]*:([ \t]|$)/)) {
            # A scalar list item (a `needs:` or `branches:` entry) is placed; a step that is not a mapping, and any
            # other line, is not.
            if (isItem && !stepItem) next
            refuse(FNR, "unreadable-yaml", (stepItem ? "a step written as `" : "`") trim(line) "`")
            next
        }
        key = substr(line, RSTART, RLENGTH)
        sub(/^[ ]*/, "", key); sub(/[ \t]*:[ \t]*$/, "", key)
        value = trim(substr(line, RSTART + RLENGTH))
        c = substr(value, 1, 1)
        if (c != "\"" && c != sq) sub(/[ \t]+#.*$/, "", value)

        # A block scalar indicator hands the lines after it to the block; any other inline value may continue as a
        # plain scalar on more-indented lines, which are its text and never keys.
        if (inStep && inSteps && stepKeyIndent < 0 && ind > itemIndent) stepKeyIndent = ind
        stepKey = (inStep && inSteps && ind == stepKeyIndent)
        if (value ~ /^[|>][-+0-9]*$/) {
            blockOwner = ind
            blockKind = (stepKey && key == "run") ? "run" : "skip"
            if (blockKind == "run") hasRun = 1
            if (!(inEnv && ind > envIndent)) next
        } else if (value != "") {
            blockOwner = ind; blockKind = "skip"
        }

        if (inEnv && ind > envIndent) { addEnv(key); next }

        if (ind == 0) {
            if (key == "env") openEnv("workflow")
            else if (key == "defaults") { inDefaults = 1; defaultsIndent = 0 }
            else if (key == "jobs") { if (value != "") refuse(FNR, "unreadable-yaml", "`jobs: " value "`"); else { inJobs = 1; jobIndent = -1 } }
            next
        }

        if (inDefaults) {
            if (key == "run" && value == "") { defaultsRun = ind; next }
            if (defaultsRun >= 0 && ind > defaultsRun && key == "shell") {
                if (defaultsIndent == 0) workflowShell = unquote(value); else { jobShell = unquote(value); jobShellAt[jobStart] = jobShell }
            }
            next
        }

        if (!inJobs) next
        if (jobIndent < 0) jobIndent = ind
        if (ind == jobIndent) { flush(); resetJob(); next }
        if (jobKeyIndent < 0 && ind > jobIndent) jobKeyIndent = ind
        if (ind == jobKeyIndent) {
            if (key == "env") openEnv("job")
            else if (key == "defaults") { inDefaults = 1; defaultsIndent = ind }
            else if (key == "runs-on") { if (value == "") { inRunsOnList = 1; runsOnIndent = ind; runsOn = ""; blockOwner = -1 } else { runsOn = unquote(value); runsOnAt[jobStart] = runsOn } }
            else if (key == "steps") { if (value != "") refuse(FNR, "unreadable-yaml", "`steps: " value "`"); else { inSteps = 1; stepsIndent = ind; itemIndent = -1 } }
            next
        }
        if (!stepKey) next
        if (key == "name") { t = unquote(value); if (t != "") stepName = t }
        else if (key == "shell") stepShell = trim(unquote(value))
        else if (key == "env") openEnv("step")
        else if (key == "run") {
            if (c == "*" || c == "&" || c == "!" || c == "[" || c == "{") refuse(FNR, "unreadable-run", "`run: " value "`")
            else {
                hasRun = 1
                body[nbody++] = unquote(value)
                # A plain scalar continues on more-indented lines, including one whose first line is empty.
                blockOwner = ind; blockKind = "run"
            }
        }
    }

    END { flush(); printf "STEPS\t%d\t%d\t%d\n", scanned, models["bash"], models["pwsh"] }
    ' "$1" "$1"
}

# Judge every workflow under @p dir; print one line per file and per refusal; return 0 clean, 1 refused.
# @param 1 The workflows directory.
# @param 2 `plant` to plant a read of an undefined name at the end of every run step, which passes only when every
#          step a model read refuses it: proof, on the real files, that each is read by a model that sees a read.
Judge() {
    local dir="$1" mode="${2:-plain}" plantFlag=0 file display status report files=0 steps=0 modelled=0 bashSteps=0 pwshSteps=0
    local tag line step kind detail remedy scanned planted=0 unplanted=0
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
                    # For a STEPS record the fields are the steps, those read as bash, and those read as PowerShell.
                    scanned="$line"
                    steps=$((steps + line)); bashSteps=$((bashSteps + step)); pwshSteps=$((pwshSteps + kind))
                    echo "workflow-step-env: ${display}: ${line} run step(s), ${step} read as bash, ${kind} as PowerShell"
                    ;;
                PLANTED)
                    # For a PLANTED record the fourth field is whether the planted read was seen.
                    planted=$((planted + 1))
                    if [ "$kind" != "1" ]; then
                        echo "  FAIL: ${display}:${line} step \"${step}\": a read planted at the end of its script was NOT seen, so its reading ends inside a quote, a heredoc or a here-string and a real read there would pass unseen. Either the script really leaves one open, or the check misreads a construct in it -- then the check is what needs fixing, with a case."
                        problems=$((problems + 1))
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
    if [ "$mode" = plant ] && [ "$planted" -ne "$modelled" ]; then
        echo "  FAIL: the plant went into ${planted} of the ${modelled} run step(s) a model read"
        problems=$((problems + 1))
    fi
    if [ "$problems" -ne 0 ]; then
        echo "workflow-step-env: ${mode}: ${problems} problem(s) across ${files} workflow file(s) and ${steps} run step(s)"
        return 1
    fi
    if [ "$mode" = plant ]; then
        echo "workflow-step-env: plant: all ${modelled} run step(s) a model read in ${files} workflow file(s) refused the planted read (${bashSteps} bash, ${pwshSteps} PowerShell); ${unplanted} unplanted problem(s) left to the unplanted run"
    else
        echo "workflow-step-env: ${files} workflow file(s), ${steps} run step(s) (${bashSteps} bash, ${pwshSteps} PowerShell), every environment name each reads is one it can see"
    fi
    return 0
}

# ---------------------------------------------------------------------------
SelfTest() {
    local tmp ran=0 failures=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/workflow-step-env-selftest.XXXXXX")" || { echo "cannot create a scratch directory"; exit 2; }
    trap 'rm -rf -- "$tmp"' EXIT

    # Judge the workflow on stdin, alone in a directory of its own.
    # @param 1 Case name. @param 2 Expected status (0 or 1). @param 3 Text the output must hold.
    # @param 4 Mode (plain or plant). @param 5 `nofile` to stage an empty directory and read nothing from stdin.
    Case() {
        local name="$1" want="$2" expect="$3" mode="${4:-plain}" out got
        mkdir -p "$tmp/$name"
        [ "${5:-}" = nofile ] || cat > "$tmp/$name/wf.yml"
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

    # ---- the plant ----------------------------------------------------------------------------------------------

    Case plantSeenEverywhere 0 'all 3 run step(s) a model read in 1 workflow file(s) refused the planted read (2 bash, 1 PowerShell)' plant <<'WF'
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
