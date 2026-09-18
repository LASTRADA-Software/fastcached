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
#   job        a job record is complete. Fires after that job's last `step`, with its matrix
#              already turned into combinations.
#   refusal    a line the walk cannot place, or a job matrix it cannot read (`unreadable-matrix`).
#              `WfRefuseKind`, `WfRefuseDetail`, `WfAt`.
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
#   WfHasMatrix    whether this job declares a `strategy.matrix`
#   WfMatrixUnread why that matrix cannot be read, or "" -- and then `WfComboCount` is 0
#   WfComboCount   how many combinations the job runs as: 1 for a job with no matrix, whose one
#                  combination has no keys, so a consumer iterates every job the same way
#   WfCombo[c, key]  combination @p c's value for @p key; absent when that combination has none
#   WfComboKeyCount[c] WfComboKey[c, k]   its keys, in the order GitHub adds them
#
# The exception: a consumer may append to `WfBody[]`/`WfBodyCount` from its `step` hook, which is
# how `check-workflow-step-env.sh` adds the planted line its neuter depends on. Nothing in this
# file reads them after `step` has fired.
#
# Two functions answer questions about a combination rather than about a line:
#
#   WorkflowMatrixExpand(s, c)  @p s with every `${{ matrix.KEY }}` substituted for combination @p c
#   WorkflowComboLabel(c)       `(key=value, ...)`, for naming a combination in a message
#
# ## The completeness cross-check, and why it is here
#
# `WorkflowCount` finds every `run:` key WITHOUT the walk -- no steps, no jobs, no key columns,
# only which lines a scalar owns -- and the walk records every one it PLACED. A consumer compares
# the two in its `END`, so a file whose keys sit at a column the walk did not expect is refused by
# line rather than read as clean. It belongs here rather than in one check because it is a property
# of the WALK: the question it answers is whether this file's steps were seen at all.
#
# ## The job matrix, and why it is the walk's (#1432)
#
# A job's NAME, RUNNER and cache KEYS may each vary by matrix combination, and three checks read
# them. Each had its own partial answer: one expanded `${{ matrix.preset }}` over a `preset:` flow
# sequence and nothing else, one expanded the axes a cache key named when they were inline lists
# and mapped `runs-on` to an OS only when it was literal, and one refused any expression in
# `runs-on` outright. So the first matrix to grow a second axis -- a `runner:` row, so one leg could
# run on arm64 -- read to the first as a context named `Linux-clang-release${{ matrix.suffix }}`,
# to the second as a runner it could not map, and to the third as a step with no shell. Three
# private models of one YAML construct is the defect #1456 removed for the walk itself, so the
# matrix is read HERE, once, with GitHub's semantics, and every consumer asks for combinations:
#
#   * the base AXES are every key under `strategy.matrix` but `include` and `exclude`, each a list
#     -- a one-line flow sequence or a block sequence of plain scalars -- and the base combinations
#     are their cartesian product, the first axis varying slowest;
#   * each `exclude` row removes every base combination it matches on all of its keys, and it runs
#     BEFORE `include`, so an include can add back what an exclude took away;
#   * each `include` row is added to every base combination whose ORIGINAL axis values it would
#     overwrite none of. Keys that are not original axes are added, and one an earlier row added
#     may be overwritten. A row that fits no base combination becomes a NEW combination of its own
#     keys -- and only base combinations are candidates, so two such rows are two combinations. A
#     matrix with no axes has no base combination, so every row is its own;
#   * `${{ matrix.KEY }}` expands per combination, and a key the combination lacks expands to the
#     EMPTY string, which is what GitHub substitutes.
#
# **A matrix this walk cannot read is REFUSED, never guessed**: an expression in place of the
# matrix (`${{ fromJSON(...) }}`), a flow mapping, a row that is not a block mapping of plain
# scalars, a value carrying an expression, a list that does not close on its line, an axis with no
# values, an exclude naming no axis, a matrix producing no combination -- and a plain value YAML
# TYPES as anything but a string, an integer or `true`/`false` (`3.10` is the number 3.1, which is
# what GitHub renders), or a comparison of `10` with `"10"`, since which types match is not
# modelled either; so are two keys that differ only in CASE. The text is kept only where it IS the
# value. It fires `refusal` with
# `unreadable-matrix` and leaves the job with NO combinations, so a consumer iterating them reads
# nothing rather than one plausible leg. A reference differing from a key of the matrix only in
# CASE is left unexpanded, for a consumer to refuse: whether GitHub folds it is not something this
# walk will guess in the direction that reads as clean.
#
# The combinations are final at the `job` event. A step fires before its job ends -- and a matrix
# may be written after the steps -- so the second pass starts each job from what the first pass
# computed for it, exactly as `runs-on` and the job's `env:` already are.
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
function WorkflowWalkLine(   line, ind, blank, isItem, itemInd, stepItem, t, key, value, c) {
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
            #
            # Inside a matrix it is a value continuing onto a second line -- a flow list wrapped
            # over two, or a plain scalar that goes on -- which the matrix model does not read.
            if (WfMxOpen && !blank && ind > WfMxIndent) WorkflowMatrixUnread("`" WorkflowTrim(line) "` continues a matrix value onto another line", FNR)
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
    if (WfMxOpen && ind <= WfMxIndent) {
        # A sequence at the matrix's own column is the matrix written as a list, which GitHub
        # does not accept either -- and read as ending the block, its rows would vanish unread.
        if (isItem && ind == WfMxIndent && WfMxKeyIndent < 0)
            WorkflowMatrixUnread("the matrix is written as a sequence rather than a mapping of axes", FNR)
        WfMxOpen = 0
    }
    if (WfInSteps && ind <= WfStepsIndent && !(isItem && (WfItemIndent < 0 || ind == WfItemIndent))) {
        WorkflowFlushStep(); WfInSteps = 0
    }
    if (WfInJobs && ind == 0) { WorkflowFlushStep(); WorkflowFlushJob(); WfInJobs = 0 }

    stepItem = 0
    if (isItem) {
        # The dash's own column, before the dash is blanked below: a matrix row's key sits at the
        # column after it, and only the dash says the row is NEW.
        itemInd = ind
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
        # A bare dash: its row's keys follow on the lines below, which the matrix reader would
        # otherwise take as keys of the row BEFORE it.
        if (line ~ /^[ \t]*$/) { WorkflowMatrixItem(itemInd, ""); return }
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
            # The RAW text rather than `WfItem`: a matrix value is read by YAML's quoting rules,
            # which `WorkflowUnquote` does not implement.
            WorkflowMatrixItem(itemInd, WorkflowTrim(line))
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
    WorkflowMatrixKey(ind, key, value, isItem, itemInd)
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
    WorkflowMatrixFinalize()
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
    WorkflowMatrixReset(); WorkflowComboReset()
    if (WfPass < 2) return
    for (k in WfJobEnvAt) { split(k, kp, SUBSEP); if (kp[1] == WfJobStart) WfJobEnv[kp[2]] = 1 }
    if (WfJobStart in WfJobShellAt) WfJobShell = WfJobShellAt[WfJobStart]
    if (WfJobStart in WfRunsOnAt) WfRunsOn = WfRunsOnAt[WfJobStart]
    WorkflowMatrixRestore()
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

# ---- the job matrix -------------------------------------------------------------------------------
# The header's `## The job matrix` states the semantics; this is the reader and the arithmetic.
#
# The PARSE state, reset per job and prefixed `WfMx` because no consumer reads it: `WfMxSeen` (a
# `matrix:` key was met), `WfMxOpen` (its block is being read), `WfMxIndent` (that key's column),
# `WfMxKeyIndent` (its axes' column), `WfMxAt` (its line), `WfMxSection` and `WfMxSectionName` (what
# the last axis-column key opened), `WfMxAxis` (the axis a block list feeds), `WfMxEntry` and
# `WfMxEntryIndent` (the include or exclude row being read), `WfMxUnread` (the first reason it
# cannot be read). The axes are `WfMxAxisN`, `WfMxAxisName[a]`, `WfMxAxisIdx[name]`,
# `WfMxAxisVals[a]`, `WfMxAxisVal[a, i]` and `WfMxAxisKind[a, i]`; the rows are `WfMxRowN[section]`,
# `WfMxRowKeyN[section, e]`, `WfMxRowKey[section, e, k]`, `WfMxRowVal[section, e, key]` and
# `WfMxRowKind[section, e, key]`, with `WfMxRowSeen[section]` refusing a second `include:`. A base
# combination's original kinds are `WfMxBaseKind[c, key]`; `WfMxFolded` is `WorkflowMatrixKeyCase`'s.

function WorkflowMatrixReset() {
    WfMxSeen = 0; WfMxOpen = 0; WfMxIndent = -1; WfMxKeyIndent = -1; WfMxAt = 0
    WfMxSection = ""; WfMxSectionName = ""; WfMxAxis = 0; WfMxEntry = 0; WfMxEntryIndent = -1
    WfMxUnread = ""; WfMxAxisN = 0
    split("", WfMxAxisName); split("", WfMxAxisIdx); split("", WfMxAxisVals); split("", WfMxAxisVal)
    split("", WfMxAxisKind); split("", WfMxRowKind); split("", WfMxBaseKind)
    split("", WfMxRowN); split("", WfMxRowKeyN); split("", WfMxRowKey); split("", WfMxRowVal)
    split("", WfMxRowSeen); split("", WfMxFolded)
    WfMxRowN["include"] = 0; WfMxRowN["exclude"] = 0
}

# The RESULT: one empty combination, which is what a job with no matrix runs as.
function WorkflowComboReset() {
    split("", WfCombo); split("", WfComboKey); split("", WfComboKeyCount)
    WfHasMatrix = 0; WfMatrixUnread = ""; WfComboCount = 1; WfComboKeyCount[1] = 0
}

# The first reason this job's matrix cannot be read, reported once as a refusal at line @p at. The
# walk's own `WfAt` and step name are put back afterwards, because a reason found while FINISHING a
# job is reported from inside the next job's key line.
function WorkflowMatrixUnread(reason, at,   savedAt, savedName) {
    if (WfMxUnread != "") return
    WfMxUnread = reason
    savedAt = WfAt; savedName = WfStepName; WfStepName = "(job)"
    WorkflowRefuse(at, "unreadable-matrix", "job `" WfJob "`: " reason)
    WfAt = savedAt; WfStepName = savedName
}

# A key line, after its `key` event. Opens the matrix at `jobs/<job>/strategy/matrix`, and inside it
# reads an axis, an include or exclude row's key, or refuses what fits neither.
# @param ind the key's column  @param isItem whether the line opened with a dash
# @param itemInd that dash's column
function WorkflowMatrixKey(ind, key, value, isItem, itemInd,   lead) {
    if (WfJob == "") return
    if (!WfMxOpen) {
        if (WfPath == "jobs/" WfJob "/strategy" && value != "") {
            WfMxSeen = 1; WfMxAt = FNR
            WorkflowMatrixUnread("`strategy: " value "` is not a mapping, so no matrix can be read from it", FNR)
            return
        }
        if (WfPath != "jobs/" WfJob "/strategy/matrix") return
        if (WfMxSeen) { WorkflowMatrixUnread("`matrix:` is written twice", FNR); return }
        WfMxSeen = 1; WfMxAt = FNR
        if (value != "") {
            WorkflowMatrixUnread("`matrix: " value "` is not a block mapping of axes", FNR)
            return
        }
        WfMxOpen = 1; WfMxIndent = ind
        return
    }
    if (WfMxUnread != "") return
    lead = isItem ? itemInd : ind
    if (WfMxKeyIndent < 0) {
        if (isItem) { WorkflowMatrixUnread("the matrix is written as a sequence rather than a mapping of axes", FNR); return }
        WfMxKeyIndent = ind
    }
    if (!isItem && ind == WfMxKeyIndent) { WorkflowMatrixSection(key, value); return }
    if (lead < WfMxKeyIndent) {
        WorkflowMatrixUnread("`" key ":` sits shallower than the matrix's own keys", FNR)
        return
    }
    if (WfMxSection != "include" && WfMxSection != "exclude") {
        WorkflowMatrixUnread("`" key ": " value "` under `" WfMxSectionName "` is a mapping where a plain scalar belongs", FNR)
        return
    }
    if (isItem) {
        WfMxEntry = ++WfMxRowN[WfMxSection]
        WfMxRowKeyN[WfMxSection, WfMxEntry] = 0
        WfMxEntryIndent = ind
    } else if (WfMxEntry == 0 || ind != WfMxEntryIndent) {
        WorkflowMatrixUnread("`" key ": " value "` under `" WfMxSectionName "` is nested inside a row rather than one of its keys", FNR)
        return
    }
    WorkflowMatrixRowValue(key, value)
}

# A key at the axes' column: `include`, `exclude`, or an axis.
function WorkflowMatrixSection(key, value) {
    WfMxSectionName = key; WfMxEntry = 0; WfMxEntryIndent = -1; WfMxAxis = 0
    if (key == "include" || key == "exclude") {
        if (key in WfMxRowSeen) { WorkflowMatrixUnread("`" key ":` is written twice", FNR); return }
        WfMxRowSeen[key] = 1
        WfMxSection = key
        if (value == "") return
        WfMxSection = "closed"
        if (value == "[]") return
        WorkflowMatrixUnread("`" key ": " value "` is not a block sequence of rows", FNR)
        return
    }
    if (key in WfMxAxisIdx) { WorkflowMatrixUnread("the axis `" key "` is written twice", FNR); return }
    WfMxAxis = ++WfMxAxisN
    WfMxAxisName[WfMxAxis] = key; WfMxAxisIdx[key] = WfMxAxis; WfMxAxisVals[WfMxAxis] = 0
    if (value == "") { WfMxSection = "axis"; return }
    WfMxSection = "closed"
    if (substr(value, 1, 1) != "[") {
        WorkflowMatrixUnread("the axis `" key ": " value "` is not a list", FNR)
        return
    }
    WorkflowMatrixFlow(value)
}

# A bare entry of a block sequence inside the matrix, which is an axis value or nothing readable.
# @param itemInd the dash's column  @param text the entry, raw
function WorkflowMatrixItem(itemInd, text,   v) {
    if (!WfMxOpen || WfMxUnread != "") return
    if (WfMxKeyIndent < 0) {
        WorkflowMatrixUnread("the matrix is written as a sequence rather than a mapping of axes", FNR)
        return
    }
    if (WfMxSection != "axis" || itemInd < WfMxKeyIndent) {
        WorkflowMatrixUnread((text == "" ? "a bare `-`, with its keys on the lines below," : "`- " text "`") \
            " under `" WfMxSectionName "` is not " \
            ((WfMxSection == "include" || WfMxSection == "exclude") ? "a row written as a block mapping" : "a value of a block-sequence axis"), FNR)
        return
    }
    v = WorkflowMatrixScalar(text)
    if (!WfMxScalarOk) {
        WorkflowMatrixUnread("`- " text "` in the axis `" WfMxSectionName "` " WfMxScalarWhy, FNR)
        return
    }
    WfMxAxisVal[WfMxAxis, ++WfMxAxisVals[WfMxAxis]] = v
    WfMxAxisKind[WfMxAxis, WfMxAxisVals[WfMxAxis]] = WfMxScalarKind
}

# One key of the include or exclude row being read.
function WorkflowMatrixRowValue(key, value,   s, e, v) {
    s = WfMxSection; e = WfMxEntry
    if ((s, e, key) in WfMxRowVal) {
        WorkflowMatrixUnread("`" key "` is written twice in one `" s "` row", FNR)
        return
    }
    v = WorkflowMatrixScalar(value)
    if (!WfMxScalarOk) {
        WorkflowMatrixUnread("`" key ":" (value == "" ? "" : " " value) "` in an `" s "` row " WfMxScalarWhy, FNR)
        return
    }
    WfMxRowKey[s, e, ++WfMxRowKeyN[s, e]] = key
    WfMxRowVal[s, e, key] = v
    WfMxRowKind[s, e, key] = WfMxScalarKind
}

# The items of the one-line flow sequence @p v, into the axis being read. Quotes are honoured, so a
# comma inside one separates nothing; a nested collection, an item that is not a plain scalar, and a
# list that does not close on its own line are refused.
function WorkflowMatrixFlow(v,   body, n, i, ch, q, item) {
    if (substr(v, length(v), 1) != "]") {
        WorkflowMatrixUnread("the axis `" WfMxSectionName ": " v "` does not close its list on its own line", FNR)
        return
    }
    body = substr(v, 2, length(v) - 2)
    if (WorkflowTrim(body) == "") return
    n = length(body); q = ""; item = ""
    for (i = 1; i <= n; i++) {
        ch = substr(body, i, 1)
        if (q != "") {
            item = item ch
            if (q == "\"" && ch == "\\") { i++; item = item substr(body, i, 1); continue }
            if (ch != q) continue
            if (q == sq && substr(body, i + 1, 1) == sq) { i++; item = item sq; continue }
            q = ""
            continue
        }
        if (ch == "\"" || ch == sq) { q = ch; item = item ch; continue }
        if (ch == "[" || ch == "]" || ch == "{" || ch == "}") {
            WorkflowMatrixUnread("the axis `" WfMxSectionName ": " v "` nests a collection", FNR)
            return
        }
        if (ch != ",") { item = item ch; continue }
        if (!WorkflowMatrixFlowItem(item, v)) return
        item = ""
    }
    if (q != "") {
        WorkflowMatrixUnread("the axis `" WfMxSectionName ": " v "` leaves a quote open", FNR)
        return
    }
    WorkflowMatrixFlowItem(item, v)
}

function WorkflowMatrixFlowItem(item, v,   s) {
    s = WorkflowMatrixScalar(WorkflowTrim(item))
    if (!WfMxScalarOk) {
        WorkflowMatrixUnread("the axis `" WfMxSectionName ": " v "` holds `" WorkflowTrim(item) "`, which " WfMxScalarWhy, FNR)
        return 0
    }
    WfMxAxisVal[WfMxAxis, ++WfMxAxisVals[WfMxAxis]] = s
    WfMxAxisKind[WfMxAxis, WfMxAxisVals[WfMxAxis]] = WfMxScalarKind
    return 1
}

# The text of the matrix value @p v, setting `WfMxScalarOk` to say whether it is one: a plain or
# YAML-quoted scalar carrying no expression -- `WfMxScalarWhy` to say why not, and `WfMxScalarKind`
# to say which YAML type it is. Empty -- YAML's null, or a key whose collection follows on the next
# lines -- is not one, and neither is anything opening with an indicator: a collection, an alias, a
# tag, a block scalar. An expression is refused because GitHub evaluates it, and what it evaluates
# to is not in this file.
#
# **A plain scalar is TYPED, and the text is not the value.** `3.10` is the number 3.1, so GitHub
# renders `${{ matrix.v }}` as `3.1` -- and whether `10` matches `"10"` in an exclude is a question
# about types. A string, a canonical integer and `true`/`false` render as written and are kept with
# their kind (`s`, `i`, `b`); every other spelling YAML may type -- a float, an exponent, a radix,
# null, and YAML 1.1's other booleans -- is refused, since a guess there is a wrong NAME or a
# missing LEG, and both read as clean. Quoting one makes it the string it looks like.
function WorkflowMatrixScalar(v,   c) {
    WfMxScalarOk = 0; WfMxScalarKind = "s"
    c = substr(v, 1, 1)
    if (c == "\"" || c == sq) {
        v = WorkflowYamlQuoted(v)
        WfMxScalarWhy = "is a quoted scalar that does not close on its line, or holds an escape this walk does not read"
        if (!WfYamlOk) return ""
    } else {
        sub(/(^|[ \t]+)#.*$/, "", v)
        v = WorkflowTrim(v)
        WfMxScalarWhy = "has no inline value, so it is null or a collection on the lines below"
        if (v == "") return ""
        WfMxScalarWhy = "opens with the YAML indicator `" c "`, so it is a collection, an alias, a tag or a block scalar"
        if (index("[]{}&*!|>%@`?", c) > 0) return ""
        WfMxScalarWhy = "is a plain scalar YAML types as a number, a boolean or null, which GitHub renders and compares its own way -- `3.10` is the number 3.1 -- so quote it to make it the string it looks like"
        if (v !~ /^(true|false|-?(0|[1-9][0-9]*))$/ && WorkflowYamlTyped(v)) return ""
        WfMxScalarKind = (v ~ /^(true|false)$/) ? "b" : ((v ~ /^-?(0|[1-9][0-9]*)$/) ? "i" : "s")
    }
    WfMxScalarWhy = "carries an expression, whose value GitHub computes and this file does not hold"
    if (index(v, "${{") > 0) return ""
    WfMxScalarWhy = ""
    WfMxScalarOk = 1
    return v
}

# Whether YAML -- 1.2's core schema, or 1.1's, since which one GitHub reads is not something this
# file says -- could type the plain scalar @p v as anything but a string.
function WorkflowYamlTyped(v,   l) {
    l = tolower(v)
    if (l ~ /^(null|~|true|false|yes|no|on|off|y|n)$/) return 1
    if (l ~ /^[-+]?\.(inf|nan)$/) return 1
    if (l ~ /^[-+]?[0-9][0-9_]*(\.[0-9_]*)?(e[-+]?[0-9]+)?$/) return 1
    if (l ~ /^[-+]?\.[0-9][0-9_]*(e[-+]?[0-9]+)?$/) return 1
    if (l ~ /^[-+]?0(x[0-9a-f_]+|o[0-7_]+|b[01_]+)$/) return 1
    if (l ~ /^[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+(\.[0-9_]*)?$/) return 1
    return 0
}

function WorkflowMatrixKindName(k) { return (k == "i") ? "an integer" : ((k == "b") ? "a boolean" : "a string") }

# Whether two matrix values are the same: 0 when their texts differ, 1 when they agree -- and a
# REFUSAL when the texts agree and the kinds do not, because whether GitHub matches `10` with `"10"`
# is not modelled, and matching them wrongly removes a leg in silence.
function WorkflowMatrixSame(v1, k1, v2, k2) {
    if (v1 != v2) return 0
    if (k1 != k2) WorkflowMatrixUnread("it compares `" v1 "`, written as " WorkflowMatrixKindName(k1) ", with `" v2 "`, written as " WorkflowMatrixKindName(k2) ", and whether GitHub matches the two is not modelled", WfMxAt)
    return 1
}

# Refuses @p key when another key of this matrix differs from it only in CASE: whether GitHub reads
# `os` and `OS` as one key is not modelled, and read as two, a row GitHub would find overwriting an
# original value merges instead -- a leg missing in silence.
function WorkflowMatrixKeyCase(key,   l) {
    l = tolower(key)
    if ((l in WfMxFolded) && WfMxFolded[l] != key) WorkflowMatrixUnread("the keys `" WfMxFolded[l] "` and `" key "` differ only in case, and whether GitHub reads them as one key is not modelled", WfMxAt)
    WfMxFolded[l] = key
}

# Set @p key in combination @p c, recording the key's position the first time it is set.
function WorkflowComboSet(c, key, value) {
    if (!((c, key) in WfCombo)) WfComboKey[c, ++WfComboKeyCount[c]] = key
    WfCombo[c, key] = value
}

# Whether the exclude row @p e matches the base choice @p pick (one value index per axis).
function WorkflowMatrixExcluded(e, pick,   k, key, a) {
    for (k = 1; k <= WfMxRowKeyN["exclude", e]; k++) {
        key = WfMxRowKey["exclude", e, k]; a = WfMxAxisIdx[key]
        if (!WorkflowMatrixSame(WfMxAxisVal[a, pick[a]], WfMxAxisKind[a, pick[a]],
                                WfMxRowVal["exclude", e, key], WfMxRowKind["exclude", e, key])) return 0
    }
    return 1
}

# The job's combinations, from what its matrix block said. Called as the job ends, in every pass.
function WorkflowMatrixFinalize(   a, e, k, key, total, i, r, pick, excluded, base, c, fits, matched) {
    WorkflowComboReset()
    if (!WfMxSeen) { WorkflowMatrixSave(); return }
    WfHasMatrix = 1
    for (a = 1; a <= WfMxAxisN; a++)
        if (WfMxAxisVals[a] == 0) WorkflowMatrixUnread("the axis `" WfMxAxisName[a] "` has no values", WfMxAt)
    for (e = 1; e <= WfMxRowN["exclude"]; e++)
        for (k = 1; k <= WfMxRowKeyN["exclude", e]; k++)
            if (!(WfMxRowKey["exclude", e, k] in WfMxAxisIdx))
                WorkflowMatrixUnread("an `exclude` row names `" WfMxRowKey["exclude", e, k] "`, which is not an axis", WfMxAt)
    for (a = 1; a <= WfMxAxisN; a++) WorkflowMatrixKeyCase(WfMxAxisName[a])
    for (e = 1; e <= WfMxRowN["include"]; e++)
        for (k = 1; k <= WfMxRowKeyN["include", e]; k++) WorkflowMatrixKeyCase(WfMxRowKey["include", e, k])
    if (WfMxUnread != "") { WfMatrixUnread = WfMxUnread; WfComboCount = 0; WorkflowMatrixSave(); return }

    # The base: the cartesian product of the axes, the first varying slowest, less every exclude.
    # With NO axes there is no base combination at all, rather than one empty one: every include
    # row is then its own combination, and one empty base would absorb them all into one.
    WfComboCount = 0
    if (WfMxAxisN > 0) {
        total = 1
        for (a = 1; a <= WfMxAxisN; a++) total *= WfMxAxisVals[a]
        for (i = 0; i < total; i++) {
            split("", pick)
            r = i
            for (a = WfMxAxisN; a >= 1; a--) { pick[a] = r % WfMxAxisVals[a] + 1; r = int(r / WfMxAxisVals[a]) }
            excluded = 0
            for (e = 1; e <= WfMxRowN["exclude"] && !excluded; e++) excluded = WorkflowMatrixExcluded(e, pick)
            if (excluded) continue
            c = ++WfComboCount
            WfComboKeyCount[c] = 0
            for (a = 1; a <= WfMxAxisN; a++) {
                WorkflowComboSet(c, WfMxAxisName[a], WfMxAxisVal[a, pick[a]])
                WfMxBaseKind[c, WfMxAxisName[a]] = WfMxAxisKind[a, pick[a]]
            }
        }
    }
    base = WfComboCount

    # Each include row, in order, into every base combination whose ORIGINAL values it overwrites
    # none of -- and a row that fits none is a combination of its own.
    for (e = 1; e <= WfMxRowN["include"]; e++) {
        matched = 0
        for (c = 1; c <= base; c++) {
            fits = 1
            for (k = 1; k <= WfMxRowKeyN["include", e]; k++) {
                key = WfMxRowKey["include", e, k]
                if ((key in WfMxAxisIdx) && !WorkflowMatrixSame(WfCombo[c, key], WfMxBaseKind[c, key], WfMxRowVal["include", e, key], WfMxRowKind["include", e, key])) fits = 0
            }
            if (!fits) continue
            matched = 1
            for (k = 1; k <= WfMxRowKeyN["include", e]; k++) {
                key = WfMxRowKey["include", e, k]
                if (!(key in WfMxAxisIdx)) WorkflowComboSet(c, key, WfMxRowVal["include", e, key])
            }
        }
        if (matched) continue
        c = ++WfComboCount
        WfComboKeyCount[c] = 0
        for (k = 1; k <= WfMxRowKeyN["include", e]; k++)
            WorkflowComboSet(c, WfMxRowKey["include", e, k], WfMxRowVal["include", e, WfMxRowKey["include", e, k]])
    }
    if (WfComboCount == 0) WorkflowMatrixUnread("the matrix produces no combination", WfMxAt)
    # A comparison refused while combining leaves NO combinations, never the ones computed so far.
    if (WfMxUnread != "") { WfMatrixUnread = WfMxUnread; WfComboCount = 0 }
    WorkflowMatrixSave()
}

# The combinations as one string per job, for the next pass to start that job from: combinations
# split by \036, keys by \037, a key from its value by \035 -- none of which a YAML scalar here holds.
function WorkflowMatrixSave(   c, k, s) {
    s = ""
    for (c = 1; c <= WfComboCount; c++) {
        if (c > 1) s = s "\036"
        for (k = 1; k <= WfComboKeyCount[c]; k++)
            s = s (k > 1 ? "\037" : "") WfComboKey[c, k] "\035" WfCombo[c, WfComboKey[c, k]]
    }
    WfMatrixAt[WfJobStart] = s
    WfMatrixCountAt[WfJobStart] = WfComboCount
    WfMatrixHasAt[WfJobStart] = WfHasMatrix
    WfMatrixUnreadAt[WfJobStart] = WfMatrixUnread
}

function WorkflowMatrixRestore(   c, k, n, rows, pairs, p) {
    if (!(WfJobStart in WfMatrixHasAt) || !WfMatrixHasAt[WfJobStart]) return
    WfHasMatrix = 1
    WfMatrixUnread = WfMatrixUnreadAt[WfJobStart]
    WfComboCount = WfMatrixCountAt[WfJobStart]
    split(WfMatrixAt[WfJobStart], rows, "\036")
    for (c = 1; c <= WfComboCount; c++) {
        WfComboKeyCount[c] = 0
        n = split(rows[c], pairs, "\037")
        for (k = 1; k <= n; k++) {
            p = index(pairs[k], "\035")
            WorkflowComboSet(c, substr(pairs[k], 1, p - 1), substr(pairs[k], p + 1))
        }
    }
}

# @p s with every `${{ matrix.KEY }}` replaced by combination @p c's value for KEY, and by NOTHING
# where that combination has no such key -- which is what GitHub substitutes. A reference that names
# a key of this matrix only when case is folded is LEFT in place for the consumer to refuse, and so
# is any other shape of reference (`matrix.os.name`, `toJSON(matrix)`): whether and how GitHub
# resolves those is not modelled, and a guess would be the answer that reads as clean.
function WorkflowMatrixExpand(s, c,   out, token, key, value) {
    out = ""
    while (match(s, /\$\{\{[ \t]*matrix\.[A-Za-z_][A-Za-z0-9_-]*[ \t]*\}\}/)) {
        token = substr(s, RSTART, RLENGTH)
        key = token
        sub(/^\$\{\{[ \t]*matrix\./, "", key); sub(/[ \t]*\}\}$/, "", key)
        if ((c, key) in WfCombo) value = WfCombo[c, key]
        else if (WorkflowMatrixFoldedKey(key)) value = token
        else value = ""
        out = out substr(s, 1, RSTART - 1) value
        s = substr(s, RSTART + RLENGTH)
    }
    return out s
}

# Whether @p key names a key of some combination only when case is folded.
function WorkflowMatrixFoldedKey(key,   c, k, other) {
    for (c = 1; c <= WfComboCount; c++)
        for (k = 1; k <= WfComboKeyCount[c]; k++) {
            other = WfComboKey[c, k]
            if (other != key && tolower(other) == tolower(key)) return 1
        }
    return 0
}

# ---- a condition over a combination (#1540) -------------------------------------------------------
# Whether the `if:` expression @p cond is TRUE, FALSE or UNDECIDED for combination @p c: "T", "F"
# or "U". Decided only from the combination's matrix values and literals, with GitHub's operators
# `==`, `!=`, `!`, `&&`, `||` and parentheses; an empty condition is "T", because a step with no
# `if:` runs. Every other context (`steps.*`, `needs.*`, `github.*`), every function call and every
# operator this does not model (`<`, `[`) is UNDECIDED, and so is anything it cannot parse.
#
# **UNDECIDED is never read as false.** The logic is three-valued, so `U && F` is F and `U || T` is
# T -- a leg an `if:` rules out on its matrix terms alone is ruled out whatever else it says -- but
# `U && T` stays U. A consumer asking "can this step run on this leg" answers yes for U, which is
# the direction that keeps a refusal rather than dropping one.
#
# Comparison is GitHub's: strings case-insensitively, and a missing matrix key is null, which GitHub
# coerces to 0 against a string -- so it equals `''`, differs from `'x'`, and is undecided against
# a numeric string. A matrix value that could be a number or a boolean (`10`, `true`) is undecided
# too: the walk keeps its text, not its YAML type, and GitHub coerces by type. A key matching only
# case-folded is undecided, for the reason `WorkflowMatrixExpand` leaves one.
function WorkflowMatrixDecide(cond, c,   e, r) {
    e = WorkflowTrim(cond)
    if (e == "") return "T"
    if (index(e, "${{") > 0) {
        if (e !~ /^\$\{\{.*\}\}$/) return "U"
        e = substr(e, 4, length(e) - 5)
        if (index(e, "${{") > 0 || index(e, "}}") > 0) return "U"
    }
    if (!WorkflowExprTokens(e)) return "U"
    WfExPos = 1; WfExErr = 0; WfExCombo = c
    r = WorkflowExprTruth(WorkflowExprOr())
    if (WfExErr || WfExPos <= WfExN) return "U"
    return r
}

# Split @p e into `WfExKind[i]`/`WfExText[i]`, `WfExN` of them: `s` a string literal (its text, the
# doubled apostrophe undone), `i` a name or property path, `#` a number, `o` an operator. Returns 0
# on any character this does not model, which the caller reads as undecided.
function WorkflowExprTokens(e,   two, ch, v, i) {
    WfExN = 0
    while (e != "") {
        ch = substr(e, 1, 1); two = substr(e, 1, 2)
        if (ch == " " || ch == "\t") { e = substr(e, 2); continue }
        if (two == "&&" || two == "||" || two == "==" || two == "!=") { WorkflowExprAdd("o", two); e = substr(e, 3); continue }
        if (ch == "!" || ch == "(" || ch == ")" || ch == ",") { WorkflowExprAdd("o", ch); e = substr(e, 2); continue }
        if (ch == sq) {
            v = ""
            for (i = 2; i <= length(e); i++) {
                if (substr(e, i, 1) != sq) { v = v substr(e, i, 1); continue }
                if (substr(e, i + 1, 1) == sq) { v = v sq; i++; continue }
                break
            }
            if (i > length(e)) return 0
            WorkflowExprAdd("s", v); e = substr(e, i + 1); continue
        }
        if (match(e, /^[A-Za-z_][A-Za-z0-9_.-]*/)) { WorkflowExprAdd("i", substr(e, 1, RLENGTH)); e = substr(e, RLENGTH + 1); continue }
        if (match(e, /^[0-9][0-9A-Za-z.]*/)) { WorkflowExprAdd("#", substr(e, 1, RLENGTH)); e = substr(e, RLENGTH + 1); continue }
        return 0
    }
    return 1
}
function WorkflowExprAdd(k, t) { WfExKind[++WfExN] = k; WfExText[WfExN] = t }
function WorkflowExprAt(t) { return WfExPos <= WfExN && WfExKind[WfExPos] == "o" && WfExText[WfExPos] == t }

# The parser, one precedence level per function, GitHub's order: `||` below `&&` below `==`/`!=`
# below `!`. A value travels as `<kind>\034<text>`: `B` a truth value (T, F or U), `s` a string
# literal, `m` a matrix value, `n` null, `u` unknown.
function WorkflowExprOr(   a) {
    a = WorkflowExprAnd()
    while (!WfExErr && WorkflowExprAt("||")) { WfExPos++; a = "B\034" WorkflowKleeneOr(WorkflowExprTruth(a), WorkflowExprTruth(WorkflowExprAnd())) }
    return a
}
function WorkflowExprAnd(   a) {
    a = WorkflowExprCompare()
    while (!WfExErr && WorkflowExprAt("&&")) { WfExPos++; a = "B\034" WorkflowKleeneAnd(WorkflowExprTruth(a), WorkflowExprTruth(WorkflowExprCompare())) }
    return a
}
function WorkflowExprCompare(   a, op) {
    a = WorkflowExprUnary()
    if (WfExErr || !(WorkflowExprAt("==") || WorkflowExprAt("!="))) return a
    op = WfExText[WfExPos++]
    return "B\034" WorkflowExprEqual(a, WorkflowExprUnary(), op)
}
function WorkflowExprUnary(   t) {
    if (!WorkflowExprAt("!")) return WorkflowExprPrimary()
    WfExPos++
    t = WorkflowExprTruth(WorkflowExprUnary())
    return "B\034" ((t == "T") ? "F" : ((t == "F") ? "T" : "U"))
}
function WorkflowExprPrimary(   k, t, depth, key, r) {
    if (WfExPos > WfExN) { WfExErr = 1; return "u\034" }
    k = WfExKind[WfExPos]; t = WfExText[WfExPos]; WfExPos++
    if (k == "o" && t == "(") {
        r = WorkflowExprOr()
        if (!WorkflowExprAt(")")) { WfExErr = 1; return "u\034" }
        WfExPos++
        return r
    }
    if (k == "s") return "s\034" t
    if (k == "#") return "u\034"
    if (k != "i") { WfExErr = 1; return "u\034" }
    # A function call is undecided, its arguments skipped by their parentheses.
    if (WorkflowExprAt("(")) {
        for (depth = 0; WfExPos <= WfExN; WfExPos++) {
            if (WorkflowExprAt("(")) depth++
            else if (WorkflowExprAt(")") && --depth == 0) { WfExPos++; return "u\034" }
        }
        WfExErr = 1; return "u\034"
    }
    if (t == "true") return "B\034T"
    if (t == "false") return "B\034F"
    if (t == "null") return "n\034"
    if (t !~ /^matrix\.[A-Za-z_][A-Za-z0-9_-]*$/) return "u\034"
    key = substr(t, 8)
    if ((WfExCombo, key) in WfCombo) return "m\034" WfCombo[WfExCombo, key]
    if (WorkflowMatrixFoldedKey(key)) return "u\034"
    return "n\034"
}

# The truth of a value, as GitHub reads one in `if:`: an empty string and null are false, any
# other string true. A matrix value that could be a number or a boolean is undecided -- `0` and
# `false` are falsy as YAML types and truthy as strings.
function WorkflowExprTruth(v,   k, t) {
    k = substr(v, 1, 1); t = substr(v, 3)
    if (k == "B") return t
    if (k == "n") return "F"
    if (k == "m" && WorkflowExprTyped(t)) return "U"
    if (k == "s" || k == "m") return (t == "") ? "F" : "T"
    return "U"
}
function WorkflowExprTyped(t) { return t ~ /^(true|false|-?(0|[1-9][0-9]*))$/ }

# `==` or `!=` between two values, as a truth value.
function WorkflowExprEqual(a, b, op,   ka, kb, ta, tb, eq) {
    ka = substr(a, 1, 1); ta = substr(a, 3); kb = substr(b, 1, 1); tb = substr(b, 3)
    if (ka == "u" || kb == "u") return "U"
    if (ka == "B" || kb == "B") {
        if (ka != kb || ta == "U" || tb == "U") return "U"
        eq = (ta == tb)
    } else {
        if ((ka == "m" && WorkflowExprTyped(ta)) || (kb == "m" && WorkflowExprTyped(tb))) return "U"
        if (ka == "n" && kb == "n") eq = 1
        else if (ka == "n" || kb == "n") {
            # null against a string: both coerce to numbers, null to 0 and `''` to 0.
            if (ka == "n") ta = tb
            if (ta == "") eq = 1
            else if (ta ~ /^[ \t]*$/ || ta ~ /^[ \t]*[-+]?(\.?[0-9]|[Ii]nfinity|0[xXoObB])/) return "U"
            else eq = 0
        } else eq = (tolower(ta) == tolower(tb))
    }
    if (op == "!=") eq = !eq
    return eq ? "T" : "F"
}

function WorkflowKleeneAnd(a, b) {
    if (a == "F" || b == "F") return "F"
    return (a == "T" && b == "T") ? "T" : "U"
}
function WorkflowKleeneOr(a, b) {
    if (a == "T" || b == "T") return "T"
    return (a == "F" && b == "F") ? "F" : "U"
}

function WorkflowComboLabel(c,   k, out) {
    out = ""
    for (k = 1; k <= WfComboKeyCount[c]; k++)
        out = out (k > 1 ? ", " : "") WfComboKey[c, k] "=" WfCombo[c, WfComboKey[c, k]]
    return "(" out ")"
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
