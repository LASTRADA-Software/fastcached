# SPDX-License-Identifier: Apache-2.0
#
# `check-workflow-step-env.sh`'s rule: for every `run:` step of a workflow, the names it reads and
# whether each is in scope at the line that reads it.
#
# The WALK is not here. `scripts/lib/workflow-walk.awk` reads the YAML and hands this program one
# step record at a time through a single hook; this file is #1175's scope rule and #1461's ordering
# rule over that record, plus the shell models both need. Run as:
#
#   awk -v ... -f scripts/lib/shell-lex.awk -f scripts/lib/workflow-walk.awk \
#       -f scripts/check-workflow-step-env.awk <file> <file>
#
# The file is passed TWICE on purpose -- the walk counts the passes from `FNR == 1`, and the second
# pass is what judges, so the first can learn the workflow-level facts a step's verdict needs.
#
# Every `Wf`-prefixed name below is the walk's, documented in its own header and read-only here.
# The one exception the walk permits is appending the planted line to `WfBody[]` from the `step`
# hook, which `judgeStep` does and which the neuter depends on.
    function refuse(at, kind, detail) {
        if (WfPass < 2) return
        printf "REFUSE\t%d\t%s\t%s\t%s\t%s\n", at, (reportName != "" ? reportName : WfStepName), kind, detail, remedy[kind]
    }

    # ---- bash ----------------------------------------------------------------------------------------------------
    # The position just past the shell word @p r starts with: quotes, `$( )`, `( )` and `${ }` keep it going.
    function bashDefs(s,   t, rest, name, nchain) {
        t = s
        sub(/^[ \t]+/, "", t)
        if (t ~ /^(export|readonly|local|declare|typeset)[ \t]/) {
            sub(/^(export|readonly|local|declare|typeset)[ \t]+(-[A-Za-z]+[ \t]+)*/, "", t)
            while (match(t, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                define(substr(t, RSTART, RLENGTH))
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
            if (rest == "" || rest ~ /^[;&|)}#]/) for (name in chain) define(chain[name])
        }
        # bash32-scan: data-begin
        if (match(s, /(mapfile|readarray)[ \t]+(-[A-Za-z]+([ \t]+[^ \t-][^ \t]*)?[ \t]+)*[A-Za-z_][A-Za-z0-9_]*/)) {
        # bash32-scan: data-end
            rest = substr(s, RSTART, RLENGTH)
            if (match(rest, /[A-Za-z_][A-Za-z0-9_]*$/)) define(substr(rest, RSTART, RLENGTH))
        }
        if (match(s, /(^|[^A-Za-z0-9_])for[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+in([ \t]|$)/)) {
            rest = substr(s, RSTART, RLENGTH)
            sub(/^[^A-Za-z0-9_]*for[ \t]+/, "", rest)
            if (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) define(substr(rest, RSTART, RLENGTH))
        }
        if (match(s, /(^|[^A-Za-z0-9_])getopts[ \t]+[^ \t]+[ \t]+[A-Za-z_][A-Za-z0-9_]*/)) {
            rest = substr(s, RSTART, RLENGTH)
            if (match(rest, /[A-Za-z_][A-Za-z0-9_]*$/)) define(substr(rest, RSTART, RLENGTH))
        }
        rest = s
        while (match(rest, /(^|[^A-Za-z0-9_])read[ \t]+/)) {
            rest = substr(rest, RSTART + RLENGTH)
            while (match(rest, /^-[A-Za-z]+[ \t]+/)) {
                t = substr(rest, 1, RLENGTH)
                rest = substr(rest, RLENGTH + 1)
                # -a takes the ARRAY it reads into, which is a name; -d, -n, -N, -p, -t and -u take a value, which is
                # not one.
                if (t ~ /a[ \t]+$/ && match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) define(substr(rest, 1, RLENGTH))
                if (t ~ /[adnNptu][ \t]+$/ && match(rest, /^[^ \t]+[ \t]*/)) rest = substr(rest, RLENGTH + 1)
            }
            while (match(rest, /^[A-Za-z_][A-Za-z0-9_]*/)) {
                define(substr(rest, RSTART, RLENGTH))
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
            noteRef(name)
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
            if (rest ~ /^\}?[ \t]*=[^=]/) define(name)
            else noteRef(name)
            s = rest
        }
    }

    # ---- the `${{ }}` expressions a step carries -------------------------------------------------------------------
    # A `${{ env.NAME }}` naming nothing in scope is substituted with an EMPTY STRING, with no
    # error and no warning (#1460). That is #1174 through a second door, and the convention this
    # check itself established -- bring an exported name into a step as `NAME: ${{ env.NAME }}` --
    # concentrates the risk in exactly that spelling: the shell half then passes, because the row
    # defines the name, while the row itself was never read.
    #
    # TWO BUCKETS, and the split is the whole point rather than a detail:
    #
    #   * a read in the step`s `run:`, `with:`, `if:` or `name:` IS satisfied by the step`s own
    #     `env:`, which is in force by the time any of those is used;
    #   * a read inside the step`s own `env:` VALUES is not. GitHub substitutes those before the
    #     step`s environment exists, so a typo`d `NAME: ${{ env.NAME }}` row would otherwise
    #     satisfy ITSELF -- the one case this exists to catch.
    #
    # `if:` is in the first bucket and no step in this tree reads an expression there, so that
    # arm is carried by a case alone. It is the PERMISSIVE side of an uncertainty about when the
    # runner applies a step`s env, and permissive is the right side to err on for a bucket whose
    # only effect is to satisfy reads: the strict reading would refuse workflows that work.
    #
    # Job-level and workflow-level fields -- `runs-on`, a job`s `env:` values, a job`s `if:` --
    # are OUT of scope and unread. So is every context other than `env.`: `github.`, `secrets.`,
    # `matrix.` and `needs.` name things this check has no model of, and a model more permissive
    # than the thing it stands for is what waves through the one read that matters.
    function noteExpr(s, atLine, inOwnEnv,   name, rest) {
        rest = s
        while (match(rest, /\$\{\{[ \t]*env\.[A-Za-z_][A-Za-z0-9_]*/)) {
            name = substr(rest, RSTART, RLENGTH)
            sub(/^\$\{\{[ \t]*env\./, "", name)
            if (inOwnEnv) { if (!(name in exprEnvValue)) exprEnvValue[name] = atLine }
            else if (!(name in exprOther)) exprOther[name] = atLine
            # Every read, wherever it sits, so the bash layer can refuse an `ActionExports` row
            # whose NAME nothing reads any more. Gated on the second pass like every other record:
            # the file is read TWICE, and an ungated printf reported every expression twice.
            if (WfPass >= 2) printf "EXPRREAD\t%d\t%s\t%s\t%s\t%s\n", atLine, WfStepName, "-", name, ""
            rest = substr(rest, RSTART + RLENGTH)
        }
    }
    # A line that NAMES `GITHUB_ENV` writes every `NAME=` on it, and that one rule covers all
    # four spellings this tree could use -- bash `echo`/`printf` redirected with `>>`, PowerShell
    # `>> $env:GITHUB_ENV`, and `Add-Content` naming it -- because in every one of them the name
    # being written is a `NAME=` token on the same line. Three rows behaving identically would be
    # decoration; the spellings are named here and one case each pins them.
    #
    # STATED BLIND SPOT: a heredoc form (`cat >> "$GITHUB_ENV" <<EOF`) writes names on LATER
    # lines and is not seen. The tree has two writes, both the `echo` form.
    function noteWrites(s,   name, rest) {
        if (index(s, "GITHUB_ENV") == 0) return
        rest = s
        while (match(rest, /[A-Za-z_][A-Za-z0-9_]*=/)) {
            name = substr(rest, RSTART, RLENGTH - 1)
            jobWritten[name] = 1
            rest = substr(rest, RSTART + RLENGTH)
        }
    }

    # ---- where in a script a name is defined ---------------------------------------------------------------------
    # A definition is a LINE, and a read is judged against what is defined at or above its own line, because that is
    # what bash does: a script reading `$X` above `X=1` dies on that line under `set -u`, and without `-u` expands it
    # EMPTY and takes a branch the wrong way, which is the worse half of #1174.
    #
    # Two constructs take the meaning out of a line number, and they are the whole of the relaxation:
    #
    #   * a LOOP body runs AGAIN, so an assignment at its end reaches a read at its start on the next iteration;
    #   * a FUNCTION body runs wherever it is CALLED, which may be below every assignment its caller makes.
    #
    # A read inside either is judged against the whole script -- which is what EVERY read was judged against before
    # #1461, so the ordering strengthens the top level and can refuse nothing inside a construct that passed before.
    #
    # A `trap` handler needs no rule, and that is a finding rather than an omission. A single-quoted handler is not
    # expanded at all, so the lexer has already masked its `$` and no read is seen in it; a double-quoted one is
    # expanded WHERE `trap` IS CALLED, so ordering is already the right answer there. Both are cases, because a
    # relaxation that could never fire is worse than none -- nothing would ever say it had stopped working.
    #
    # The grain is a LINE, so a read and an assignment on ONE line are not ordered against each other: a one-line
    # loop (`for x in a; do echo "$X"; X=1; done`) is accepted. Stated rather than closed -- a finer grain would have
    # to interleave the construct walk with the read walk, and this tree has never held the shape.
    function define(name) { if (!(name in defs)) defs[name] = curLine }
    # A STRICT read outranks a loose one for the same name, and the FIRST strict read wins, because it is the one
    # that fails: -1 means judged against the whole script, and any other value means judged at that line.
    function noteRef(name) {
        if (loose) { if (!(name in refs)) refs[name] = -1; return }
        if (!(name in refs) || refs[name] == -1 || refs[name] > curLine) refs[name] = curLine
    }
    # How many times the reserved word @p w stands in COMMAND position in @p code. It is reserved only there:
    # `echo done` is an argument, and counting it would close a loop that never opened.
    # Whether the line about to be read lies inside a construct. Asked BEFORE the tokens of that line are counted, so
    # the OPENING line of a construct is judged strictly -- the reads it carries happen once, before the body.
    function looseHere(fold) {
        if (fold) return (pwBrace > 0)
        return (loopDepth > 0 || fnStarted)
    }
    # Carry the construct state across the line just read, over the CODE the lexer left, so a `do`, a `done` or a
    # brace inside a string or a heredoc is masked and moves nothing.
    #
    # In PowerShell the loops and the functions are brace-delimited, and so are the `if` blocks, so every
    # brace-enclosed block counts as a construct there: telling one from another needs a parser this reader does not
    # have, and the cost is that PowerShell ordering is enforced at the top level of a script only. No pwsh step in
    # this tree assigns an environment name at all -- measured, `$env:NAME =` appears nowhere under
    # `.github/workflows` -- so that half is carried by cases rather than by the files.
    function advance(code, fold) {
        if (code == "") return
        if (fold) {
            pwBrace += countChar(code, "{") - countChar(code, "}")
            if (pwBrace < 0) { pwBrace = 0; constructBroken = "a `}` with no `{`" }
            return
        }
        loopDepth += countWord(code, "do") - countWord(code, "done")
        if (loopDepth < 0) { loopDepth = 0; constructBroken = "a `done` with no `do`" }
        if (!inFn && isFnOpen(code)) { inFn = 1; fnBrace = 0; fnStarted = 0 }
        if (inFn) {
            fnBrace += countChar(code, "{") - countChar(code, "}")
            if (fnBrace > 0) fnStarted = 1
            if (fnStarted && fnBrace <= 0) { inFn = 0; fnStarted = 0 }
        }
    }

    # Every expression this step carries, against every source that can supply one.
    #
    # Compared AS WRITTEN, with no case folding: the `env` context is not a shell, and the
    # PowerShell arm above folds only what a PowerShell script READS.
    function judgeExpressions(   name) {
        for (name in exprOther) {
            if ((name in WfWorkflowEnv) || (name in WfJobEnv) || (name in WfStepEnv)) continue
            if ((name in jobWritten) || (name in jobExported)) continue
            refuse(exprOther[name], "undefined-expression", "reads ${{ env." name " }} and nothing in scope supplies it")
        }
        for (name in exprEnvValue) {
            if ((name in WfWorkflowEnv) || (name in WfJobEnv)) continue
            if ((name in jobWritten) || (name in jobExported)) continue
            refuse(exprEnvValue[name], "undefined-expression",
                   "reads ${{ env." name " }} in this step`s own `env:` block, where its own `env:` is not yet in force")
        }
    }
    # What this step hands to the steps AFTER it. Adopted after the judging above, so neither an
    # action nor a write can satisfy a read in the very step that performs it -- an expression is
    # substituted before the step runs, and an action exports only once it has run.
    function adoptExports(   i, n, parts, row) {
        if (WfStepUses == "") return
        printf "USES\t%d\t%s\t%s\t%s\t%s\n", WfStepLine, WfStepName, "-", WfStepUses, ""
        n = split(exports, parts, " ")
        for (i = 1; i <= n; i++) {
            if (split(parts[i], row, "|") != 2) continue
            if (row[1] == WfStepUses) jobExported[row[2]] = 1
        }
    }

    # ---- one step --------------------------------------------------------------------------------------------------
    function resolveShell(   s) {
        s = WfStepShell != "" ? WfStepShell : (WfJobShell != "" ? WfJobShell : WfWorkflowShell)
        if (s == "") {
            if (WfRunsOn ~ /\$\{\{/) return "?"
            if (tolower(WfRunsOn) ~ /windows/) s = "pwsh"
            else if (tolower(WfRunsOn) ~ /(ubuntu|macos|linux)/) s = "bash"
            else return "?"
        }
        sub(/[ \t].*$/, "", s)
        return (s in shellModel) ? shellModel[s] : "!" s
    }
    # The names a step of @p model can see: every scope, its own assignments, and its shell family allowlist.
    # PowerShell compares them upper-cased, as Windows does.
    function admit(src, fold,   k) { for (k in src) visible[fold ? toupper(k) : k] = 1 }
    # One step, judged. Reached from the `step` kind of `WorkflowOn` below, so the walk owns
    # WHEN a step has ended and this owns what that means -- the `inStep` guard that used to
    # open this function belongs to the library now, which fires the kind only for a step it
    # had opened.
    function judgeStep(   model, i, name, missing, n, fold, t, at, code) {
        if (WfPass < 2) return
        # The expressions BEFORE the `hasRun` return, because a `uses:` step has no `run:` at all
        # and it is the step that EXPORTS -- returning first would leave every action export
        # unadopted and refuse the reads they supply.
        judgeExpressions()
        if (plant) { printf "PLANTEDEXPR\t%d\t%s\t%d\t%d\t%s\n", WfStepLine, WfStepName, (PlantExprName in exprEnvValue), WfStepEnvRows, ""; delete exprEnvValue[PlantExprName] }
        adoptExports()
        if (!WfHasRun) return
        scanned++
        model = resolveShell()
        if (model == "?") { refuse(WfStepLine, "unresolved-shell", "runs-on `" WfRunsOn "`"); return }
        if (substr(model, 1, 1) == "!") { refuse(WfStepLine, "unknown-shell", "shell `" substr(model, 2) "`"); return }
        split("", defs); split("", refs); split("", visible); lexState = ""; heredocEnd = ""; hereString = ""
        loopDepth = 0; inFn = 0; fnBrace = 0; fnStarted = 0; pwBrace = 0; constructBroken = ""
        models[model]++
        fold = (model == "pwsh")
        # The plant is one more line, read LAST and by the same lexer, so a body that leaves a quote, a heredoc or a
        # here-string open hides it -- which is what the plant exists to catch.
        if (plant) { WfBodyAt[WfBodyCount] = WfStepLine; WfBody[WfBodyCount++] = fold ? "$null = $env:" PlantName : ": \"$" PlantName "\"" }
        # No line is skipped for starting with `#`: the lexer ends a comment itself, and inside a heredoc or a
        # here-string such a line is text that expands -- a Markdown heading in a report body. A heredoc line is
        # text, too, so `NAME=$NAME` written into a file assigns nothing.
        for (i = 0; i < WfBodyCount; i++) {
            curLine = i
            loose = looseHere(fold)
            if (fold) { t = pwshVisible(WfBody[i]); code = lexCode; pwshScan(t) }
            else { t = bashVisible(WfBody[i]); code = lexCode; if (code != "") bashDefs(code); bashRefs(t) }
            # The RAW line, not the lexed code: the name in `echo "NAME=$v" >> "$GITHUB_ENV"` sits
            # inside quotes, which the lexer masks. Adopted after this step has been judged above.
            noteWrites(WfBody[i])
            advance(code, fold)
        }
        if (constructBroken == "" && (inFn || loopDepth != 0))
            constructBroken = inFn ? "a function whose braces never close" : "a `do` with no `done`"
        if (plant) {
            printf "PLANTED\t%d\t%s\t%d\n", WfStepLine, WfStepName, (PlantName in refs)
            delete refs[PlantName]
        }
        # `defs` is deliberately NOT admitted into `visible`: a scope and the allowlist hold for a whole script
        # while an assignment holds from its own line down, and folding the two throws that line away.
        admit(WfStepEnv, fold); admit(WfJobEnv, fold); admit(WfWorkflowEnv, fold)
        if (fold) admit(windowsSet, fold); else admit(bashSet, fold)
        missing = ""; n = 0
        for (name in refs) {
            if (name in visible) continue
            at = refs[name]
            if (!(name in defs)) { missing = missing (n++ ? ", " : "") (fold ? "$env:" : "$") name; continue }
            if (at == -1 || defs[name] <= at) continue
            # Refused at the line of the READ rather than at the line of the step, because the remedy is a line to
            # move, and a step can be a hundred lines long.
            refuse(WfBodyAt[at], "order", "reads " (fold ? "$env:" : "$") name " here, and this script assigns it further down, at line " WfBodyAt[defs[name]])
        }
        if (n) refuse(WfStepLine, "undefined", "reads " missing)
        if (constructBroken != "") refuse(WfStepLine, "unreadable-construct", constructBroken)
    }
    # ---- this check`s own per-scope state ------------------------------------------------------
    # The walk owns the workflow`s structure; these four arrays are this check`s records, cleared
    # on the same boundaries. A step`s are cleared AFTER `judgeStep` has read them, which is why
    # the hook does both in that order rather than resetting on the way in.
    function resetCheckStep() { split("", exprOther); split("", exprEnvValue) }
    function resetCheckJob() { split("", jobWritten); split("", jobExported); resetCheckStep() }

    # The tables this check is given, and nothing the walk owns: `sq`, the pass counter and every
    # scope flag are set by `scripts/lib/workflow-walk.awk`, whose `BEGIN` runs first because it is
    # named first on the command line.
    BEGIN {
        n = split(bashNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") bashSet[p[i]] = 1
        n = split(windowsNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") windowsSet[p[i]] = 1
        n = split(shells, p, " "); for (i = 1; i <= n; i++) if (split(p[i], q, "|") == 2) shellModel[q[1]] = q[2]
        n = split(remedies, p, "\036"); for (i = 1; i <= n; i++) if ((k = index(p[i], "|")) > 0) remedy[substr(p[i], 1, k - 1)] = substr(p[i], k + 1)
        reportName = ""
        resetCheckJob()
    }

    FNR == 1 { resetCheckJob() }

    # The ONE function `scripts/lib/workflow-walk.awk` calls. Every kind it can emit has an arm
    # here, the three this check makes no use of included -- spelled as a returning arm rather than
    # left to fall off the end, because a kind nobody decided about and a kind decided to be
    # uninteresting are otherwise the same silence.
    function WorkflowOn(kind) {
        if (kind == "raw") return              # the `run:` count is the walk`s own
        # `text` fires for a continuation ANYWHERE since the walk stopped confining it to
        # steps; this rule is about a step`s own values, so it says so rather than relying on
        # the walk to have confined it.
        if (kind == "text" && WfScope != "step") return
        if (kind == "text" || kind == "step-line") {
            # A `run:` body line and a step`s own key line both carry `${{ env.* }}`. The plant
            # goes in HERE, at the read site, on every env row of every step -- not synthesised in
            # the judging. Synthesised, it would prove the judging works and say nothing about
            # whether the reader ever VISITED the env block of a step, which is the half that can
            # silently stop being true. A value written as a BLOCK SCALAR arrives as `text` and is
            # not planted; stated rather than covered, because no step in this tree writes an
            # `env:` value that way.
            noteExpr((plant && WfEnvRow && kind == "step-line") ? WfLine " ${{ env." PlantExprName " }}" : WfLine,
                     WfAt, WfEnvRow)
            return
        }
        if (kind == "key") return              # the record the walk built is what this check reads
        if (kind == "step") { judgeStep(); resetCheckStep(); return }
        if (kind == "job") { resetCheckJob(); return }
        if (kind == "refusal") { refuse(WfAt, WfRefuseKind, WfRefuseDetail); return }
        if (kind == "end") { report(); return }
    }

    # Every `run:` key the walk COUNTED without placing it in a step, and every script it placed on
    # a line the count found no key on. Two directions, because either one alone is satisfied by a
    # reader that sees nothing and a count that sees nothing.
    function report(   i, placedKeys) {
        reportName = "(none)"
        for (i = 1; i <= FNR; i++) {
            if (i in WfPlaced) placedKeys++
            if ((i in WfCounted) && !(i in WfPlaced)) refuse(i, "unplaced-run", "`" WfCounted[i] "` is a `run:` key the reader placed in no step")
            else if ((i in WfPlaced) && !(i in WfCounted)) refuse(i, "unplaced-run", "the reader placed a `run:` script on a line the count found no `run:` key on")
        }
        printf "STEPS\t%d\t%d\t%d\t%d\t%d\n", scanned, models["bash"], models["pwsh"], WfRunKeys, placedKeys
    }
