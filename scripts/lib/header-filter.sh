# SPDX-License-Identifier: Apache-2.0
#
# clang-tidy's HeaderFilterRegex, asked the way clang-tidy asks it: against the ABSOLUTE path a
# header was opened by, in every SPELLING a supported build opens it by. Sourced, never run --
# by `scripts/local-gate.sh`, whose coverage check is the tidy sweep's precondition, and by
# `scripts/check-tidy-header-filter.sh` (`ctest -R tidy-header-filter`), which asks the same
# question on every platform. ONE predicate and ONE table of spellings, because two copies of a
# model of `llvm::Regex` are two places to be wrong in.
#
# ## Why spellings
#
# The path clang-tidy matches is the one the preprocessor OPENED, and on Windows that is not
# the POSIX one: the MSVC compile database spells its include directories with backslashes, so
# `#include <tests/X.hpp>` opens `D:\...\src/tests/X.hpp`. `.clang-tidy` spelled every separator
# `/`, and a Windows tidy run discarded every header finding while reading clean -- measured with
# clang-tidy 22.1.8: a naming violation planted in a header under src/tests/ reported 0 times
# under that filter and twice under `[/\\]`. The gate's coverage count read 363/363 under both,
# because it matched POSIX paths only: a guard that could not fail on the defect it guards.
#
# The two dimensions are independent, and each has already shipped a defect: WHERE a checkout
# lives (#1040 -- a pattern anchored on a directory name matched CI's `.../fastcached/` and no
# lane worktree) and HOW its separators are spelled (the above). So a verdict is taken at a root
# AND in every spelling of it.
#
# ## The regex engine
#
# `grep -E`, POSIX extended. clang-tidy's is `llvm::Regex`, POSIX extended too, and in both a
# backslash inside a bracket expression is literal -- `[/\\]` is "a slash or a backslash". A
# MODEL of the tool rather than the tool, sound for the constructs this filter has carried
# (classes, alternation, `.*`); checked against clang-tidy itself by hand, not by a registered
# check (see `header_filter_coverage` in `local-gate.sh`).
#
# bash 3.2, because a hygiene script `ctest` runs is constrained to it.

# Every spelling a supported build opens a header by. A NAME per row; `header_filter_spell`
# renders one. A name it does not know is refused rather than skipped, since a skipped spelling
# is one nothing checked.
HeaderFilterSpellings="posix windows msvc"

# Spell @p 2, an absolute POSIX path, the way spelling @p 1 opens it, into `HeaderFilterSpelled`
# -- a variable rather than stdout, so a loop over every header forks nothing per path. Measured:
# the per-path `$(...)` and `grep` it replaces cost 56 s for 363 headers on Git Bash, where a
# fork is expensive, against a 120 s ctest budget.
#
#   posix    as given.
#   windows  every separator a backslash, as a Windows build names its own sources.
#   msvc     the INCLUDE DIRECTORY in backslashes and the `#include` name in slashes: split at
#            the last `/src/`, which is where an `-I...\src` directory ends -- for this
#            repository's roots and for a dependency's (`catch2-src\src` + `/catch2/...`).
#            With no `/src/` in the path it is the windows spelling.
#
# @param 1 The spelling. @param 2 The path.
header_filter_spell_into() {
    local spelling="$1" path="$2" head tail
    case "$spelling" in
        posix) HeaderFilterSpelled="$path" ;;
        windows) HeaderFilterSpelled="${path//\//\\}" ;;
        msvc)
            case "$path" in
                */src/*)
                    head="${path%/src/*}/src"
                    tail="${path##*/src/}"
                    HeaderFilterSpelled="${head//\//\\}/${tail}"
                    ;;
                *) HeaderFilterSpelled="${path//\//\\}" ;;
            esac
            ;;
        *)
            echo "header-filter: no spelling named '${spelling}'" >&2
            return 2
            ;;
    esac
}

# Print @p 2 the way spelling @p 1 opens it. `header_filter_spell_into` for one path.
# @param 1 The spelling. @param 2 The path.
header_filter_spell() {
    header_filter_spell_into "$1" "$2" || return 2
    printf '%s\n' "$HeaderFilterSpelled"
}

# Say why a filter's grep exited above 1, on stderr. grep answers 2 both for a pattern it cannot
# compile and for failing itself, so the status alone cannot tell the FILTER's fault from the
# CHECK's: the pattern is compiled once more against no input, where only the pattern can fail. A
# grep that fails there too names the filter; one that compiles it names the check. grep's own
# message, if it printed one, is above.
# @param 1 `include` or `exclude`. @param 2 The regex. @param 3 The status the match exited with.
header_filter_grep_failure() {
    local probe=0
    grep -E -- "$2" < /dev/null > /dev/null 2>&1 || probe=$?
    if [[ "$probe" -gt 1 ]]; then
        echo "header-filter: the $1 filter '$2' does not compile as an extended regular expression (grep exited $3, and $probe again over no input) -- this is a verdict about the FILTER, not the tree" >&2
    else
        echo "header-filter: grep exited $3 matching the $1 filter, which compiles (grep exited $probe over no input), so which headers it selects is not known -- this is the CHECK failing, not a verdict about the filter" >&2
    fi
}

# Which of @p 4's paths the filter takes in ONE spelling, as `,<line>,<line>,` -- 1-based input
# lines, the shape `[[ $set == *",$i,"* ]]` asks. One `grep -n` per regex over the whole spelled
# list, never one per path.
# @param 1 The include regex. @param 2 The exclude regex, possibly empty.
# @param 3 The root. @param 4 The spelling. @param 5 Repo-relative paths, one per line, no blanks.
header_filter_taken_lines() {
    local include="$1" exclude="$2" root="$3" spelling="$4" paths="$5" path spelled="" line taken excluded set=","
    while IFS= read -r path; do
        # Blank lines are skipped here as the callers' classifying loops skip them, so a line
        # number and a path's index are the same count.
        [[ -n "$path" ]] || continue
        header_filter_spell_into "$spelling" "$root/$path" || return 2
        spelled="${spelled}${HeaderFilterSpelled}"$'\n'
    done < <(printf '%s\n' "$paths")
    # Each grep's status is CHECKED, and the line numbers are cut in this shell: grep answers 1
    # for "matched nothing", and anything above it -- a grep killed, a regex it cannot compile --
    # left an empty set behind `|| true`, which reads as "the filter takes nothing" (or "excludes
    # nothing") and decides the verdict in whichever direction that happens to point.
    local status matched=""
    taken="$(grep -nE -- "$include" < <(printf '%s' "$spelled"))" && status=0 || status=$?
    if [[ "$status" -gt 1 ]]; then
        header_filter_grep_failure include "$include" "$status"
        return 2
    fi
    excluded=","
    if [[ -n "$exclude" ]]; then
        matched="$(grep -nE -- "$exclude" < <(printf '%s' "$spelled"))" && status=0 || status=$?
        if [[ "$status" -gt 1 ]]; then
            header_filter_grep_failure exclude "$exclude" "$status"
            return 2
        fi
        while IFS= read -r line; do
            [[ -n "$line" ]] && excluded="${excluded}${line%%:*},"
        done < <(printf '%s\n' "$matched")
    fi
    while IFS= read -r line; do
        line="${line%%:*}"
        [[ -n "$line" && "$excluded" != *",${line},"* ]] && set="${set}${line},"
    done < <(printf '%s\n' "$taken")
    printf '%s' "$set"
}

# The non-blank lines of stdin, and how many.
# @param 1 Name of the variable to receive the lines. @param 2 Name of the variable for the count.
header_filter_read_paths() {
    local path lines="" count=0
    while IFS= read -r path; do
        [[ -n "$path" ]] || continue
        lines="${lines}${path}"$'\n'
        count=$((count + 1))
    done
    printf -v "$1" '%s' "${lines%$'\n'}"
    printf -v "$2" '%s' "$count"
}

# COVERAGE: how many repo-relative paths on stdin the filter takes in EVERY spelling, as
# `<matched>/<total>`, plus the first path it misses -- and, when that path IS taken in some
# spelling and not another, ` (<spelling> spelling)`, naming the first spelling that missed it.
# A path missed in every spelling carries no suffix: that is a LAYOUT or pattern question, and
# naming a spelling would send the reader to the wrong cause.
#
# @param 1 The include regex. @param 2 The exclude regex, possibly empty.
# @param 3 The root the paths live under -- the filter sees absolute paths.
# @param 4 The spellings to ask, default all of `HeaderFilterSpellings`. A case about where a
#          checkout LIVES passes `posix`, so it asserts that and not the separators.
header_filter_match() {
    local include="$1" exclude="$2" root="$3" spellings="${4:-$HeaderFilterSpellings}"
    local paths total matched=0 firstMissed="" path spelling index=0 k missedIn takenIn
    local -a sets names
    header_filter_read_paths paths total
    k=0
    for spelling in $spellings; do
        sets[k]="$(header_filter_taken_lines "$include" "$exclude" "$root" "$spelling" "$paths")" || return 2
        names[k]="$spelling"
        k=$((k + 1))
    done
    while IFS= read -r path; do
        [[ -n "$path" ]] || continue
        index=$((index + 1))
        missedIn=""
        takenIn=""
        for k in "${!names[@]}"; do
            if [[ "${sets[k]}" == *",${index},"* ]]; then
                takenIn="${names[k]}"
            elif [[ -z "$missedIn" ]]; then
                missedIn="${names[k]}"
            fi
        done
        if [[ -z "$missedIn" ]]; then
            matched=$((matched + 1))
        elif [[ -z "$firstMissed" ]]; then
            firstMissed="$path"
            [[ -n "$takenIn" ]] && firstMissed="$path (${missedIn} spelling)"
        fi
    done < <(printf '%s\n' "$paths")
    echo "${matched}/${total}${firstMissed:+ $firstMissed}"
}

# REACH: how many repo-relative paths on stdin the filter takes in ANY spelling, as
# `<reached>/<total>`, plus the first one reached and in which spelling. The question for a
# path that must stay OUT -- a dependency, a third-party root -- where a filter taking it in one
# spelling is a leak on that platform, and `header_filter_match`'s every-spelling count would
# read it as 0.
#
# @param 1 The include regex. @param 2 The exclude regex, possibly empty.
# @param 3 The root. @param 4 The spellings, default all.
header_filter_reach() {
    local include="$1" exclude="$2" root="$3" spellings="${4:-$HeaderFilterSpellings}"
    local paths total reached=0 firstReached="" path spelling index=0 k
    local -a sets names
    header_filter_read_paths paths total
    k=0
    for spelling in $spellings; do
        sets[k]="$(header_filter_taken_lines "$include" "$exclude" "$root" "$spelling" "$paths")" || return 2
        names[k]="$spelling"
        k=$((k + 1))
    done
    while IFS= read -r path; do
        [[ -n "$path" ]] || continue
        index=$((index + 1))
        for k in "${!names[@]}"; do
            if [[ "${sets[k]}" == *",${index},"* ]]; then
                reached=$((reached + 1))
                [[ -z "$firstReached" ]] && firstReached="$path (${names[k]} spelling)"
                break
            fi
        done
    done < <(printf '%s\n' "$paths")
    echo "${reached}/${total}${firstReached:+ $firstReached}"
}

# One key of a `.clang-tidy`, as the file spells it between single quotes, or nothing.
# @param 1 The file. @param 2 The key: `HeaderFilterRegex` or `ExcludeHeaderFilterRegex`.
header_filter_config() {
    local all
    all="$(sed -n "s/^$2:[[:space:]]*'\(.*\)'[[:space:]]*$/\1/p" "$1")"
    printf '%s' "${all%%$'\n'*}"
}

# The packages a tree declares, one per line, sorted: the name every CPM call that fetches a
# package gives it -- `CPMAddPackage` and `CPMFindPackage` by their `NAME`, `CPMGetPackage` by its
# first argument -- in its tracked first-party CMake files, LOWERCASED, because that is how CPM
# names the package's
# cache directory (`Catch2` unpacks under `.../catch2/<hash>/`). DERIVED rather than listed, so a
# dependency added tomorrow is asked about without anybody remembering this file; a comment line
# is skipped, and a NAME is taken only inside a call. Refuses -- stderr and status 2, never an
# empty answer -- when the tree declares none: zero is a parser that found nothing, not a tree
# with no dependencies. And every CALL is accounted for: one with no NAME -- the shorthand
# `CPMAddPackage("gh:owner/repo@1.0")` -- is refused BY FILE AND LINE, because dropping it would
# answer about fewer packages than the tree declares and read like the whole set; so is one whose
# NAME is spelled through a variable, which no reading of the files can resolve. Needs
# `scripts/lib/third-party-roots.sh` sourced (`first_party_paths`).
#
# The commands are a TABLE (`form` in the reader), matched case-insensitively as CMake matches
# command names, each row saying how its call names the package: `keyword` (`NAME <x>`),
# `positional` (the first argument, `CPMGetPackage(<x>)`), or `declare` -- `CPMDeclarePackage`
# records a declaration that a later `CPMGetPackage` fetches, so it is walked, so that nothing
# inside it is misread, and adds nothing by itself. Matching only `CPMAddPackage` dropped the other
# three in silence, the same fail-OPEN class as a dropped call.
#
# BLIND SPOT, and the direction it fails in: a CPM command reached INDIRECTLY --
# `cmake_language(CALL CPMAddPackage ...)`, `cmake_language(EVAL CODE ...)`, or a project wrapper
# whose own name is not in the table and which calls CPM through a variable -- is not a call this
# reader sees. It fails SILENT, which is OPEN: that package is simply absent from the derived set,
# and a header filter reaching its headers passes. Not built, deliberately: resolving it means
# evaluating CMake, which this reader exists not to do. The tree has no such call today; the table
# is where a wrapper this project writes would be added as a row.
# @param 1 The repository root.
header_filter_declared_packages() {
    local root="$1" tracked firstParty found reader names unnamed dynamic malformed bracket file
    local -a files=()
    tracked="$(git -C "$root" ls-files -- '*.cmake' '*CMakeLists.txt' 2>/dev/null)"
    if [[ -z "$tracked" ]]; then
        echo "header-filter: no tracked CMake file under ${root}, so which packages it declares cannot be read" >&2
        return 2
    fi
    firstParty="$(first_party_paths "$root" "$tracked")" || return 2
    # The files as ARGUMENTS to one awk, never through `xargs`: xargs reports 123 for any failure
    # of what it ran, so the reader's own status -- the one its message on stderr explains -- was
    # replaced by xargs's. A handful of CMake files is far below any argument limit.
    # Each as `./<file>`: awk reads an operand of the shape `name=value` as an ASSIGNMENT and
    # never opens it, and `-` as stdin, so a tracked `x=y.cmake` was skipped in silence -- which
    # fails OPEN. A `./` prefix makes every one of them a path; the reader strips it again
    # (`substr(FILENAME, 3)`), so a refusal names the file as git does.
    while IFS= read -r file; do
        [[ -n "$file" ]] && files+=("./$file")
    done < <(printf '%s\n' "$firstParty")
    if [[ "${#files[@]}" -eq 0 ]]; then
        echo "header-filter: every tracked CMake file under ${root} is third-party, so which packages this tree declares cannot be read" >&2
        return 2
    fi
    # One line per CALL: `name <x>` for one that names its package, `dynamic <file>:<line>` for one
    # whose NAME reaches a variable reference (`NAME ${dep}` in a foreach, `NAME $ENV{X}`, and
    # `NAME foo_${x}`, whose literal prefix read alone would be a WRONG name rather than a missing
    # one), `unnamed <file>:<line>` for one that closed -- or met the next call, or the end of its
    # file -- without a NAME.
    #
    # A call is WALKED, not matched per line: `walk` reads `bare` left to right, so a line holding
    # two calls derives both -- one match per line took the first and dropped the second, which
    # fails OPEN -- and a call closes on the `)` that balances its `(`, so an unquoted `X(y)`
    # argument before the NAME does not end it. The NAME's value may sit on the next line: CMake
    # separates arguments by any whitespace, newlines included. A call's outcome is RECORDED as its
    # keywords are read and printed when it closes, so the LAST `NAME` decides, as it does in
    # `cmake_parse_arguments` -- the first one read as the name was a package CPM never fetches --
    # and the value is stepped over once read, so a value spelled `NAME` is not a keyword.
    #
    # A value is a NAME only when the literal ENDS where CMake's argument does: whitespace, `)`,
    # `;` (a list separator), a closing `"` or the end of the line. Anything else -- `foo::bar`,
    # `foo/bar`, `foo@1`, `foo\;bar` -- is `malformed <file>:<line> <token>`, refused by name:
    # reading the literal's prefix derived `foo`, a real-looking WRONG package, which fails OPEN.
    # A value given as a BRACKET argument (`NAME [[Foo]]`) is `bracket <file>:<line>`, refused with
    # a remedy that says so: it is a legal spelling this reader does not unwrap, and "no NAME" or
    # "not a plain name" would each send its reader looking for the wrong fault.
    #
    # Each line is SCANNED once into `code` -- what CMake reads of it, comments dropped -- and
    # `bare`, the same length with every character of a quoted or bracket argument, and every
    # escape sequence, replaced by `_`. Whether a quoted or bracket argument is open, and whether a
    # bracket comment is, CARRIES ACROSS LINES, as it does in CMake's own lexer: a `CPMAddPackage(`
    # inside `#[[ ... ]]` or `#[=[ ... ]=]` (closed only by a `]`, as many `=`, `]`) is not a call,
    # which counted a package the tree does not declare, and a `NAME Wrong` on the second line of a
    # quoted or bracket argument is not the NAME. An escape outside quotes -- `-DX=\"y\"` -- opens
    # no quote, or the one it seemed to open would now run on past the line.
    # The `)` that closes a call, the `(` that opens one and the NAME keyword are asked of `bare`,
    # so `GIT_TAG v1.0 # (pinned)` and `OPTIONS "X=$(y)"` before the NAME no longer close the call
    # early -- which refused two legitimate calls as having no NAME -- and `OPTIONS "A NAME B"`
    # before it is not read as `NAME B`, a WRONG package. The VALUE is read from `code` at the
    # keyword's position, which is why the placeholder is not a blank: blanks would let the
    # keyword's trailing whitespace run on across a quoted value. And the command is matched
    # CASE-INSENSITIVELY, as CMake matches it: `cpmaddpackage(NAME Lower)` was dropped in silence,
    # which fails OPEN. NAME stays uppercase: it is a `cmake_parse_arguments` keyword, and those are
    # case-sensitive.
    found="$(cd "$root" && awk '
        BEGIN {
            form["cpmaddpackage"] = "keyword"
            form["cpmfindpackage"] = "keyword"
            form["cpmgetpackage"] = "positional"
            form["cpmdeclarepackage"] = "declare"
        }
        function blank(n,    s) { s = ""; while (n-- > 0) s = s "_"; return s }
        function scan(line,    i, ch, n, shut) {
            code = ""; bare = ""; n = length(line); i = 1
            while (i <= n) {
                ch = substr(line, i, 1)
                if (inbracket) {
                    shut = "]" equals "]"
                    if (substr(line, i, length(shut)) == shut) {
                        if (!bracketcomment) { code = code shut; bare = bare blank(length(shut)) }
                        inbracket = 0; i += length(shut)
                        continue
                    }
                    if (!bracketcomment) { code = code ch; bare = bare "_" }
                    i++
                    continue
                }
                if (ch == "\\" && i < n) {
                    code = code substr(line, i, 2); bare = bare "__"; i += 2
                    continue
                }
                if (quoted) {
                    code = code ch; bare = bare "_"; i++
                    if (ch == "\"") quoted = 0
                    continue
                }
                if (ch == "#") {
                    if (!match(substr(line, i + 1), /^\[=*\[/)) break
                    inbracket = 1; bracketcomment = 1; equals = substr(line, i + 2, RLENGTH - 2)
                    i += 1 + RLENGTH
                    continue
                }
                if (ch == "[" && (code == "" || substr(code, length(code), 1) ~ /[[:space:](]/) && match(substr(line, i), /^\[=*\[/)) {
                    inbracket = 1; bracketcomment = 0; equals = substr(line, i + 1, RLENGTH - 2)
                    code = code substr(line, i, RLENGTH); bare = bare blank(RLENGTH); i += RLENGTH
                    continue
                }
                if (ch == "\"") quoted = 1
                code = code ch; bare = bare (quoted ? "_" : ch); i++
            }
        }
        function finish() { if (kind != "declare") print (result != "" ? result : "unnamed " at); open = 0; want = 0 }
        function walk(    p, n, rest, c, value, quote, lit, after, token, cmd) {
            n = length(bare); p = 1
            while (p <= n) {
                rest = substr(bare, p)
                if (want) {
                    if (match(rest, /^[[:space:]]+/)) { p += RLENGTH; continue }
                    want = 0
                    if (substr(code, p, 1) == "[" && substr(bare, p, 1) == "_") {
                        result = "bracket " at
                        if (match(rest, /^[^[:space:]()]+/)) p += RLENGTH
                        continue
                    }
                    value = substr(code, p)
                    quote = (substr(value, 1, 1) == "\"")
                    if (quote) value = substr(value, 2)
                    match(value, /^[A-Za-z0-9_.+-]*/)
                    lit = substr(value, 1, RLENGTH); after = substr(value, RLENGTH + 1, 1)
                    if (after == "$") result = "dynamic " at
                    else if (quote ? after == "\"" : (after == "" || after ~ /[[:space:]);"]/)) result = (lit != "" ? "name " tolower(lit) : "")
                    else {
                        token = substr(code, p); sub(/[[:space:]].*/, "", token)
                        result = "malformed " at " " token
                    }
                    if (match(rest, /^[^[:space:]()]+/)) p += RLENGTH
                    continue
                }
                if (match(rest, /^[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/) && (p == 1 || substr(bare, p - 1, 1) !~ /[A-Za-z0-9_]/)) {
                    cmd = tolower(substr(rest, 1, RLENGTH)); sub(/[[:space:]]*\($/, "", cmd)
                    if (cmd in form) {
                        if (open) finish()
                        open = 1; depth = 1; result = ""; kind = form[cmd]; at = substr(FILENAME, 3) ":" FNR
                        want = (kind == "positional")
                        p += RLENGTH
                        continue
                    }
                }
                if (open) {
                    c = substr(bare, p, 1)
                    if (c == "(") depth++
                    else if (c == ")") {
                        if (--depth == 0) finish()
                    } else if (kind == "keyword" && match(rest, /^NAME([[:space:]]|$)/) && (p == 1 || substr(bare, p - 1, 1) ~ /[[:space:](]/)) {
                        want = 1; p += 4
                        continue
                    }
                }
                p++
            }
        }
        FNR == 1 { if (open) finish(); quoted = 0; inbracket = 0 }
        { scan($0); walk() }
        END { if (open) finish() }
    ' "${files[@]}")" && reader=0 || reader=$?
    # The READER's own failure is a third outcome, not a tree: awk that cannot parse its program
    # prints nothing, which the refusals below would name as "no CPMAddPackage", and awk that
    # cannot open one tracked file reads the rest and exits non-zero with a partial answer that
    # would otherwise pass as the whole set.
    if [[ "$reader" != 0 ]]; then
        echo "header-filter: the CPM reader failed over the tracked CMake files under ${root} (awk exit ${reader}; its own message is above), so which packages the tree declares is not known -- this is the CHECK failing, not a verdict about the tree" >&2
        return 2
    fi
    # The reader's lines split by their tag in THIS shell, never one `sed` per tag: a sed that
    # failed or was killed printed nothing, and an empty list here is a refusal not made -- a
    # malformed or unnamed call passed, and the derived set asked about fewer packages than the
    # tree declares, which fails OPEN. Round 8's silent grep was the same shape one call away.
    local line rest named=""
    unnamed=""; dynamic=""; malformed=""; bracket=""
    while IFS= read -r line; do
        rest="${line#* }"
        case "$line" in
            "unnamed "*) unnamed="${unnamed}${rest}"$'\n' ;;
            "dynamic "*) dynamic="${dynamic}${rest}"$'\n' ;;
            "malformed "*) malformed="${malformed}${rest%% *} (${rest#* })"$'\n' ;;
            "bracket "*) bracket="${bracket}${rest}"$'\n' ;;
            "name "*) named="${named}${rest}"$'\n' ;;
        esac
    done < <(printf '%s\n' "$found")
    unnamed="${unnamed%$'\n'}"; dynamic="${dynamic%$'\n'}"; malformed="${malformed%$'\n'}"; bracket="${bracket%$'\n'}"
    # Both refusals are SAID before either returns, so a tree holding both shapes names every call
    # at once rather than one kind per run.
    [[ -z "$unnamed" ]] \
        || echo "header-filter: CPM call(s) with no NAME -- a CPMAddPackage or CPMFindPackage in the shorthand, or a CPMGetPackage with no argument -- so the package each adds cannot be named and asked about: $(printf '%s\n' "$unnamed" | tr '\n' ' ')-- give each call a NAME (CPM's own long form) rather than the shorthand" >&2
    [[ -z "$dynamic" ]] \
        || echo "header-filter: CPM call(s) whose NAME is spelled through a variable, so the name cannot be derived statically: $(printf '%s\n' "$dynamic" | tr '\n' ' ')-- this derivation reads the files and runs no CMake, so it would ask about fewer packages than the tree declares, or about a wrong one; spell each NAME as a literal, one call per package" >&2
    [[ -z "$malformed" ]] \
        || echo "header-filter: CPM call(s) whose NAME is not a plain package name -- it runs on past the name, where only whitespace, ')', ';', a closing quote or the end of the line may follow: $(printf '%s\n' "$malformed" | tr '\n' ' ')-- reading its prefix would ask about a package the tree does not declare; spell the NAME as the package's plain name" >&2
    [[ -z "$bracket" ]] \
        || echo "header-filter: CPM call(s) whose NAME is given as a bracket argument ([[...]]), a spelling CMake accepts and this reader does not unwrap: $(printf '%s\n' "$bracket" | tr '\n' ' ')-- spell the NAME as a plain literal, or as a quoted one" >&2
    [[ -z "$unnamed" && -z "$dynamic" && -z "$malformed" && -z "$bracket" ]] || return 2
    names=""
    if [[ -n "$named" ]]; then
        local sorted=0
        names="$(sort -u < <(printf '%s' "$named"))" || sorted=$?
        if [[ "$sorted" != 0 || -z "$names" ]]; then
            echo "header-filter: sort exited ${sorted} over the package names derived under ${root}, so which packages the tree declares is not known -- this is the CHECK failing, not a verdict about the tree" >&2
            return 2
        fi
    fi
    if [[ -z "$names" ]]; then
        echo "header-filter: no CPM call naming a package (CPMAddPackage, CPMFindPackage, CPMGetPackage) in the tracked CMake files under ${root}; refused rather than read as a tree with no dependencies" >&2
        return 2
    fi
    printf '%s\n' "$names"
}

# Where each package's headers would be, repo-relative, in the two layouts this project unpacks
# into -- FetchContent's `_deps/<name>-src/` under a build tree, and `.cache/CPM/<name>/<hash>/`,
# where `build.yml` points `CPM_SOURCE_CACHE` -- and the two shapes a package ships its headers
# in, `src/<name>/` and `include/<name>/`. PATHS rather than files that must exist: a dependency
# tree is untracked and a build directory may not be there yet, and a check that only bites after
# a build does not bite when it is first needed. Pure.
# @param ... The package names, lowercased.
header_filter_dependency_layout() {
    local name shape
    for name in "$@"; do
        for shape in src include; do
            printf 'out/build/gate-clang-debug/_deps/%s-src/%s/%s/x.hpp\n' "$name" "$shape" "$name"
            printf '.cache/CPM/%s/0123456789abcdef/%s/%s/x.hpp\n' "$name" "$shape" "$name"
        done
    done
}

# Every dependency path a filter must not reach for the tree at @p 1: its declared packages in
# every layout. Refuses exactly as `header_filter_declared_packages`.
# @param 1 The repository root.
header_filter_dependency_paths() {
    local names name
    names="$(header_filter_declared_packages "$1")" || return 2
    while IFS= read -r name; do
        header_filter_dependency_layout "$name"
    done < <(printf '%s\n' "$names")
}
