# SPDX-License-Identifier: Apache-2.0
#
# The text a shell would expand, as an awk library.
#
# Loaded with `awk -f scripts/lib/shell-lex.awk -f <the check>.awk`: awk has no include
# directive, and multiple `-f` is POSIX -- measured on gawk 5.3.2 under both Git Bash and WSL,
# with a negative control showing the same program refused without this file, so the functions
# demonstrably come from here (#1456).
#
# Extracted verbatim from `check-workflow-step-env.sh`, which was the only depth-relative reader
# of the five and the only one with a self-test driving folded scalars and heredocs. Not one
# character of the awk changed in the move, so that self-test is what proves the extraction
# behaviour-preserving.
#
# ## The state these functions own, named because awk has no other way to say it
#
#   lexState        "", "single" or "double" -- the quote a line ended inside, carried across lines
#   lexCode         set by `lex`: the same line with every character inside a quote masked
#   heredocEnd      the delimiter an open bash heredoc ends at, or ""
#   heredocQuoted   whether that heredoc's delimiter was quoted, so its body does not expand
#   hereString      "", "single" or "double" -- an open PowerShell here-string
#   sq              a single quote, as `sprintf("%c", 39)`; the program cannot spell one directly
#
# A caller sets `sq` in its own `BEGIN` and resets the rest per file. This file declares no
# `BEGIN` of its own, deliberately: two `BEGIN` blocks across `-f` files both run, and a library
# that initialised state would silently outrank or be outrun by the caller's own depending on
# the order of the `-f` flags.
#
# ## What is NOT here, and why
#
# `looseHere` and `advance` model bash and PowerShell CONSTRUCT nesting -- loops, functions,
# brace blocks -- which is #1461's ordering rule rather than lexing, so they stay with the check
# that owns that rule. The line between the two is whether a function answers *what text would a
# shell see* (here) or *what does this repository require of it* (the check).

# ## And `check-unguarded-prerequisites.sh`'s `Blank()` is NOT this, however much it looks like it
#
# #1456 describes the tree as having "two bash lexers" and asks for one. **Driven against each
# other they disagree on nearly every line, and each is right for its own rule** -- so merging
# them breaks one. Measured:
#
#   echo "$HOME is expanded"           lexCode: echo "________     Blank(): echo  $HOME
#   echo "$(sed -e 's/x/y/' f)"        lexCode: echo "________     Blank(): echo  $(sed -e      f)
#
# `lexCode` masks everything inside ANY quote, double quotes included, because the rule here asks
# *where is a name ASSIGNED* and an assignment cannot happen inside a string. `Blank()` keeps
# double-quoted content as code and blanks only single-quoted content, because its rule asks
# *which guards and invocations does this script RUN* -- and a command inside `"$( ... )"` is run,
# so masking it would hide the invocations that check exists to find.
#
# What this file does NOT model, stated so nobody reads the absence as a defect: `$( )` is not a
# quoting context here. `Blank()` models it and carries a fix (#1416) for getting it wrong. That
# is not a gap here, because this lexer tracks no parentheses at all -- it tracks quote state, and
# treating a `'` as literal inside a `"` reaches the same end-of-line state a shell does on every
# balanced line, verified against `bash -n`. Where the two differ is the VISIBLE text, and there
# this one is the more permissive, which for a guard is the fail-closed direction: it can see a
# read a shell would not perform, which is a false refusal somebody meets rather than a silent
# pass.

    # ---- the text a shell would expand ------------------------------------------------------------------------------
    # One lexer state per body, carried across lines, because a quoted string may span them. Inside single quotes
    # every `$` becomes `_`, so nothing there reads a name while the words, the quotes and a heredoc delimiter stay
    # where they were -- `read -d '' name` still has a value before the name. @p esc is the escape character
    # (a backslash in bash, a backtick in PowerShell); @p heredocs says whether `<<` opens one.
    # Beside the text it returns, it leaves lexCode: the same line with every character inside a quote masked, which
    # is what an assignment is looked for in -- `echo "please read NAME"` assigns nothing.
    function lex(s, esc, heredocs,   out, i, c, n, heredocAt, t, quoted, wasQuoted) {
        out = ""; lexCode = ""; n = length(s); heredocAt = 0
        for (i = 1; i <= n; i++) {
            c = substr(s, i, 1)
            if (lexState == "single") {
                if (c == sq) { lexState = ""; lexCode = lexCode c } else lexCode = lexCode "_"
                out = out (c == "$" ? "_" : c); continue
            }
            if (c == esc) { i++; continue }
            # A `#` starting a word outside any quote begins a comment in both shells, so the rest of the line is
            # neither a read nor a quote -- an apostrophe in `# do not` must not open a string that hides the step.
            if (lexState == "" && c == "#" && (i == 1 || substr(s, i - 1, 1) ~ /[ \t;]/)) break
            wasQuoted = (lexState == "double" && c != "\"")
            if (lexState == "" && c == sq) lexState = "single"
            else if (c == "\"") lexState = (lexState == "double") ? "" : "double"
            else if (heredocs && lexState == "" && substr(s, i, 2) == "<<" && substr(s, i + 2, 1) != "<" && (i == 1 || substr(s, i - 1, 1) != "<"))
                if (!heredocAt) heredocAt = length(out) + 1
            out = out c
            lexCode = lexCode (wasQuoted ? "_" : c)
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
    # This library defines every function it calls. It did NOT: as extracted it called a `trim`
    # that lives in one consumer, so a second consumer would have died at its first heredoc --
    # fatally, and only once a heredoc appeared, which is the per-reached-path silence measured on
    # #1456 arriving in the same change that measured it.
    function lexTrim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function bashVisible(s) {
        lexCode = ""
        if (heredocEnd != "") {
            if (lexTrim(s) == heredocEnd) { heredocEnd = ""; return "" }
            if (heredocQuoted) return ""
            # An unquoted heredoc expands, but a quote in it is a character, not a string.
            gsub(/\\\$/, "", s)
            return s
        }
        return lex(s, "\\", 1)
    }
    # A here-string opens where the CODE of a line ends in `@` and a quote, so the opener is looked for in what the
    # lexer returns: a comment ending in one opens nothing, and the lines after it are still code.
    function pwshVisible(s,   t, before, out) {
        t = lexTrim(s); lexCode = ""
        if (hereString == "single") { if (substr(t, 1, 2) == sq "@") hereString = ""; return "" }
        if (hereString == "double") { if (substr(t, 1, 2) == "\"@") { hereString = ""; return "" } return s }
        before = lexState
        out = lex(s, "`", 0)
        t = out; sub(/[ \t]+$/, "", t)
        if (before == "" && lexState == "single" && substr(t, length(t) - 1) == "@" sq) { hereString = "single"; lexState = ""; return substr(t, 1, length(t) - 2) }
        if (before == "" && lexState == "double" && substr(t, length(t) - 1) == "@\"") { hereString = "double"; lexState = ""; return substr(t, 1, length(t) - 2) }
        return out
    }

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
    function countWord(code, w,   n, t, re) {
        n = 0; t = code
        re = "(^|[;&|(){])[ \t]*" w "([ \t;&|)}]|$)"
        while (match(t, re)) { n++; t = substr(t, RSTART + RLENGTH - 1) }
        return n
    }
    function countChar(code, ch,   n, i, len) {
        n = 0; len = length(code)
        for (i = 1; i <= len; i++) if (substr(code, i, 1) == ch) n++
        return n
    }
    # A bash function declaration whose body starts at the brace or on the line below. `f() { echo; }` written on one
    # line is not one: it IS that line, which the grain note above covers.
    function isFnOpen(code) {
        return (code ~ /^[ \t]*([A-Za-z_][A-Za-z0-9_]*[ \t]*\(\)|function[ \t]+[A-Za-z_][A-Za-z0-9_]*([ \t]*\(\))?)[ \t]*\{?[ \t]*$/)
    }
