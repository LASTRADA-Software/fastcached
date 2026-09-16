# SPDX-License-Identifier: Apache-2.0
#
# A GitHub Actions workflow, as an awk library: one walk, one step record, one job record.
#
# Loaded with `awk -f scripts/lib/workflow-walk.awk -f <the check>.awk`, ahead of the check's own
# program and after `scripts/lib/shell-lex.awk` where that is wanted too. awk has no include
# directive and multiple `-f` is POSIX (#1456).
#
# Extracted from `check-workflow-step-env.sh`, which of the five structure readers was the only
# DEPTH-RELATIVE one -- the other four spell `build.yml`'s two-space indentation as literal column
# counts, so each is correct for the file as written today and answers nothing about a file whose
# jobs sit at another depth, which is legal YAML. It is also the only one whose self-test drives
# folded scalars, heredocs and a second document.
#
# ## How a consumer is called back into
#
# awk has no function pointers, so a library reaches its consumer by calling a function the
# consumer defines. This one calls exactly ONE:
#
#     function WorkflowOn(kind) { ... }
#
# `kind` says what happened; the record is in the globals below. A consumer that does not care
# about a kind returns -- which is a decision spelled in its own code rather than an omission.
#
# **One hook rather than eight, and that is measured.** gawk makes a call to an undefined function
# a FATAL runtime error, not a silent no-op, so the convention enforces itself: a consumer that
# forgets the hook cannot report clean, because it cannot run at all. But the error names an
# `FNR`, and driving it directly showed the enforcement is **per reached path, not per program** --
# a hook called only on some lines exits 0 in silence until such a line appears, and is then a
# fatal error in CI on somebody's unrelated pull request, naming an awk function rather than the
# check that lacks it. Eight hooks would be seven of those waiting. The `raw` kind fires on every
# line of every input, so the one hook is reached before anything else can be.
#
# ## The kinds, in the order they can fire for one line
#
#   raw        every line as read, before any interpretation. `WfLine` and `WfAt` only.
#   text       a continuation line owned by a scalar -- a `run:` body line, or any other block or
#              plain scalar's text. `WfBlockKey` is the key it belongs to and `WfScope` says
#              where it sits. It fires OUTSIDE a step too, and deliberately: a folded job-level
#              `if:` is a continuation nothing else would report, and two of the readers being
#              migrated join one on purpose -- read a line at a time, a wrapped condition looks
#              exactly like a deleted reader. A consumer that wants step bodies only says
#              `WfScope == "step"`.
#   step-line  a line inside a step, before it is read as a key, so a step's first line is this
#              step's and not its predecessor's. `WfEnvRow` says it is a row of the step's own
#              `env:`.
#   key        a mapping line. `WfKey`, `WfValue`, `WfPath`, `WfScope`, `WfIndent`.
#   item       a bare scalar entry of a block sequence -- a `needs:`, `tags:` or `branches:`
#              value. `WfItem` is its text, unquoted; `WfPath` is the OWNING key's path,
#              since nothing is pushed for the entry itself. A `runs-on:` entry fires this
#              too, and additionally accumulates into `WfRunsOn`.
#   step       a step record is complete: every field below is final and the next line belongs to
#              another step, another job, or nothing.
#   job        a job record is complete. Fires after that job's last `step`.
#   refusal    a line the walk cannot place. `WfRefuseKind`, `WfRefuseDetail`, `WfAt`.
#   end        after the last line of the last file.
#
# ## The state this library owns
#
# A consumer READS these. It must not write them, with one exception named at the end.
#
#   WfPass         which pass over the input this is, counted from `FNR == 1`
#   WfLine WfAt    the current line (CR stripped) and its `FNR`
#   WfIndent       that line's indentation in spaces
#   WfKey WfValue  a mapping line's key and its inline value, comment stripped
#   WfPath         the `/`-joined key path from the document root, e.g. `on/pull_request`
#   WfScope        where the line sits: "" (document), "job", "steps" or "step"
#   WfStepKey      1 when the line is one of the current step's own keys
#   WfEnvRow       1 when the line is a row of the current step's own `env:`
#   WfBlockKey     at a `text` event, the KEY whose scalar this continuation belongs to --
#                  the only way to tell a folded `if:`'s second line from any other
#                  scalar's, and a fact only the walk has
#   WfJob          the current job's key, or "" outside `jobs:`
#   WfJobStart     the `FNR` of that job's key -- the identity a pass-1 finding is filed under
#   WfJobShell     `defaults.run.shell` for this job, or ""
#   WfRunsOn       this job's `runs-on`, list items joined by spaces
#   WfItem         at an `item` event, that entry's text
#   WfWorkflowShell  the workflow-level `defaults.run.shell`, or ""
#   WfStepIndex    this step's position in its job, from 1
#   WfStepLine     the `FNR` the step's item began on
#   WfStepName WfStepUses WfStepShell   its `name`, `uses` (tag stripped) and `shell`
#   WfHasRun       whether it carries a `run:`
#   WfBody[] WfBodyAt[] WfBodyCount     that `run:`'s lines and the `FNR` of each
#   WfStepEnvRows  how many rows the step's own `env:` block had
#   WfWorkflowEnv[] WfJobEnv[] WfStepEnv[]   the names each `env:` scope defines
#   WfCounted[] WfPlaced[] WfRunKeys    the completeness cross-check, below
#   WfYamlOk       whether `WorkflowYamlQuoted` closed the scalar it was given
#
# The exception: a consumer may append to `WfBody[]`/`WfBodyCount` from its `step` hook, which is
# how `check-workflow-step-env.sh` adds the planted line its neuter depends on. Nothing in this
# file reads them after `step` has fired.
#
# ## The completeness cross-check, and why it is here
#
# `WorkflowCount` finds every `run:` key WITHOUT the walk -- no steps, no jobs, no key columns,
# only which lines a scalar owns -- and the walk records every one it PLACED. A consumer compares
# the two in its `END`, so a file whose keys sit at a column the walk did not expect is refused by
# line rather than read as clean. It belongs here rather than in one check because it is a property
# of the WALK: the question it answers is whether this file's steps were seen at all.
#
# ## What is NOT here
#
# The shell lexer is `scripts/lib/shell-lex.awk`, a separate library, because a check may want the
# walk without any shell model. `looseHere`/`advance` -- bash and PowerShell CONSTRUCT nesting --
# stay with `check-workflow-step-env.sh`, which owns #1461's ordering rule. The line is whether a
# function answers *what does this YAML say* (here) or *what does this repository require of it*.

BEGIN {
    # `sq` is a single quote. No awk program in this tree can spell one directly: these programs
    # are read from files now, but the SELF-TESTS stage fragments of their vocabulary, and a
    # literal quote would be one more thing that has to agree across two files. Set HERE rather
    # than in each consumer's `BEGIN`, which also satisfies `shell-lex.awk`'s need for it whenever
    # both libraries are loaded -- library `BEGIN` blocks run in `-f` order, before the consumer's.
    sq = sprintf("%c", 39)
    WfPass = 0
    WfWorkflowShell = ""
    WfCountOwner = -1
    WorkflowResetAll()
}

# A new file is a new pass, and the pass that is ending owes its last step and its last job
# the same completion `END` gives the final one -- flushed BEFORE the counter moves, so each
# record is attributed to the pass it was read in. Both flushes are guarded on something
# being open, so on the very first file they are no-ops.
FNR == 1 {
    WorkflowFlushStep(); WorkflowFlushJob()
    WfPass++; WorkflowResetAll()
}

{ WorkflowWalkLine() }

END {
    WorkflowFlushStep(); WorkflowFlushJob()
    WfKind = "end"; WorkflowOn("end")
}

# ---- the walk ------------------------------------------------------------------------------------

function WorkflowEmit(kind) { WfKind = kind; WorkflowOn(kind) }

function WorkflowRefuse(at, kind, detail) {
    WfAt = at; WfRefuseKind = kind; WfRefuseDetail = detail
    # A line the walk could not read is pushed onto the path as `?`, so a `key` event under it
    # reads `jobs/one/steps/?/nothing` rather than inheriting the ancestry of whatever key last
    # happened to sit at that indent -- which was a DIFFERENT step. The refusal already says the
    # region is unreadable; the path must not go on claiming otherwise one level down.
    if (kind == "unreadable-yaml" || kind == "unreadable-run") WorkflowPushPath(WfIndent, "?")
    WorkflowEmit("refusal")
}

# Where the current line sits. One answer, asked at each event rather than computed once at
# the key site -- a scope that is one line stale is wrong exactly at a context boundary.
function WorkflowScope() {
    return (WfInStep && WfInSteps) ? "step" : (WfInSteps ? "steps" : (WfInJobs ? "job" : ""))
}
function WorkflowIndentOf(s,   n) { n = match(s, /[^ ]/); return n ? n - 1 : length(s) }
function WorkflowTrim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
function WorkflowUnquote(t,   first, last) {
    first = substr(t, 1, 1); last = substr(t, length(t), 1)
    if (length(t) >= 2 && first == last && (first == "\"" || first == sq)) return substr(t, 2, length(t) - 2)
    return t
}

# One line of the walk. Its scratch is in the parameter list, which is awk's only way to declare a
# local -- a global here would be a name five consumers could collide with.
function WorkflowWalkLine(   line, ind, blank, isItem, stepItem, t, key, value, c) {
    line = $0
    sub(/\r$/, "", line)
    WfLine = line; WfAt = FNR
    WfIndent = ind = WorkflowIndentOf(line)
    blank = (line ~ /^[ \t]*$/)
    WorkflowEmit("raw")
    if (WfPass == 2) WorkflowCount(line)

    # A block scalar, or a plain scalar continuing, owns every blank or more-indented line after
    # its key.
    if (WfBlockOwner >= 0) {
        if (blank || ind > WfBlockOwner) {
            if (WfBlockKind == "run") {
                WfBodyAt[WfBodyCount] = FNR
                WfBody[WfBodyCount++] = blank ? line : substr(line, WfBlockOwner + 1)
            }
            # A continuation line of one of this step's values. A `#` line inside a `run:` body is
            # SHELL text and its expressions are still substituted, so it is reported here --
            # unlike a YAML comment, which the site below never sees.
            WfLine = line
            WfEnvRow = (WfInEnv && WfEnvScope == "step" && ind > WfEnvIndent)
            WfScope = WorkflowScope()
            WorkflowEmit("text")
            return
        }
        WfBlockOwner = -1
    }
    if (blank || line ~ /^[ \t]*#/) return
    if (line ~ /^(---|\.\.\.)([ \t]|$)/) {
        if (WfSeenContent) WorkflowRefuse(FNR, "unreadable-yaml", "a document marker after the first document")
        return
    }
    WfSeenContent = 1

    isItem = (line ~ /^[ ]*-([ \t]|$)/)

    # Leaving a context, by indentation.
    if (WfInEnv && ind <= WfEnvIndent && !(isItem && ind == WfEnvIndent)) WfInEnv = 0
    if (WfInDefaults && ind <= WfDefaultsIndent) { WfInDefaults = 0; WfDefaultsRun = -1 }
    if (WfInRunsOnList && ind <= WfRunsOnIndent && !(isItem && ind == WfRunsOnIndent)) WfInRunsOnList = 0
    if (WfInSteps && ind <= WfStepsIndent && !(isItem && (WfItemIndent < 0 || ind == WfItemIndent))) {
        WorkflowFlushStep(); WfInSteps = 0
    }
    if (WfInJobs && ind == 0) { WorkflowFlushStep(); WorkflowFlushJob(); WfInJobs = 0 }

    stepItem = 0
    if (isItem) {
        if (WfInRunsOnList) {
            t = line; sub(/^[ ]*-[ \t]*/, "", t)
            WfItem = WorkflowUnquote(WorkflowTrim(t))
            WfRunsOn = WfRunsOn " " WfItem; WfRunsOnAt[WfJobStart] = WfRunsOn
            WorkflowEmit("item")
            return
        }
        if (WfInSteps && (WfItemIndent < 0 || ind == WfItemIndent)) {
            if (WfItemIndent < 0) WfItemIndent = ind
            WorkflowFlushStep(); WorkflowResetStep(); stepItem = 1
        }
        sub(/-/, " ", line)
        ind = WorkflowIndentOf(line)
        WfLine = line; WfIndent = ind
        if (line ~ /^[ \t]*$/) return
        # The keys of a step sit where its first key does, however many spaces follow the dash.
        if (stepItem) WfStepKeyIndent = ind
    }

    # Every other line of a step: its keys and their inline values, and the rows of its `with:`
    # and `env:` blocks. AFTER the boundary handling above, so a step's first line is read as that
    # step's and not as its predecessor's, and after the comment skip, so a YAML comment
    # explaining an expression is not read as carrying one.
    WfScope = WorkflowScope()
    if (WfInStep && WfInSteps) {
        WfEnvRow = (WfInEnv && WfEnvScope == "step" && ind > WfEnvIndent)
        if (WfEnvRow) WfStepEnvRows++
        WorkflowEmit("step-line")
    }

    if (!match(line, /^[ ]*[A-Za-z_][A-Za-z0-9_.-]*[ \t]*:([ \t]|$)/)) {
        # A scalar list item -- a `needs:`, `tags:` or `branches:` entry written as a block
        # sequence. It is PLACED and reported: for years it was placed and reported nothing,
        # so a consumer wanting `release.needs` had to walk the file itself.
        if (isItem && !stepItem) {
            WfItem = WorkflowUnquote(WorkflowTrim(line))
            WorkflowEmit("item")
            return
        }
        # A step that is not a mapping, and any other line, is not placed.
        WorkflowRefuse(FNR, "unreadable-yaml", (stepItem ? "a step written as `" : "`") WorkflowTrim(line) "`")
        return
    }
    key = substr(line, RSTART, RLENGTH)
    sub(/^[ ]*/, "", key); sub(/[ \t]*:[ \t]*$/, "", key)
    value = WorkflowTrim(substr(line, RSTART + RLENGTH))
    c = substr(value, 1, 1)
    # A comment may be all there is after the colon, and the block below the key is then its value.
    if (c != "\"" && c != sq) sub(/(^|[ \t]+)#.*$/, "", value)
    WfKey = key; WfValue = value
    WorkflowPushPath(ind, key)

    # A job's own key CLOSES the job before it, and that is settled here rather than in the
    # dispatch below because the `key` event fires in between: a consumer reading `WfJob` on
    # the line `  second:` would otherwise be told `first`, which is the path and the record
    # disagreeing about one line. Reachable only with `jobs:` open and no `env:` or
    # `defaults:` in force, both of which any column-0 or shallower key has already closed.
    if (WfInJobs && ind > 0) {
        if (WfJobIndent < 0) WfJobIndent = ind
        if (ind == WfJobIndent) {
            WorkflowFlushStep(); WorkflowFlushJob(); WorkflowResetJob(); WfJob = key
        }
    }

    # A block scalar indicator hands the lines after it to the block; any other inline value may
    # continue as a plain scalar on more-indented lines, which are its text and never keys.
    if (WfInStep && WfInSteps && WfStepKeyIndent < 0 && ind > WfItemIndent) WfStepKeyIndent = ind
    WfStepKey = (WfInStep && WfInSteps && ind == WfStepKeyIndent)
    WorkflowEmit("key")
    if (value ~ /^[|>][-+0-9]*$/) {
        WfBlockOwner = ind; WfBlockKey = key
        WfBlockKind = (WfStepKey && key == "run") ? "run" : "skip"
        if (WfBlockKind == "run") { WfHasRun = 1; WorkflowPlace() }
        if (!(WfInEnv && ind > WfEnvIndent)) return
    } else if (value != "") {
        WfBlockOwner = ind; WfBlockKind = "skip"; WfBlockKey = key
    }

    if (WfInEnv && ind > WfEnvIndent) { WorkflowAddEnv(key); return }

    if (ind == 0) {
        if (key == "env") WorkflowOpenEnv("workflow", value, ind)
        else if (key == "defaults") { WfInDefaults = 1; WfDefaultsIndent = 0 }
        else if (key == "jobs") {
            if (value != "") WorkflowRefuse(FNR, "unreadable-yaml", "`jobs: " value "`")
            else { WfInJobs = 1; WfJobIndent = -1 }
        }
        return
    }

    if (WfInDefaults) {
        if (key == "run" && value == "") { WfDefaultsRun = ind; WorkflowPlace(); return }
        if (WfDefaultsRun >= 0 && ind > WfDefaultsRun && key == "shell") {
            if (WfDefaultsIndent == 0) WfWorkflowShell = WorkflowUnquote(value)
            else { WfJobShell = WorkflowUnquote(value); WfJobShellAt[WfJobStart] = WfJobShell }
        }
        return
    }

    if (!WfInJobs) return
    if (ind == WfJobIndent) return          # a job key, already opened above the `key` event
    if (WfJobKeyIndent < 0 && ind > WfJobIndent) WfJobKeyIndent = ind
    if (ind == WfJobKeyIndent) {
        if (key == "env") WorkflowOpenEnv("job", value, ind)
        else if (key == "defaults") { WfInDefaults = 1; WfDefaultsIndent = ind }
        else if (key == "runs-on") {
            if (value == "") { WfInRunsOnList = 1; WfRunsOnIndent = ind; WfRunsOn = ""; WfBlockOwner = -1 }
            else { WfRunsOn = WorkflowUnquote(value); WfRunsOnAt[WfJobStart] = WfRunsOn }
        }
        else if (key == "steps") {
            if (value != "") WorkflowRefuse(FNR, "unreadable-yaml", "`steps: " value "`")
            else { WfInSteps = 1; WfStepsIndent = ind; WfItemIndent = -1 }
        }
        return
    }
    if (!WfStepKey) return
    if (key == "name") { t = WorkflowUnquote(value); if (t != "") WfStepName = t }
    else if (key == "uses") { WfStepUses = WorkflowTrim(WorkflowUnquote(value)); sub(/@.*$/, "", WfStepUses) }
    else if (key == "shell") WfStepShell = WorkflowTrim(WorkflowUnquote(value))
    else if (key == "env") WorkflowOpenEnv("step", value, ind)
    else if (key == "run") {
        WorkflowPlace()
        if (c == "*" || c == "&" || c == "!" || c == "[" || c == "{")
            WorkflowRefuse(FNR, "unreadable-run", "`run: " value "`")
        else {
            if (c == sq || c == "\"") {
                t = WorkflowYamlQuoted(value)
                if (!WfYamlOk) {
                    WorkflowRefuse(FNR, "unreadable-run", "`run: " value "` is a quoted scalar this check cannot read on one line")
                    return
                }
                value = t
            }
            WfHasRun = 1
            WfBodyAt[WfBodyCount] = FNR; WfBody[WfBodyCount++] = value
            # A plain scalar continues on more-indented lines, including one whose first line is empty.
            WfBlockOwner = ind; WfBlockKind = "run"; WfBlockKey = key
        }
    }
}

# ---- the key path ---------------------------------------------------------------------------------
# The ancestors of the current key, so a consumer states a PATH rather than a column count. A stack
# of the open indents: everything at or deeper than this line's indent has closed.
function WorkflowPushPath(ind, key,   i) {
    while (WfPathDepth > 0 && WfPathIndent[WfPathDepth] >= ind) WfPathDepth--
    WfPathDepth++
    WfPathIndent[WfPathDepth] = ind
    WfPathKey[WfPathDepth] = key
    WfPath = ""
    for (i = 1; i <= WfPathDepth; i++) WfPath = WfPath (i > 1 ? "/" : "") WfPathKey[i]
}

# ---- the records ----------------------------------------------------------------------------------

function WorkflowFlushStep() {
    if (!WfInStep) return
    WfInStep = 0
    WorkflowEmit("step")
}

function WorkflowFlushJob() {
    if (WfJob == "") return
    WorkflowEmit("job")
    WfJob = ""
}

function WorkflowResetStep() {
    split("", WfStepEnv); split("", WfBody); split("", WfBodyAt)
    WfStepUses = ""; WfStepEnvRows = 0; WfBodyCount = 0; WfHasRun = 0; WfInStep = 1
    WfStepName = "(unnamed)"; WfStepLine = FNR; WfStepShell = ""; WfStepKeyIndent = -1
    WfStepIndex++
}

# The `env:`, `defaults` and `runs-on` may follow its `steps:`, so the second pass starts each job
# from what the first pass found anywhere in that job.
function WorkflowResetJob(   k, kp) {
    split("", WfJobEnv)
    WfJobShell = ""; WfRunsOn = ""; WfInSteps = 0; WfItemIndent = -1; WfJobKeyIndent = -1
    WfStepName = "(job)"; WfJobStart = FNR; WfStepIndex = 0; WfJob = ""
    if (WfPass < 2) return
    for (k in WfJobEnvAt) { split(k, kp, SUBSEP); if (kp[1] == WfJobStart) WfJobEnv[kp[2]] = 1 }
    if (WfJobStart in WfJobShellAt) WfJobShell = WfJobShellAt[WfJobStart]
    if (WfJobStart in WfRunsOnAt) WfRunsOn = WfRunsOnAt[WfJobStart]
}

# Every state the walk carries, reset at the start of each pass. The workflow scope is not: `env:`
# and `defaults` may follow `jobs:`, and the first pass is what the second one reads them from.
function WorkflowResetAll() {
    WfInStep = 0; WfInJobs = 0; WfJobIndent = -1; WfBlockOwner = -1; WfInEnv = 0; WfInDefaults = 0
    WfDefaultsRun = -1; WfInRunsOnList = 0; WfSeenContent = 0; WfPathDepth = 0; WfPath = ""
    WorkflowResetJob(); WfStepName = "(workflow)"
}

# An `env:` key opens a block mapping of @p scope, or is refused when it carries anything else.
function WorkflowOpenEnv(scope, value, ind) {
    if (value != "" && value != "{}") {
        WorkflowRefuse(FNR, "unreadable-env", scope " `env: " value "`")
        return
    }
    WfInEnv = 1; WfEnvIndent = ind; WfEnvScope = scope
}

function WorkflowAddEnv(name) {
    if (WfEnvScope == "workflow") WfWorkflowEnv[name] = 1
    else if (WfEnvScope == "job") { WfJobEnv[name] = 1; WfJobEnvAt[WfJobStart, name] = 1 }
    else WfStepEnv[name] = 1
}

# ---- the completeness cross-check -----------------------------------------------------------------
# Every `run:` key outside a scalar, found WITHOUT the walk: no steps, no jobs, no key columns, only
# which lines a scalar owns. A line the walk PLACES as a script and a line counted here are compared
# by the consumer, so a step the walk stops recognising -- keys at a column it did not expect -- is
# refused by line rather than read as clean. A script body is a scalar, so a heredoc writing `run:`
# into a file is text here too, as in the walk.
function WorkflowCount(s,   lead, key, v) {
    if (WfCountOwner >= 0 && (s ~ /^[ \t]*$/ || WorkflowIndentOf(s) > WfCountOwner)) return
    WfCountOwner = -1
    if (match(s, /^[ ]*(-[ ]+)*/)) { lead = substr(s, 1, RLENGTH); gsub(/-/, " ", lead); s = lead substr(s, RLENGTH + 1) }
    if (!match(s, /^[ ]*[A-Za-z_][A-Za-z0-9_.-]*[ \t]*:([ \t]|$)/)) return
    key = substr(s, RSTART, RLENGTH)
    sub(/^[ ]*/, "", key); sub(/[ \t]*:[ \t]*$/, "", key)
    v = WorkflowTrim(substr(s, RSTART + RLENGTH))
    if (substr(v, 1, 1) != "\"" && substr(v, 1, 1) != sq) sub(/(^|[ \t]+)#.*$/, "", v)
    if (key == "run") { WfCounted[FNR] = WorkflowTrim(s); WfRunKeys++ }
    if (v != "") WfCountOwner = WorkflowIndentOf(s)
}

function WorkflowPlace() { if (WfPass == 2) WfPlaced[FNR] = 1 }

# ---- YAML-quoted scalars --------------------------------------------------------------------------
# The text of a YAML-quoted scalar @p v, which must close on its own line with at most a comment
# after it, and sets `WfYamlOk` to say whether it did. YAML quoting is not shell quoting, so the
# quotes must not reach a shell model, where they would hide every read between them: a doubled
# apostrophe in single quotes is one apostrophe, and in double quotes an escaped quote, backslash
# or slash is that character. Any other escape, a quote left open for the next line, or text after
# the closing quote, is not read at all -- `WfYamlOk` is 0 and the caller refuses the step.
function WorkflowYamlQuoted(v,   q, i, n, ch, e, out) {
    q = substr(v, 1, 1); n = length(v); out = ""; WfYamlOk = 0
    for (i = 2; i <= n; i++) {
        ch = substr(v, i, 1)
        if (ch == q) {
            if (q == sq && substr(v, i + 1, 1) == sq) { out = out sq; i++; continue }
            WfYamlOk = (substr(v, i + 1) ~ /^([ \t]+#.*)?$/)
            return WfYamlOk ? out : ""
        }
        if (q == "\"" && ch == "\\") {
            e = substr(v, ++i, 1)
            if (e != "\"" && e != "\\" && e != "/") return ""
            out = out e
            continue
        }
        out = out ch
    }
    return ""
}
