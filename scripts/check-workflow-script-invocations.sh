#!/usr/bin/env bash
# A workflow's script invocation must agree with that script's file mode.
#
# A `run:` line naming `scripts/x.sh` either spells an interpreter (`bash x.sh`)
# or relies on the executable bit. When it relies on the bit and the file is 644,
# the step exits 126 -- Permission denied -- and that is a CI-ONLY failure: a local
# run never reproduces it, because a human types `bash` in front of the script by
# habit.
#
# It has happened twice (#721, #723). Once inside a `--self-test` re-invoking
# itself as a bare "$0", where eight `want-fail` cases passed because the SHELL
# refused rather than because the rule fired -- green, testing nothing. Once in
# `build.yml`, in a step whose `name:` is a REQUIRED context.
#
# The rule is AGREEMENT, not a spelling. `scripts/ci-scope.sh` is 755 and invoked
# bare, and that is correct and must keep passing; demanding `bash` everywhere
# would fail a tree that is right.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -euo pipefail

selfTest=0
for arg in ${1+"$@"}; do
    case "$arg" in
        --self-test) selfTest=1 ;;
        *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
    esac
done

# Where file modes come from.
#
# INJECTABLE, and the source is part of the OUTPUT so both sides can assert it.
# `git ls-files -s` needs a git repository and a synthetic tree is not one, so a
# self-test built on a plain directory would exercise a fallback CI never takes --
# the mode under test not being the mode in use, which .agent/rules/testing.md
# records against `check-catch-skip-return-code`.
#
# There is no filesystem fallback on purpose: the INDEX mode is what a fresh clone
# gets, and a local chmod that was never staged is not the fact this asks about.
# A tree with no index cannot answer, and says so.
ModeTable() {  # $1 = repo root -- prints "<mode> <path>" lines
    if [ -n "${FASTCACHED_WORKFLOW_MODE_FILE:-}" ]; then
        cat "$FASTCACHED_WORKFLOW_MODE_FILE"
        return 0
    fi
    git -C "$1" ls-files -s -- 'scripts/*.sh' 2>/dev/null | awk '{ print $1, $4 }'
}

ModeSourceName() {
    if [ -n "${FASTCACHED_WORKFLOW_MODE_FILE:-}" ]; then echo "file"; else echo "git"; fi
}

Check() {
    local root="$1" hits found=0 bare=0 bad=0 file prefix path mode

    echo "check-workflow-script-invocations: modes from $(ModeSourceName)"

    local -a workflows=()
    for file in "$root"/.github/workflows/*.yml; do
        [ -f "$file" ] && workflows[${#workflows[@]}]="$file"
    done
    if [ "${#workflows[@]}" -eq 0 ]; then
        echo "FAIL no workflow files under .github/workflows -- the scan is broken, not the tree"
        return 1
    fi

    # ONE awk pass over every workflow, not a shell pipeline per LINE. The
    # per-line version spawned several processes for each of ~3000 lines and timed
    # out at 120s on Windows, where a process spawn under Git Bash costs orders of
    # more than on Linux -- correct, and unusably slow on the only platform whose
    # failure mode this check exists to prevent.
    #
    # `-` for an absent word, never an empty field: `IFS=$'\t' read` does NOT read
    # TSV -- tab is IFS *whitespace*, so a leading empty field is COLLAPSED and
    # every field after it shifts left. That dropped the one bare invocation with
    # nothing before it, an indented `run: |` body, and the check read as clean.
    hits="$(awk '
        {
            line = $0
            sub(/^[ \t]+/, "", line)
            if (substr(line, 1, 1) == "#") next          # a comment is not a call site
            n = split($0, word, /[ \t]+/)
            prev = "-"
            for (i = 1; i <= n; i++) {
                if (word[i] == "") continue
                if (word[i] ~ /^scripts\/[A-Za-z0-9_.-]+\.sh$/)
                    printf "%s\t%s\t%s\n", FILENAME, prev, word[i]
                prev = word[i]
            }
        }' "${workflows[@]}")"

    # One `git ls-files -s` for every path, not one per hit: same spawn cost, same
    # platform.
    local modeTable
    modeTable="$(ModeTable "$root")"

    while IFS="$(printf '\t')" read -r file prefix path; do
        [ -n "${path:-}" ] || continue
        found=$((found + 1))
        case "$prefix" in
            bash | sh | */bash | */sh) continue ;;
        esac
        bare=$((bare + 1))
        mode="$(printf '%s\n' "$modeTable" | awk -v want="$path" '$2 == want { print $1; f = 1 } END { if (!f) print "absent" }')"
        if [ "$mode" = "absent" ]; then
            echo "FAIL ${file}: invokes ${path} bare, and that path is not in the index"
            bad=$((bad + 1))
        elif [ "$mode" != "100755" ]; then
            echo "FAIL ${file}: invokes ${path} bare, but it is ${mode} -- exit 126 on the runner"
            bad=$((bad + 1))
        fi
    done <<EOF
$hits
EOF

    if [ "$found" -eq 0 ]; then
        echo "FAIL no scripts/*.sh invocation found in any workflow -- the scan is broken, not the tree"
        return 1
    fi
    echo "check-workflow-script-invocations: ${found} invocation(s), ${bare} relying on the executable bit"
    [ "$bad" -eq 0 ]
}

if [ "$selfTest" -eq 0 ]; then
    Check "$(cd "$(dirname "$0")/.." && pwd)"
    echo "check-workflow-script-invocations: OK"
    exit 0
fi

cases=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

Stage() {  # $1 = name, $2 = run-line body, $3 = mode for scripts/x.sh
    mkdir -p "$tmp/$1/.github/workflows"
    printf 'jobs:\n  j:\n    steps:\n      - run: %s\n' "$2" > "$tmp/$1/.github/workflows/build.yml"
    printf '%s scripts/x.sh\n' "$3" > "$tmp/$1/modes"
}

Assert() {  # $1 = label, $2 = dir, $3 = pass|fail, $4 = expected mode source
    local out rc
    out=$(FASTCACHED_WORKFLOW_MODE_FILE="$tmp/$2/modes" Check "$tmp/$2" 2>&1) && rc=0 || rc=1
    cases=$((cases + 1))
    grep -q "modes from $4" <<< "$out" \
        || { echo "  FAIL: $1 -- expected mode source '$4'"; printf '%s\n' "$out"; exit 1; }
    if [ "$3" = pass ] && [ "$rc" -ne 0 ]; then echo "  FAIL: $1 -- expected a pass"; printf '%s\n' "$out"; exit 1; fi
    if [ "$3" = fail ] && [ "$rc" -eq 0 ]; then echo "  FAIL: $1 -- expected a refusal"; printf '%s\n' "$out"; exit 1; fi
    echo "  ok: $1"
}

Stage bare-755  'scripts/x.sh --self-test'      100755
Assert "a bare invocation of a 755 script passes -- the rule is agreement, not a spelling" bare-755 pass file

Stage bare-644  'scripts/x.sh --self-test'      100644
Assert "a bare invocation of a 644 script is refused" bare-644 fail file

Stage bash-644  'bash scripts/x.sh --self-test' 100644
Assert "an interpreted invocation of a 644 script passes" bash-644 pass file

Stage bash-755  'bash scripts/x.sh --self-test' 100755
Assert "an interpreted invocation of a 755 script passes" bash-755 pass file

# The shape that was silently skipped: a bare invocation with NO word before it,
# in an indented `run: |` body. The empty first field collapsed under
# `IFS=$'\t' read` -- tab is IFS whitespace -- and shifted the path into the
# prefix, leaving the path empty and the entry dropped. The check read as clean
# over the one invocation it most needed to see.
mkdir -p "$tmp/indented/.github/workflows"
printf 'jobs:\n  j:\n    steps:\n      - run: |\n          scripts/x.sh --self-test\n' \
    > "$tmp/indented/.github/workflows/build.yml"
printf '100644 scripts/x.sh\n' > "$tmp/indented/modes"
Assert "a bare invocation with no preceding word is still seen" indented fail file

# A comment is not a call site.
Stage commented '# scripts/x.sh is invoked elsewhere'  100644
mkdir -p "$tmp/commented2/.github/workflows"
printf 'jobs:\n  j:\n    steps:\n      - run: |\n          # scripts/x.sh -- a note, not a call\n          bash scripts/x.sh\n' \
    > "$tmp/commented2/.github/workflows/build.yml"
printf '100644 scripts/x.sh\n' > "$tmp/commented2/modes"
Assert "a commented mention is not a call site" commented2 pass file

# The scan must refuse rather than pass when it finds nothing.
mkdir -p "$tmp/no-workflows"
cases=$((cases + 1))
if FASTCACHED_WORKFLOW_MODE_FILE=/dev/null Check "$tmp/no-workflows" >/dev/null 2>&1; then
    echo "  FAIL: a tree with no workflows passed"; exit 1
else
    echo "  ok: a tree with no workflows is refused"
fi

mkdir -p "$tmp/no-hits/.github/workflows"
printf 'jobs:\n  j:\n    steps:\n      - run: echo hello\n' > "$tmp/no-hits/.github/workflows/build.yml"
cases=$((cases + 1))
if FASTCACHED_WORKFLOW_MODE_FILE=/dev/null Check "$tmp/no-hits" >/dev/null 2>&1; then
    echo "  FAIL: a tree with no script invocations passed"; exit 1
else
    echo "  ok: a tree with no script invocations is refused"
fi

echo "check-workflow-script-invocations self-test: $cases case(s) ran, all as expected"
