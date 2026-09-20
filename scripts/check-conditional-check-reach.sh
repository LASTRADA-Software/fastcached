#!/usr/bin/env bash
#
# A check registered only under a build OPTION must have a CI job that both configures
# that build and runs `ctest` in it (#1254, #589).
#
# THE DEFECT THIS EXISTS FOR. `tidy-blind-spots` is registered under
# `CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ENABLE_SANITIZER_ADDRESS AND
# FASTCACHED_ENABLE_TLS`. Its own registration block says, in as many words:
#
#     KNOWN RESIDUAL: if that job's flags change, this stops being registered and
#     reports nothing, which reads like a pass. The selftest below runs everywhere and
#     keeps the check itself honest, but nothing yet asserts that some CI job still
#     configures a build this can run in.
#
# That is the whole subject. A conditionally-registered check whose condition no job
# satisfies is ABSENT from every run, and absent reads exactly like passing: `ctest`
# prints a green total over a set that silently no longer contains it. Nothing about
# the output says a check went missing, and nothing can, because a count cannot
# describe a row that is not there.
#
# DERIVED, NEVER RESTATED. The population comes out of `src/tests/CMakeLists.txt` by
# walking its `if()` nesting; there is no list of check names in this file. A restated
# list is exact about the entries it knows and silent about the ones it does not, and
# silence reads identically to complete coverage -- so a list would catch a check going
# away and be blind to one ARRIVING, which is the direction that actually happens.
#
# WHAT COUNTS AS THE POPULATION, and why it is narrower than "conditionally registered".
# A registration whose false arm calls `fastcached_register_skipped_test` is already
# honest: the row exists either way and says which state it is in. Those are EXCLUDED,
# so this check measures exactly the registrations that VANISH. Platform conditions
# (`if(NOT WIN32)`) are excluded too -- a platform is not something a job configures,
# and "there is nothing here for this platform to answer" is a different claim with a
# different remedy.
#
# THREE ROUTES TO SATISFACTION, and the third is not optional. An option can be ON
# because a job passed `-DOPT=ON`, because a preset the job names sets it, or because
# its `option()` default is already ON and nobody turned it off. `FASTCACHED_BUILD_TUI`
# is the third kind: it appears in NO job and NO preset and is nonetheless on in every
# CI build, because it defaults ON. A check missing that route would report three
# vendor-tui rows as unreachable -- a loud, misattributed finding sending somebody to a
# defect that is not there, which is the same class of error as missing a real one and
# has the worse failure mode.
#
# AND THE JOB MUST RUN `ctest`. Configuring a build the check could be registered in is
# not enough: a job that configures and only BUILDS never runs it. That clause is what
# makes this a reachability check rather than a spelling check.
#
# The workflow is read through `scripts/lib/workflow-walk.awk`, which is this tree's one
# model of workflow YAML -- a private walk here would be refused by
# `ctest -R workflow-walk-sole`, and rightly: four of the five readers it replaced spelled
# `build.yml`'s indentation as literal column counts.
#
# Needs no compiler, no build, no daemon, no socket and no network.
set -o errexit
set -o nounset
set -o pipefail

Root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
Mode="scan"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test) Mode="selftest"; shift ;;
        --root)      Root="$2"; shift 2 ;;
        *) echo "usage: $0 [--root <dir>] | --self-test" >&2; exit 2 ;;
    esac
done

Failed=0
Fail() { echo "CONDITIONAL CHECK REACH FAILED: $*" >&2; Failed=$(( Failed + 1 )); }

# --- the population ---------------------------------------------------------
#
# Every `add_test` under an `if()` whose condition names a `FASTCACHED_*`/`ENABLE_*`
# option, minus the names that also have a `fastcached_register_skipped_test` arm.
# Prints `<test name><TAB><comma-separated option tokens>`.
DeriveRegistrations() {
    local file="$1"
    awk '
        function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }
        # Pass 1 is not separate: the skip-arm names are collected as we go and the
        # findings are filtered at END, so a helper call BELOW its add_test still counts.
        {
            line = trim($0)
            if (line ~ /^#/) next
            if (match(line, /fastcached_register_skipped_test\(["]/)) {
                rest = substr(line, RSTART + RLENGTH)
                q = index(rest, "\"")
                if (q > 0) skipped[substr(rest, 1, q - 1)] = 1
            }
            if (line ~ /^if\(/) {
                cond = substr(line, 4)
                sub(/\)[ \t]*$/, "", cond)
                depth++
                conds[depth] = cond
                next
            }
            if (line ~ /^elseif\(/) { if (depth > 0) { cond = substr(line, 8); sub(/\)[ \t]*$/, "", cond); conds[depth] = cond } next }
            # An `else()` arm is the NEGATION, and an option-gated negation is not a
            # build this check can ask CI to configure. Blanked rather than negated:
            # the population is "registered when the option is ON".
            if (line ~ /^else\(\)/) { if (depth > 0) conds[depth] = "" ; next }
            if (line ~ /^endif\(/)  { if (depth > 0) { conds[depth] = ""; depth-- } next }
            if (line ~ /^add_test[ \t]*\(/) { pending = 1; toks = "" ; for (d = 1; d <= depth; d++) toks = toks " " conds[d] }
            if (pending && match(line, /NAME[ \t]+"[^"]+"/)) {
                nm = substr(line, RSTART, RLENGTH)
                sub(/^NAME[ \t]+"/, "", nm); sub(/"$/, "", nm)
                out = ""
                n = split(toks, w, /[^A-Za-z0-9_]+/)
                # Deduplicated by the membership test on `out` alone. An earlier
                # spelling also carried `!(w[i] in seen_tok[nm])`, which was two
                # defects: `seen_tok` was never assigned, so the clause was always
                # true and did nothing -- and `arr[k]` used AS an array is a gawk
                # extension. macOS ships BWK awk as `awk`, where that is a syntax
                # error, so the check would not have run at all on the one platform
                # nobody here would have tested it on.
                for (i = 1; i <= n; i++)
                    if (w[i] ~ /^(FASTCACHED|ENABLE)_[A-Z0-9_]+$/) {
                        if (index("," out ",", "," w[i] ",") == 0) out = (out == "" ? w[i] : out "," w[i])
                    }
                # AND or OR. `vendor-sanitized` is registered under
                # `(ENABLE_SANITIZER_ADDRESS OR ENABLE_SANITIZER_THREAD) AND ...`, and
                # reading that as a conjunction demands one build be both an ASan and a
                # TSan build -- which nothing is, so the row would be refused as
                # unreachable while being perfectly reachable. That is an over-report:
                # loud, misattributed, and it sends somebody to a defect that is not
                # there. The first version of this check did exactly that.
                #
                # A condition mixing both at the OPTION-TOKEN level cannot be settled by
                # this reading, so it gets its own outcome rather than the nearer
                # neighbour -- `mixed`, which the scan refuses to classify instead of
                # guessing in whichever direction happens to be convenient.
                mode = "all"
                if (toks ~ /[^A-Za-z0-9_]OR[^A-Za-z0-9_]/) mode = "any"
                if (mode == "any" && toks ~ /[^A-Za-z0-9_]AND[^A-Za-z0-9_]/) {
                    # Only MIXED if the tokens are not all inside one OR group; a
                    # trailing `AND NOT MSVC` names no option and does not mix.
                    probe = toks
                    gsub(/\([^()]*\)/, "", probe)
                    if (probe ~ /(FASTCACHED|ENABLE)_[A-Z0-9_]+/) mode = "mixed"
                }
                if (out != "") { found[nm] = out; modeOf[nm] = mode }
                pending = 0
            }
        }
        END {
            for (nm in found) if (!(nm in skipped)) print nm "\t" found[nm] "\t" modeOf[nm]
        }
    ' "$file" | sort
}

# --- option defaults --------------------------------------------------------
#
# `option(NAME "desc" ON)` in the root lists. Prints `<NAME><TAB>ON|OFF`.
DeriveOptionDefaults() {
    local root="$1"
    awk '
        /^[ \t]*option[ \t]*\(/ {
            s = $0
            sub(/^[ \t]*option[ \t]*\(/, "", s)
            if (match(s, /^[A-Za-z0-9_]+/)) nm = substr(s, RSTART, RLENGTH); else next
            val = (s ~ /(^|[ \t)])ON[ \t]*\)?[ \t]*$/) ? "ON" : "OFF"
            print nm "\t" val
        }
    ' "${root}/CMakeLists.txt" | sort -u
}

# --- preset -> options ------------------------------------------------------
#
# A flat scan of `CMakePresets.json` rather than a JSON parse: a preset block runs from
# its `"name":` to the next one. `inherits` is resolved transitively by the caller.
# Prints `<preset><TAB><TOKEN>=<VALUE>` and `<preset><TAB>@inherits=<other>`.
DerivePresets() {
    awk '
        /"name"[ \t]*:/ {
            s = $0; sub(/.*"name"[ \t]*:[ \t]*"/, "", s); sub(/".*/, "", s); cur = s; next
        }
        cur == "" { next }
        /"inherits"[ \t]*:/ {
            s = $0; sub(/.*"inherits"[ \t]*:/, "", s)
            n = split(s, parts, /"/)
            for (i = 2; i <= n; i += 2) if (parts[i] != "") print cur "\t@inherits=" parts[i]
            next
        }
        /"(FASTCACHED|ENABLE)_[A-Z0-9_]+"[ \t]*:/ {
            s = $0
            if (!match(s, /"(FASTCACHED|ENABLE)_[A-Z0-9_]+"/)) next
            k = substr(s, RSTART + 1, RLENGTH - 2)
            v = s; sub(/.*:[ \t]*"?/, "", v); sub(/"?,?[ \t]*$/, "", v)
            print cur "\t" k "=" v
        }
    ' "$1"
}

# Does preset $2 (in file $1, table $3) set token $4 to ON, following `inherits`?
PresetSets() {
    local table="$1" preset="$2" token="$3" depth="${4:-0}" parent
    [ "$depth" -gt 8 ] && return 1
    if grep -Fxq "${preset}	${token}=ON" "$table"; then return 0; fi
    while IFS= read -r parent; do
        [ -n "$parent" ] || continue
        if PresetSets "$table" "$parent" "$token" $(( depth + 1 )); then return 0; fi
    done <<< "$(awk -F'\t' -v p="$preset" '$1 == p && $2 ~ /^@inherits=/ { sub(/^@inherits=/, "", $2); print $2 }' "$table")"
    return 1
}

# The lines of a job that are COMMANDS -- full-line `#` comments dropped.
#
# A COMMENT IS NOT A CALL SITE, and here it decided a verdict. `workflow-walk.awk`
# hands a `run: |` block's lines through verbatim, comments included, so
# `package-linux` -- which runs no `ctest` and never has -- was classified a
# ctest-running job by a comment reading "Assert the INSTALLED result: the ctest ...".
# The `-D` reading has the identical exposure: a commented-out flag would satisfy a
# row nothing configures.
#
# The direction is what makes it worth fixing rather than noting: this check fails
# OPEN on it. A registration reachable by no job would be reported REACHABLE because
# some job's prose mentions the word, which is the exact shape -- absent reading as
# passing -- the whole file exists to refuse.
#
# @param 1 the workflow record file  @param 2 the job key
JobCommands() {
    awk -F'\t' -v j="$2" '$1 == "TEXT" && $2 == j { print $3 }' "$1" \
        | sed -e 's/^[[:space:]]*#.*$//'
}

# Does this job's text INVOKE ctest, as opposed to mentioning it?
#
# Anchored at command position -- start of line, or after `;`, `&&`, `||`, `|` or `(`
# -- with `run:` allowed in front, because the walk renders a single-line step as
# `run: ctest --preset ...`. Stripping comments alone is not enough: the
# `check-clang-format` job carries `echo "::error::... It skips in ctest by design"`,
# which is a live line of shell and still not an invocation. That job satisfied three
# rows before this anchor existed.
#
# It is deliberately NARROW. A ctest invocation this cannot see is reported as a job
# that does not run ctest, which costs a row a satisfier and errs toward REFUSING --
# the direction a reader can act on. The broad reading errs toward a false green.
#
# @param 1 the job's command text
InvokesCtest() {
    grep -Eq '(^|[;&|(])[[:space:]]*(run:[[:space:]]*)?ctest([[:space:]]|$)' <<< "$1"
}

Scan() {
    local root="$1" scratch regs defaults presets wf jobs
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    regs="${scratch}/regs.tsv"
    defaults="${scratch}/defaults.tsv"
    presets="${scratch}/presets.tsv"
    wf="${scratch}/wf.tsv"

    DeriveRegistrations "${root}/src/tests/CMakeLists.txt" > "$regs"
    DeriveOptionDefaults "$root" > "$defaults"
    DerivePresets "${root}/CMakePresets.json" > "$presets"
    awk -f "${root}/scripts/lib/workflow-walk.awk" \
        -f "${root}/scripts/lib/conditional-check-reach.awk" \
        "${root}/.github/workflows/build.yml" > "$wf"

    # --- fail closed, four ways -------------------------------------------
    #
    # Each of these is a state in which the scan would otherwise report a clean run
    # over nothing. An empty derivation agrees with every claim.
    local nRegs nJobs nPresets nDefaults
    nRegs="$(grep -c . < "$regs" || true)"
    nJobs="$(grep -c '^JOB' < "$wf" || true)"
    nPresets="$(grep -c . < "$presets" || true)"
    nDefaults="$(grep -c . < "$defaults" || true)"
    if [ "${nRegs:-0}" -eq 0 ]; then
        Fail "no option-gated registration was derived from src/tests/CMakeLists.txt. Either every such registration is gone -- which would be news -- or the \`if()\` walk has stopped matching the file. An empty population passes every row it has, so this is a refusal, not a clean run."
    fi
    if [ "${nJobs:-0}" -eq 0 ]; then
        Fail "no job was derived from .github/workflows/build.yml. The workflow walk read nothing, so every row below would be unreachable for the same wrong reason."
    fi
    if [ "${nPresets:-0}" -eq 0 ]; then
        Fail "no preset variable was read from CMakePresets.json; the preset route to satisfaction is dead and rows would be refused that are actually fine."
    fi
    if [ "${nDefaults:-0}" -eq 0 ]; then
        Fail "no \`option()\` default was read from CMakeLists.txt; the default route is dead and rows would be refused that are actually fine."
    fi
    if grep -q '^REFUSAL' "$wf"; then
        Fail "the workflow walk refused a line it could not place: $(grep -m1 '^REFUSAL' "$wf")"
    fi
    [ "$Failed" -eq 0 ] || { rm -rf "$scratch"; trap - EXIT; return 1; }

    # Jobs that actually RUN ctest. Configuring a build a check could live in is not
    # the property; running it is.
    jobs="${scratch}/ctest-jobs"
    : > "$jobs"
    local job
    while IFS= read -r job; do
        # A HERESTRING, never `awk ... | grep -q`. `grep -q` exits at the first
        # match, the producer dies of SIGPIPE, and `set -o pipefail` then reports
        # the PRODUCER's status -- so the pipeline fails on the SUCCESS path, which
        # is the direction nobody looks. `check-e2e-helpers.sh`'s `early-exit-scan`
        # caught this one.
        if InvokesCtest "$(JobCommands "$wf" "$job")"; then
            echo "$job" >> "$jobs"
        fi
    done <<< "$(awk -F'\t' '$1 == "JOB" { print $2 }' "$wf")"

    local nCtest
    nCtest="$(grep -c . < "$jobs" || true)"
    if [ "${nCtest:-0}" -eq 0 ]; then
        Fail "no job in build.yml runs \`ctest\` at all. Every row would then be unreachable for one shared wrong reason rather than on its own merits."
        rm -rf "$scratch"; trap - EXIT; return 1
    fi
    echo "reach: ${nRegs} option-gated registration(s), ${nCtest} of ${nJobs} job(s) run ctest"

    # --- the rows ----------------------------------------------------------
    local name toks tok ok why satisfiedBy jobText d preset
    while IFS="	" read -r name toks mode; do
        [ -n "$name" ] || continue
        if [ "${mode:-all}" = "mixed" ]; then
            # The third outcome. A condition mixing AND and OR at the option-token
            # level is one this reading cannot settle, and answering it with the
            # nearest neighbour would be wrong in a direction nobody could see.
            Fail "${name} is gated on [${toks}] in a condition that mixes AND and OR at the option level, which this check cannot evaluate. It is NOT reported reachable or unreachable -- it is unclassified, which is its own outcome and not a pass. Either split the registration so each arm has one shape, or add the evaluation here deliberately."
            continue
        fi
        satisfiedBy=""
        while IFS= read -r job; do
            [ -n "$job" ] || continue
            jobText="$(JobCommands "$wf" "$job")"
            # `any` needs ONE token; `all` needs every one. The mode comes from the
            # registration's own condition, not from a guess here.
            if [ "${mode:-all}" = "any" ]; then ok=0; else ok=1; fi
            why=""
            for tok in $(echo "$toks" | tr ',' ' '); do
                if [ "${mode:-all}" = "any" ]; then
                    if grep -q -- "-D${tok}=ON" <<< "$jobText"; then ok=1; why="${tok} via -D in the job (any-of)"; break; fi
                    local anyPreset=0
                    while IFS= read -r preset; do
                        [ -n "$preset" ] || continue
                        if PresetSets "$presets" "$preset" "$tok"; then anyPreset=1; break; fi
                    done <<< "$(grep -o -- '--preset [A-Za-z0-9_-]*' <<< "$jobText" | awk '{print $2}' | sort -u)"
                    if [ "$anyPreset" = "1" ]; then ok=1; why="${tok} via preset ${preset} (any-of)"; break; fi
                    d="$(awk -F'\t' -v t="$tok" '$1 == t { print $2; exit }' "$defaults")"
                    if [ "${d:-}" = "ON" ] && ! grep -q -- "-D${tok}=OFF" <<< "$jobText"; then
                        ok=1; why="${tok} via its ON default (any-of)"; break
                    fi
                    continue
                fi
                # Route 1: the job names the flag.
                if grep -q -- "-D${tok}=ON" <<< "$jobText"; then
                    why="${why}${why:+, }${tok} via -D in the job"
                    continue
                fi
                if grep -q -- "-D${tok}=OFF" <<< "$jobText"; then ok=0; break; fi
                # Route 2: a preset the job names sets it.
                local viaPreset=0
                while IFS= read -r preset; do
                    [ -n "$preset" ] || continue
                    if PresetSets "$presets" "$preset" "$tok"; then viaPreset=1; break; fi
                done <<< "$(grep -o -- '--preset [A-Za-z0-9_-]*' <<< "$jobText" | awk '{print $2}' | sort -u)"
                if [ "$viaPreset" = "1" ]; then
                    why="${why}${why:+, }${tok} via preset ${preset}"
                    continue
                fi
                # Route 3: the option's own default. `FASTCACHED_BUILD_TUI` is here.
                d="$(awk -F'\t' -v t="$tok" '$1 == t { print $2; exit }' "$defaults")"
                if [ "${d:-}" = "ON" ]; then
                    why="${why}${why:+, }${tok} via its ON default"
                    continue
                fi
                ok=0
                break
            done
            if [ "$ok" = "1" ]; then satisfiedBy="$job"; break; fi
        done < "$jobs"

        if [ -n "$satisfiedBy" ]; then
            echo "ok: ${name} [${toks}] -- reachable in job '${satisfiedBy}' (${why})"
        else
            Fail "${name} is registered only when [${toks}] hold, and NO job in build.yml both configures that and runs ctest. It is therefore absent from every CI run, and absent reads exactly like passing. Remedy, in preference order: (1) make an existing ctest-running job configure it -- add the \`-D\` to that job's configure step; (2) if it genuinely cannot run in CI, give the registration an \`else()\` arm calling \`fastcached_register_skipped_test\` so the row exists and says why, which also removes it from this check's population; (3) if the check has been superseded, delete it rather than leaving it registered under a condition nothing meets. Do NOT satisfy this by loosening the registration's condition -- the condition describes what the check needs to be meaningful."
        fi
    done < "$regs"

    rm -rf "$scratch"
    trap - EXIT
    return 0
}

SelfTest() {
    local scratch ran=0 out rc
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    StageTree() {
        local d="$1" cond="$2" skipArm="$3" jobFlag="$4"
        mkdir -p "${d}/src/tests" "${d}/.github/workflows" "${d}/scripts/lib"
        cp "${Root}/scripts/lib/workflow-walk.awk" "${d}/scripts/lib/"
        cp "${Root}/scripts/lib/conditional-check-reach.awk" "${d}/scripts/lib/"
        printf 'option(FASTCACHED_THING "t" OFF)\noption(FASTCACHED_DEFAULTED "t" ON)\n' > "${d}/CMakeLists.txt"
        # Pretty-printed, one key per line, because that is what `CMakePresets.json`
        # is and what `DerivePresets` reads. A compact one-line fixture passes no
        # information to a line-oriented reader, and the first version of this
        # fixture wrote one -- five cases failed on a check that was fine, which is
        # the fixture testing itself rather than its subject.
        {
            printf '{\n  "configurePresets": [\n    {\n'
            printf '      "name": "p1",\n'
            printf '      "cacheVariables": {\n'
            printf '        "ENABLE_SANITIZER_ADDRESS": "ON"\n'
            printf '      }\n    }\n  ]\n}\n'
        } > "${d}/CMakePresets.json"
        {
            printf 'if(%s)\n    add_test(\n        NAME "staged-check"\n        COMMAND true\n    )\n' "$cond"
            if [ "$skipArm" = "skip" ]; then
                printf 'else()\n    fastcached_register_skipped_test("staged-check" "not here")\n'
            fi
            printf 'endif()\n'
        } > "${d}/src/tests/CMakeLists.txt"
        {
            printf 'jobs:\n  builder:\n    steps:\n      - name: configure\n        run: |\n'
            printf '          cmake --preset p1 %s\n' "$jobFlag"
            printf '      - name: test\n        run: ctest --preset p1\n'
        } > "${d}/.github/workflows/build.yml"
    }

    Run() { ( cd "$1" && bash "${Root}/scripts/check-conditional-check-reach.sh" --root "$1" ) 2>&1; }

    # 1. ACCEPTING arm first. A guard nobody has watched accept is not known to work,
    #    and one that refused every tree would pass every refusing case below.
    StageTree "${scratch}/a" "FASTCACHED_THING" "none" "-DFASTCACHED_THING=ON"
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/a")" && rc=0 || rc=$?
    if [ "${rc:-0}" -eq 0 ] && grep -q "ok: staged-check" <<< "$out"; then
        echo "self-test 1/8 ok: a row a job configures with -D is accepted"
    else
        Fail "self-test 1: expected acceptance via -D, got rc=${rc} <<${out}>>"
    fi

    # 2. The defect itself: nothing configures it.
    StageTree "${scratch}/b" "FASTCACHED_THING" "none" ""
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/b")" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "NO job in build.yml" <<< "$out"; then
        echo "self-test 2/8 ok: an option no job configures is refused"
    else
        Fail "self-test 2: expected refusal, got rc=${rc} <<${out}>>"
    fi

    # 3. The skip arm takes it out of the population -- the row is honest already.
    StageTree "${scratch}/c" "FASTCACHED_THING" "skip" ""
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/c")" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "no option-gated registration was derived" <<< "$out"; then
        echo "self-test 3/8 ok: a registration with a skip arm leaves the population (and an empty population is a refusal, not a pass)"
    else
        Fail "self-test 3: expected the empty-population refusal, got rc=${rc} <<${out}>>"
    fi

    # 4. The preset route.
    StageTree "${scratch}/d" "ENABLE_SANITIZER_ADDRESS" "none" ""
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/d")" && rc=0 || rc=$?
    if [ "${rc:-0}" -eq 0 ] && grep -q "via preset p1" <<< "$out"; then
        echo "self-test 4/8 ok: a token a named preset sets is accepted, and says so"
    else
        Fail "self-test 4: expected preset satisfaction, got rc=${rc} <<${out}>>"
    fi

    # 5. The DEFAULT route -- the one whose absence would invent three findings.
    StageTree "${scratch}/e" "FASTCACHED_DEFAULTED" "none" ""
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/e")" && rc=0 || rc=$?
    if [ "${rc:-0}" -eq 0 ] && grep -q "via its ON default" <<< "$out"; then
        echo "self-test 5/8 ok: an option that is ON by default needs no job to name it"
    else
        Fail "self-test 5: expected default satisfaction, got rc=${rc} <<${out}>>"
    fi

    # 6. Configuring is not running. The job builds and never calls ctest.
    StageTree "${scratch}/f" "FASTCACHED_THING" "none" "-DFASTCACHED_THING=ON"
    sed -i.bak 's/ctest --preset p1/cmake --build ./' "${scratch}/f/.github/workflows/build.yml"
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/f")" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "runs .ctest. at all" <<< "$out"; then
        echo "self-test 6/8 ok: a job that configures but never runs ctest does not satisfy a row"
    else
        Fail "self-test 6: expected the no-ctest refusal, got rc=${rc} <<${out}>>"
    fi

    # 7 and 8. MENTIONING ctest is not RUNNING it, and both shapes are live in
    # `build.yml` today: `package-linux` carries the word in a comment inside a
    # `run:` block and `check-clang-format` carries it inside an `echo`. Before the
    # anchor and the comment strip, each made its job count as a ctest job -- and
    # `check-clang-format` was then the sole satisfier of three vendor-tui rows,
    # a false GREEN in the one direction this whole file exists to refuse.
    #
    # Staged as a `run: |` block rather than by `sed`ing the one-line step, because
    # the defect is about a comment INSIDE such a block: a trailing comment on a
    # live line would test the anchor twice and the stripper not at all.
    StageTree "${scratch}/g" "FASTCACHED_THING" "none" "-DFASTCACHED_THING=ON"
    {
        printf 'jobs:\n  builder:\n    steps:\n      - name: configure\n        run: |\n'
        printf '          cmake --preset p1 -DFASTCACHED_THING=ON\n'
        printf '      - name: test\n        run: |\n'
        printf '          # we used to ctest --preset p1 here\n'
        printf '          cmake --build .\n'
    } > "${scratch}/g/.github/workflows/build.yml"
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/g")" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "runs .ctest. at all" <<< "$out"; then
        echo "self-test 7/8 ok: ctest named only in a comment does not make a job a ctest job"
    else
        Fail "self-test 7: expected the no-ctest refusal over a commented mention, got rc=${rc} <<${out}>>"
    fi

    StageTree "${scratch}/h" "FASTCACHED_THING" "none" "-DFASTCACHED_THING=ON"
    {
        printf 'jobs:\n  builder:\n    steps:\n      - name: configure\n        run: |\n'
        printf '          cmake --preset p1 -DFASTCACHED_THING=ON\n'
        printf '      - name: test\n        run: echo "that is not a pass -- it skips in ctest by design"\n'
    } > "${scratch}/h/.github/workflows/build.yml"
    ran=$(( ran + 1 ))
    out="$(Run "${scratch}/h")" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "runs .ctest. at all" <<< "$out"; then
        echo "self-test 8/8 ok: ctest named only inside an echo does not make a job a ctest job"
    else
        Fail "self-test 8: expected the no-ctest refusal over a mention inside an echo, got rc=${rc} <<${out}>>"
    fi

    rm -rf "$scratch"
    trap - EXIT
    echo "conditional-check-reach --self-test: ${ran} case(s) ran, ${Failed} failed"
    [ "$Failed" -eq 0 ]
}

if [ "$Mode" = "selftest" ]; then
    SelfTest
    exit $?
fi

Scan "$Root" || true
echo "conditional-check-reach: ${Failed} unreachable registration(s)"
[ "$Failed" -eq 0 ]
