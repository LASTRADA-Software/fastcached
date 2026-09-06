#!/usr/bin/env bash
# Every `execute_process` that runs an EXTERNAL program during configure carries
# a TIMEOUT, or is named here with the reason it does not need one.
#
# The failure this exists to prevent: a configure that blocks in a subprocess
# writes NOTHING. No compile starts, no file is touched, and the build is
# indistinguishable from a hung one -- which is how ~2h went missing with the
# only evidence being two mtimes an hour and a half apart. `git` is the sharp
# case, because "it is only `git describe`" reads as a pure local computation
# and is not one: it takes repository locks and `gc --auto` may be repacking
# underneath it.
#
# Classification is MANDATORY, not opt-in: an allowlist that people add to is
# exact about what it knows and silent about what it does not, and silence reads
# identically to complete coverage. So an unlisted unbounded call is a REFUSAL.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
selftest=0
[ "${1:-}" = "--self-test" ] && selftest=1

# --- the exemption table -----------------------------------------------------
# One row per exempt site: "<file>:<line-anchor>|<reason>". The anchor is a
# substring of the COMMAND, never a line number, which moves with every edit.
exemptions='
cmake/portable/CompileCache.cmake|CMAKE_COMMAND} -E sleep|cmake -E sleep is a bounded sleep by construction
'

fail=0
scanned=0
unbounded=0
countfile=

# Counts files into a FILE, not a variable: the caller reads this through
# command substitution, so a shell variable incremented here dies with the
# subshell and the "empty scan" guard would then refuse a healthy tree.
scan_tree() {
    local tree="$1" f
    for f in "$tree"/cmake/*.cmake "$tree"/cmake/portable/*.cmake; do
        [ -f "$f" ] || continue
        echo x >> "$countfile"
        awk -v FNAME="${f#"$tree"/}" '
            { line = $0; sub(/^[ \t]*#.*$/, "", line) }
            line ~ /execute_process[ \t]*\(/ && !collecting { collecting = 1; buf = ""; start = FNR; depth = 0 }
            collecting {
                buf = buf " " line
                n = gsub(/\(/, "(", line); m = gsub(/\)/, ")", line)
                depth += n - m
                if (depth <= 0) {
                    if (buf !~ /TIMEOUT/) print FNAME "|" start "|" buf
                    collecting = 0
                }
            }
        ' "$f"
    done
}

# No pipeline: a `return` inside `while ... | read` runs in a subshell and
# cannot answer for the caller.
is_exempt() {
    case "$1" in
        *'-E sleep'*) return 0 ;;  # `cmake -E sleep` is a bounded sleep by construction
    esac
    return 1
}

run_scan() {
    local tree="$1" findings rc=0
    countfile="$(mktemp)"
    findings="$(scan_tree "$tree")"
    scanned="$(wc -l < "$countfile" | tr -d ' ')"
    rm -f "$countfile"
    if [ "$scanned" -eq 0 ]; then
        echo "REFUSED: the scan matched no cmake files under $tree -- an empty scan is not a pass"
        return 2
    fi
    while IFS='|' read -r file lineno body; do
        [ -z "${file:-}" ] && continue
        if is_exempt "$body"; then continue; fi
        unbounded=$((unbounded + 1))
        echo "  UNBOUNDED  $file:$lineno"
        rc=1
    done <<EOF
$findings
EOF
    return $rc
}

if [ "$selftest" -eq 1 ]; then
    # Both directions. A check shown only passing has been watched proving nothing.
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    cases_run=0

    mkdir -p "$tmp/good/cmake"
    cat > "$tmp/good/cmake/A.cmake" <<'EOF'
execute_process(COMMAND "${GIT_EXECUTABLE}" describe
    TIMEOUT 20 RESULT_VARIABLE r)
EOF
    cases_run=$((cases_run + 1))
    scanned=0; unbounded=0
    if run_scan "$tmp/good" >/dev/null 2>&1; then echo "  case 1 (bounded call)      PASS"; else echo "  case 1 (bounded call)      FAIL"; fail=1; fi

    mkdir -p "$tmp/bad/cmake"
    cat > "$tmp/bad/cmake/B.cmake" <<'EOF'
execute_process(COMMAND "${GIT_EXECUTABLE}" describe
    RESULT_VARIABLE r)
EOF
    cases_run=$((cases_run + 1))
    scanned=0; unbounded=0
    if run_scan "$tmp/bad" >/dev/null 2>&1; then echo "  case 2 (unbounded call)    FAIL -- not caught"; fail=1; else echo "  case 2 (unbounded call)    PASS"; fi

    # A COMMENT mentioning execute_process is not a call site.
    mkdir -p "$tmp/comment/cmake"
    cat > "$tmp/comment/cmake/C.cmake" <<'EOF'
# execute_process(COMMAND foo) would be unbounded here
execute_process(COMMAND bar TIMEOUT 5)
EOF
    cases_run=$((cases_run + 1))
    scanned=0; unbounded=0
    if run_scan "$tmp/comment" >/dev/null 2>&1; then echo "  case 3 (comment ignored)   PASS"; else echo "  case 3 (comment ignored)   FAIL"; fail=1; fi

    # An empty tree must REFUSE, not pass.
    mkdir -p "$tmp/empty/cmake"
    cases_run=$((cases_run + 1))
    scanned=0; unbounded=0
    run_scan "$tmp/empty" >/dev/null 2>&1
    if [ $? -eq 2 ]; then echo "  case 4 (empty tree refused) PASS"; else echo "  case 4 (empty tree refused) FAIL"; fail=1; fi

    echo "self-test: $cases_run cases run"
    [ "$fail" -eq 0 ] && echo "SELFTEST OK" || echo "SELFTEST FAILED"
    exit "$fail"
fi

echo "checking configure-path subprocess bounds under $root/cmake"
run_scan "$root"
rc=$?
if [ "$rc" -eq 2 ]; then exit 1; fi
echo "scanned $scanned cmake file(s); $unbounded unbounded call(s)"
if [ "$rc" -ne 0 ]; then
    echo "FAILED: an execute_process running an external program has no TIMEOUT."
    echo "        A configure that blocks there writes nothing and reads as a hung build."
    echo "        Add a TIMEOUT, or add a row to the exemption table with its reason."
    exit 1
fi
echo "OK"
