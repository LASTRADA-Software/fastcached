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
cases=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/good/scripts"
printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/good/scripts/a.sh"; chmod +x "$tmp/good/scripts/a.sh"
printf '# sourced library, no shebang\ntrue\n' > "$tmp/good/scripts/lib.sh"   # 644, must be ignored
cases=$((cases + 1))
if Check "$tmp/good" >/dev/null 2>&1; then echo "  ok: clean tree passes"
else echo "  FAIL: clean tree was refused"; exit 1; fi

mkdir -p "$tmp/bad/scripts"
printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/bad/scripts/a.sh"; chmod 644 "$tmp/bad/scripts/a.sh"
cases=$((cases + 1))
if Check "$tmp/bad" >/dev/null 2>&1; then echo "  FAIL: a shebanged 644 script was accepted"; exit 1
else echo "  ok: a shebanged 644 script is refused"; fi

# The empty-scan refusal: a directory with no .sh at all must FAIL, not pass.
mkdir -p "$tmp/empty/scripts"
cases=$((cases + 1))
if Check "$tmp/empty" >/dev/null 2>&1; then echo "  FAIL: an empty scan passed"; exit 1
else echo "  ok: an empty scan is refused"; fi

# A tree whose only .sh has no shebang: the probe cannot be vacuously satisfied.
mkdir -p "$tmp/noshebang/scripts"
printf 'true\n' > "$tmp/noshebang/scripts/lib.sh"
cases=$((cases + 1))
if Check "$tmp/noshebang" >/dev/null 2>&1; then echo "  FAIL: a tree with no shebang at all passed"; exit 1
else echo "  ok: a tree with no shebanged script is refused"; fi

echo "check-script-modes self-test: $cases case(s) ran, all as expected"
