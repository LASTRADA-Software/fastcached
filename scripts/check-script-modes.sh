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

# Which files are third-party, asked of the tree being checked (#1370): an upstream
# script's mode is upstream's decision, and a vendored copy stays byte-for-byte theirs.
# shellcheck source=lib/third-party-roots.sh
. "$(dirname "$0")/lib/third-party-roots.sh"

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
    local root="$1" out mode total=0 shebanged=0 bad=0 rc=0 declined nl=$'\n'
    out=$(Enumerate "$root")
    # NOT `printf ... | head -1`. `head` leaves after its line, `printf` takes
    # SIGPIPE, `pipefail` reports PRINTF's status and the `set -e` above aborts
    # the whole script -- measured as a deterministic exit 141 with no output at
    # all under MSYS2 bash, on every Windows developer's machine (#1111). Pure
    # bash here: no pipe, no fork, nothing to lose.
    mode=${out%%$'\n'*}
    echo "check-script-modes: enumerated via $mode"

    # Declined by path, once, before the loop -- a membership test per file below rather
    # than a process per file. A roots file that cannot be read is a refusal.
    if ! declined="$(third_party_paths "$root" "$(printf '%s\n' "$out" | tail -n +2 | cut -f2)")"; then
        echo "FAIL check-script-modes: the third-party roots of $root could not be read (the reader says why above), so a vendored script cannot be told from a first-party one"
        return 1
    fi
    [ -z "$declined" ] \
        || echo "check-script-modes: $(third_party_declined_summary 'shell script(s)' "$declined")"

    local line filemode path first
    while IFS="$(printf '\t')" read -r filemode path; do
        [ -n "${path:-}" ] || continue
        case "$nl$declined$nl" in
            *"$nl$path$nl"*) continue ;;
        esac
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

# Every fixture states its third-party roots (#1370): one that did not would be REFUSED,
# and the cases expecting a refusal would then pass for the wrong reason.
PlantRoots() {
    mkdir -p "$1/scripts/lib"
    printf '# planted\nvendor/upstream\n' > "$1/scripts/lib/third-party-roots.txt"
}

MakeRepo() {  # $1 = dir, $2 = mode for scripts/a.sh (+x or -x)
    mkdir -p "$1/scripts"
    PlantRoots "$1"
    git -C "$1" init -q
    printf '#!/usr/bin/env bash\ntrue\n' > "$1/scripts/a.sh"
    printf 'true\n' > "$1/scripts/lib.sh"   # sourced library: no shebang, no bit
    git -C "$1" add -A >/dev/null 2>&1
    git -C "$1" update-index "--chmod=$2" scripts/a.sh
}

Assert() {  # $1 = label, $2 = dir, $3 = expect pass|fail, $4 = expected mode, $5 = text the output must carry
    local out rc
    out=$(Check "$2" 2>&1) && rc=0 || rc=1
    cases=$((cases + 1))
    case "$4" in
        ?*) grep -q "enumerated via $4" <<< "$out" \
                || { echo "  FAIL: $1 -- expected enumeration '$4', got: ${out%%$'\n'*}"; exit 1; } ;;
    esac
    case "${5:-}" in
        ?*) grep -qF -- "$5" <<< "$out" \
                || { echo "  FAIL: $1 -- the output does not carry '$5'"; printf '%s\n' "$out"; exit 1; } ;;
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
mkdir -p "$tmp/empty"; PlantRoots "$tmp/empty"; git -C "$tmp/empty" init -q
Assert "a tree with no .sh at all is refused" "$tmp/empty" fail git

mkdir -p "$tmp/noshebang/scripts"; PlantRoots "$tmp/noshebang"; git -C "$tmp/noshebang" init -q
printf 'true\n' > "$tmp/noshebang/scripts/lib.sh"
git -C "$tmp/noshebang" add -A >/dev/null 2>&1
Assert "a tree whose only .sh carries no shebang is refused" "$tmp/noshebang" fail git

# And the fallback is REACHED for a non-git tree. Asserted on the enumeration it
# chose, not on a verdict: the walk reads the filesystem bit, which Windows does
# not carry, so a mode-dependent expectation here would be the #499 trap again.
mkdir -p "$tmp/nogit/scripts"
PlantRoots "$tmp/nogit"
printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/nogit/scripts/a.sh"
out=$(Check "$tmp/nogit" 2>&1 || true)
cases=$((cases + 1))
grep -q "enumerated via walk" <<< "$out" \
    && echo "  ok: a non-git tree falls back to the walk" \
    || { echo "  FAIL: a non-git tree did not use the walk: ${out%%$'\n'*}"; exit 1; }

# A third-party script whose mode would be refused is DECLINED, by name (#1370). The
# planted one carries a shebang and no bit, so a check that failed to decline it refuses
# the tree: passing is the proof, and the name is what says why.
MakeRepo "$tmp/vendored" "+x"
mkdir -p "$tmp/vendored/vendor/upstream"
printf '#!/usr/bin/env bash\ntrue\n' > "$tmp/vendored/vendor/upstream/tool.sh"
git -C "$tmp/vendored" add vendor/upstream/tool.sh >/dev/null 2>&1
git -C "$tmp/vendored" update-index --chmod=-x vendor/upstream/tool.sh
Assert "a third-party script without the bit is declined, and named" "$tmp/vendored" pass git \
    "declined 1 third-party shell script(s) under the roots in scripts/lib/third-party-roots.txt, first vendor/upstream/tool.sh"

# A roots file naming no root is a REFUSAL, never "nothing is third-party".
MakeRepo "$tmp/noroots" "+x"
printf '# no root at all\n' > "$tmp/noroots/scripts/lib/third-party-roots.txt"
Assert "a roots file naming no root is refused" "$tmp/noroots" fail git "could not be read"

echo "check-script-modes self-test: $cases case(s) ran, all as expected"
