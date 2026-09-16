# SPDX-License-Identifier: Apache-2.0
#
# `check-workflow-step-env.sh`'s reader: every `run:` step of a workflow, the names it reads, and
# whether each is in scope at the line that reads it.
#
# In a file rather than inline in the script since #1456, because the shell lexer it shares with
# other readers is loaded as an awk library and `-f` cannot be mixed with an inline program. Run
# as:
#
#   awk -v ... -f scripts/lib/shell-lex.awk -f scripts/check-workflow-step-env.awk <file> <file>
#
# The file is passed TWICE on purpose -- `FNR == 1` counts the passes, and the second pass is
# what judges, so the first can learn the workflow-level facts a step's verdict needs.
#
# The quoting constraint that shaped this program is still in force even though it is no longer
# inside a single-quoted shell string: `sq = sprintf("%c", 39)` stays, because the SELF-TEST
# stages fragments of this program's vocabulary and a literal quote here would be one more thing
# that has to agree across two files.

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
            if (pass >= 2) printf "EXPRREAD\t%d\t%s\t%s\t%s\t%s\n", atLine, stepName, "-", name, ""
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
            if ((name in workflowEnv) || (name in jobEnv) || (name in stepEnv)) continue
            if ((name in jobWritten) || (name in jobExported)) continue
            refuse(exprOther[name], "undefined-expression", "reads ${{ env." name " }} and nothing in scope supplies it")
        }
        for (name in exprEnvValue) {
            if ((name in workflowEnv) || (name in jobEnv)) continue
            if ((name in jobWritten) || (name in jobExported)) continue
            refuse(exprEnvValue[name], "undefined-expression",
                   "reads ${{ env." name " }} in this step`s own `env:` block, where its own `env:` is not yet in force")
        }
    }
    # What this step hands to the steps AFTER it. Adopted after the judging above, so neither an
    # action nor a write can satisfy a read in the very step that performs it -- an expression is
    # substituted before the step runs, and an action exports only once it has run.
    function adoptExports(   i, n, parts, row) {
        if (stepUses == "") return
        printf "USES\t%d\t%s\t%s\t%s\t%s\n", stepLine, stepName, "-", stepUses, ""
        n = split(exports, parts, " ")
        for (i = 1; i <= n; i++) {
            if (split(parts[i], row, "|") != 2) continue
            if (row[1] == stepUses) jobExported[row[2]] = 1
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
    function flush(   model, i, name, missing, n, fold, t, at, code) {
        if (!inStep) return
        inStep = 0
        if (pass < 2) return
        # The expressions BEFORE the `hasRun` return, because a `uses:` step has no `run:` at all
        # and it is the step that EXPORTS -- returning first would leave every action export
        # unadopted and refuse the reads they supply.
        judgeExpressions()
        if (plant) { printf "PLANTEDEXPR\t%d\t%s\t%d\t%d\t%s\n", stepLine, stepName, (PlantExprName in exprEnvValue), stepEnvRows, ""; delete exprEnvValue[PlantExprName] }
        adoptExports()
        if (!hasRun) return
        scanned++
        model = resolveShell()
        if (model == "?") { refuse(stepLine, "unresolved-shell", "runs-on `" runsOn "`"); return }
        if (substr(model, 1, 1) == "!") { refuse(stepLine, "unknown-shell", "shell `" substr(model, 2) "`"); return }
        split("", defs); split("", refs); split("", visible); lexState = ""; heredocEnd = ""; hereString = ""
        loopDepth = 0; inFn = 0; fnBrace = 0; fnStarted = 0; pwBrace = 0; constructBroken = ""
        models[model]++
        fold = (model == "pwsh")
        # The plant is one more line, read LAST and by the same lexer, so a body that leaves a quote, a heredoc or a
        # here-string open hides it -- which is what the plant exists to catch.
        if (plant) { bodyAt[nbody] = stepLine; body[nbody++] = fold ? "$null = $env:" PlantName : ": \"$" PlantName "\"" }
        # No line is skipped for starting with `#`: the lexer ends a comment itself, and inside a heredoc or a
        # here-string such a line is text that expands -- a Markdown heading in a report body. A heredoc line is
        # text, too, so `NAME=$NAME` written into a file assigns nothing.
        for (i = 0; i < nbody; i++) {
            curLine = i
            loose = looseHere(fold)
            if (fold) { t = pwshVisible(body[i]); code = lexCode; pwshScan(t) }
            else { t = bashVisible(body[i]); code = lexCode; if (code != "") bashDefs(code); bashRefs(t) }
            # The RAW line, not the lexed code: the name in `echo "NAME=$v" >> "$GITHUB_ENV"` sits
            # inside quotes, which the lexer masks. Adopted after this step has been judged above.
            noteWrites(body[i])
            advance(code, fold)
        }
        if (constructBroken == "" && (inFn || loopDepth != 0))
            constructBroken = inFn ? "a function whose braces never close" : "a `do` with no `done`"
        if (plant) {
            printf "PLANTED\t%d\t%s\t%d\n", stepLine, stepName, (PlantName in refs)
            delete refs[PlantName]
        }
        # `defs` is deliberately NOT admitted into `visible`: a scope and the allowlist hold for a whole script
        # while an assignment holds from its own line down, and folding the two throws that line away.
        admit(stepEnv, fold); admit(jobEnv, fold); admit(workflowEnv, fold)
        if (fold) admit(windowsSet, fold); else admit(bashSet, fold)
        missing = ""; n = 0
        for (name in refs) {
            if (name in visible) continue
            at = refs[name]
            if (!(name in defs)) { missing = missing (n++ ? ", " : "") (fold ? "$env:" : "$") name; continue }
            if (at == -1 || defs[name] <= at) continue
            # Refused at the line of the READ rather than at the line of the step, because the remedy is a line to
            # move, and a step can be a hundred lines long.
            refuse(bodyAt[at], "order", "reads " (fold ? "$env:" : "$") name " here, and this script assigns it further down, at line " bodyAt[defs[name]])
        }
        if (n) refuse(stepLine, "undefined", "reads " missing)
        if (constructBroken != "") refuse(stepLine, "unreadable-construct", constructBroken)
    }
    function resetStep() {
        split("", stepEnv); split("", body); split("", bodyAt)
        split("", exprOther); split("", exprEnvValue); stepUses = ""; stepEnvRows = 0
        nbody = 0; hasRun = 0; inStep = 1; stepName = "(unnamed)"; stepLine = FNR; stepShell = ""; stepKeyIndent = -1
    }
    # The `env:`, `defaults` and `runs-on` may follow its `steps:`, so the second pass starts each job from what the
    # first pass found anywhere in that job.
    function resetJob(   k, kp) {
        split("", jobEnv); split("", jobWritten); split("", jobExported)
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

    # ---- the count ---------------------------------------------------------------------------------------------------
    # Every `run:` key outside a scalar, found WITHOUT the reader: no steps, no jobs, no key columns, only which lines a
    # scalar owns. A line the reader places as a script and a line counted here are compared in END, so a step the
    # reader stops recognising -- keys at a column it did not expect -- is refused by line rather than read as clean.
    # A script body is a scalar, so a heredoc writing `run:` into a file is text here too, as in the reader.
    function count(s,   lead, key, v) {
        if (countOwner >= 0 && (s ~ /^[ \t]*$/ || indentOf(s) > countOwner)) return
        countOwner = -1
        if (match(s, /^[ ]*(-[ ]+)*/)) { lead = substr(s, 1, RLENGTH); gsub(/-/, " ", lead); s = lead substr(s, RLENGTH + 1) }
        if (!match(s, /^[ ]*[A-Za-z_][A-Za-z0-9_.-]*[ \t]*:([ \t]|$)/)) return
        key = substr(s, RSTART, RLENGTH)
        sub(/^[ ]*/, "", key); sub(/[ \t]*:[ \t]*$/, "", key)
        v = trim(substr(s, RSTART + RLENGTH))
        if (substr(v, 1, 1) != "\"" && substr(v, 1, 1) != sq) sub(/(^|[ \t]+)#.*$/, "", v)
        if (key == "run") { counted[FNR] = trim(s); runKeys++ }
        if (v != "") countOwner = indentOf(s)
    }
    function place() { if (pass == 2) placed[FNR] = 1 }

    # The text of a YAML-quoted scalar @p v, which must close on its own line with at most a comment after it, and sets
    # yamlOk to say whether it did. YAML quoting is not shell quoting, so the quotes must not reach a shell model, where
    # they would hide every read between them: a doubled apostrophe in single quotes is one apostrophe, and in double
    # quotes an escaped quote, backslash or slash is that character. Any other escape, a quote left open for the next
    # line, or text after the closing quote, is not read at all -- yamlOk is 0 and the caller refuses the step.
    function yamlQuoted(v,   q, i, n, ch, e, out) {
        q = substr(v, 1, 1); n = length(v); out = ""; yamlOk = 0
        for (i = 2; i <= n; i++) {
            ch = substr(v, i, 1)
            if (ch == q) {
                if (q == sq && substr(v, i + 1, 1) == sq) { out = out sq; i++; continue }
                yamlOk = (substr(v, i + 1) ~ /^([ \t]+#.*)?$/)
                return yamlOk ? out : ""
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

    BEGIN {
        sq = sprintf("%c", 39)
        n = split(bashNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") bashSet[p[i]] = 1
        n = split(windowsNames, p, " "); for (i = 1; i <= n; i++) if (p[i] != "") windowsSet[p[i]] = 1
        n = split(shells, p, " "); for (i = 1; i <= n; i++) if (split(p[i], q, "|") == 2) shellModel[q[1]] = q[2]
        n = split(remedies, p, "\036"); for (i = 1; i <= n; i++) if ((k = index(p[i], "|")) > 0) remedy[substr(p[i], 1, k - 1)] = substr(p[i], k + 1)
        pass = 0; workflowShell = ""; countOwner = -1; resetAll()
    }

    FNR == 1 { pass++; resetAll() }

    {
        line = $0
        sub(/\r$/, "", line)
        ind = indentOf(line)
        blank = (line ~ /^[ \t]*$/)
        if (pass == 2) count(line)

        # A block scalar, or a plain scalar continuing, owns every blank or more-indented line after its key.
        if (blockOwner >= 0) {
            if (blank || ind > blockOwner) {
                if (blockKind == "run") { bodyAt[nbody] = FNR; body[nbody++] = blank ? line : substr(line, blockOwner + 1) }
                # A continuation line of one of this step`s values. A `#` line inside a `run:` body
                # is SHELL text and its expressions are still substituted, so it is read here --
                # unlike a YAML comment, which the site below never sees.
                if (inStep && inSteps) noteExpr(line, FNR, inEnv && envScope == "step" && ind > envIndent)
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

        # Every other line of a step: its keys and their inline values, and the rows of its
        # `with:` and `env:` blocks. AFTER the boundary handling above, so a step`s first line is
        # read as that step`s and not as its predecessor`s, and after the comment skip, so a YAML
        # comment explaining an expression is not read as carrying one.
        if (inStep && inSteps) {
            inOwnEnvRow = (inEnv && envScope == "step" && ind > envIndent)
            if (inOwnEnvRow) stepEnvRows++
            # The plant goes in HERE, at the read site, on every env row of every step -- not
            # synthesised in the judging. Synthesised, it would prove the judging works and say
            # nothing about whether the reader ever VISITED the env block of a step, which is the
            # half that can silently stop being true. A value written as a BLOCK SCALAR is read at
            # the continuation site above and is not planted; stated rather than covered, because
            # no step in this tree writes an `env:` value that way.
            noteExpr((plant && inOwnEnvRow) ? line " ${{ env." PlantExprName " }}" : line, FNR, inOwnEnvRow)
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
        # A comment may be all there is after the colon, and the block below the key is then its value.
        if (c != "\"" && c != sq) sub(/(^|[ \t]+)#.*$/, "", value)

        # A block scalar indicator hands the lines after it to the block; any other inline value may continue as a
        # plain scalar on more-indented lines, which are its text and never keys.
        if (inStep && inSteps && stepKeyIndent < 0 && ind > itemIndent) stepKeyIndent = ind
        stepKey = (inStep && inSteps && ind == stepKeyIndent)
        if (value ~ /^[|>][-+0-9]*$/) {
            blockOwner = ind
            blockKind = (stepKey && key == "run") ? "run" : "skip"
            if (blockKind == "run") { hasRun = 1; place() }
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
            if (key == "run" && value == "") { defaultsRun = ind; place(); next }
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
        else if (key == "uses") { stepUses = trim(unquote(value)); sub(/@.*$/, "", stepUses) }
        else if (key == "shell") stepShell = trim(unquote(value))
        else if (key == "env") openEnv("step")
        else if (key == "run") {
            place()
            if (c == "*" || c == "&" || c == "!" || c == "[" || c == "{") refuse(FNR, "unreadable-run", "`run: " value "`")
            else {
                if (c == sq || c == "\"") {
                    t = yamlQuoted(value)
                    if (!yamlOk) { refuse(FNR, "unreadable-run", "`run: " value "` is a quoted scalar this check cannot read on one line"); next }
                    value = t
                }
                hasRun = 1
                bodyAt[nbody] = FNR; body[nbody++] = value
                # A plain scalar continues on more-indented lines, including one whose first line is empty.
                blockOwner = ind; blockKind = "run"
            }
        }
    }

    END {
        flush()
        stepName = "(none)"
        for (i = 1; i <= FNR; i++) {
            if (i in placed) placedKeys++
            if ((i in counted) && !(i in placed)) refuse(i, "unplaced-run", "`" counted[i] "` is a `run:` key the reader placed in no step")
            else if ((i in placed) && !(i in counted)) refuse(i, "unplaced-run", "the reader placed a `run:` script on a line the count found no `run:` key on")
        }
        printf "STEPS\t%d\t%d\t%d\t%d\t%d\n", scanned, models["bash"], models["pwsh"], runKeys, placedKeys
    }
