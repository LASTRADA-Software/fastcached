#!/usr/bin/env bash
# Every tracked shell script carrying a shebang is mode 100755.
#
# #720: `scripts/local-gate.sh` was checked in 100644 while AGENT.md documents
# running it directly, so the gate this project tells every contributor to run
# refused on a fresh checkout. Thirteen siblings shared the mode and did not
# care, because ctest registers them as `COMMAND bash <path>` -- which is why the
# one script with no automated caller was the one that bit.
#
# Derived from the SHEBANG, never from a list of files. A list is exact about the
# files it knows and silent about the ones it does not, and silence reads
# identically to complete coverage (#492). A sourced library legitimately has
# neither a shebang nor the bit (`scripts/lib/e2e-common.sh`), and falls out of
# the rule rather than needing an exemption.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
# No mapfile, no `declare -A`, no `${var^^}`.
set -euo pipefail

selfTest=0
for arg in ${1+"$@"}; do
    case "$arg" in
        --self-test) selfTest=1 ;;
        *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
    esac
done

# Enumerate. The MODE is part of the output and is asserted, because a synthetic
# tree is not a git repository: a self-test that silently exercised the fallback
# while CI exercised git would be a guard testing something else (#499's shape).
Enumerate() {
    local root="$1"
    if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "git"
        git -C "$root" ls-files -s -- '*.sh' | awk '{print $1 "\t" $4}'
    else
        echo "walk"
        find "$root" -name '*.sh' -type f | while read -r f; do
            if [ -x "$f" ]; then printf '100755\t%s\n' "${f#"$root"/}"
            else printf '100644\t%s\n' "${f#"$root"/}"; fi
        done
    fi
}

Check() {
    local root="$1" out mode total=0 shebanged=0 bad=0 rc=0
    out=$(Enumerate "$root")
    mode=$(printf '%s\n' "$out" | head -1)
    echo "check-script-modes: enumerated via $mode"

    local line filemode path first
    while IFS="$(printf '\t')" read -r filemode path; do
        [ -n "${path:-}" ] || continue
        total=$((total + 1))
        first=$(head -1 "$root/$path" 2>/dev/null || true)
        case "$first" in
            '#!'*) ;;
            *) continue ;;
        esac
        shebanged=$((shebanged + 1))
        if [ "$filemode" != "100755" ]; then
            echo "FAIL $path is $filemode with a shebang; it must be 100755"
            bad=$((bad + 1))
        fi
    done <<EOF
$(printf '%s\n' "$out" | tail -n +2)
EOF

    # An empty scan is a refusal, never a pass: two empty lists agree perfectly.
    if [ "$total" -eq 0 ]; then
        echo "FAIL check-script-modes matched no .sh files at all -- the scan is broken, not the tree"
        return 1
    fi
    if [ "$shebanged" -eq 0 ]; then
        echo "FAIL $total .sh file(s) found and none carries a shebang -- the shebang probe is broken"
        return 1
    fi
    echo "check-script-modes: $shebanged of $total tracked .sh file(s) carry a shebang"
    [ "$bad" -eq 0 ] || rc=1
    return $rc
}

if [ "$selfTest" -eq 0 ]; then
    Check "$(cd "$(dirname "$0")/.." && pwd)"
    echo "check-script-modes: OK"
    exit 0
fi

# --- self-test: both directions, or a check that always passes looks the same ---
#
# The fixtures are REAL git repositories, and each case asserts which enumeration
# it exercised. A synthetic directory is not a git repo, so a self-test built on
# one would drive the `walk` fallback while CI drives `git` -- the mode under
# test would not be the mode in use, which is a guard passing because it is
# testing something else (#499's shape).
#
# It is also the only portable way to stage the negative: on Windows `chmod 644`
# does not stick and the staged violation reads as executable, so the case that
# must FAIL passes. Git index modes are the same on every platform.
cases=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

MakeRepo() {  # $1 = dir, $2 = mode for scripts/a.sh (+x or -x)
    mkdir -p "$1/scripts"
    git -C "$1" init -q
    printf '#!/usr/bin/env bash\ntrue\n' > "$1/scripts/a.sh"
    printf 'true\n' > "$1/scripts/lib.sh"   # sourced library: no shebang, no bit
    git -C "$1" add -A >/dev/null 2>&1
    git -C "$1" update-index "--chmod=$2" scripts/a.sh
}

Assert() {  # $1 = label, $2 = dir, $3 = expect pass|fail, $4 = expected mode
    local out rc
    out=$(Check "$2" 2>&1) && rc=0 || rc=1
    cases=$((cases + 1))
    case "$4" in
        ?*) printf '%s\n' "$out" | grep -q "enumerated via $4" \
                || { echo "  FAIL: $1 -- expected enumeration '$4', got: $(printf '%s' "$out" | head -1)"; exit 1; } ;;
    esac
    if [ "$3" = pass ] && [ "$rc" -ne 0 ]; then echo "  FAIL: $1 -- expected a pass"; printf '%s\n' "$out"; exit 1; fi
    if [ "$3" = fail ] && [ "$rc" -eq 0 ]; then echo "  FAIL: $1 -- expected a refusal"; printf '%s\n' "$out"; exit 1; fi
    echo "  ok: $1"
}

MakeRepo "$tmp/good" "+x"
Assert "a shebanged script with the bit passes, and a bitless library is ignored" "$tmp/good" pass git

MakeRepo "$tmp/bad" "-x"
Assert "a shebanged script without the bit is refused" "$tmp/bad" fail git

# Two empty lists agree perfectly, so both of these must REFUSE rather than pass.
mkdir -p "$tmp/empty"; git -C "$tmp/empty" init -q
Assert "a tree with no .sh at all is refused" "$tmp/empty" fail git

mkdir -p "$tmp/noshebang/scripts"; git -C "$tmp/noshebang" init -q
printf 'true\n' > "$tmp/noshebang/scripts/lib.sh"
git -C "$tmp/noshebang" add -A >/dev/null 2>&1
Assert "a tree whose only .sh carries no shebang is refused" "$tmp/noshebang" fail git

# And the fallback is REACHED for a non-git tree. Asserted on the enumeration it
# chose, not on a verdict: the walk reads the filesystem bit, which Windows does
# not carry, so a mode-dependent expectation here would be the #499 trap again.
mkdir -p "$tmp/nogit/scripts"
printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/nogit/scripts/a.sh"
out=$(Check "$tmp/nogit" 2>&1 || true)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q "enumerated via walk" \
    && echo "  ok: a non-git tree falls back to the walk" \
    || { echo "  FAIL: a non-git tree did not use the walk: $(printf '%s' "$out" | head -1)"; exit 1; }

echo "check-script-modes self-test: $cases case(s) ran, all as expected"
