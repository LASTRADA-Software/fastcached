#!/usr/bin/env bash
# Every `gh` listing goes through the shared reporter, or says why it does not.
#
# A `gh` listing that came back AT its `--limit` is a real answer about a set
# that is not the whole set -- a third state beside "found it" and "not there" --
# and nothing about the rows distinguishes it, so the cap reads as the total
# (#724). It has been measured wrong twice here, both times in the SAFE-LOOKING
# direction (smaller), which is why neither reading invited a second look.
#
# `scripts/ci-report-issue.sh` is the seam. It is the only caller that has all
# three properties, each of which is a mistake already made in this tree:
#
#   * a query that FAILED is not a query that found nothing;
#   * the SHAPE of the response is checked before a conclusion is drawn from it;
#   * the count is printed, so a reader can tell 147 from a cap of 147.
#
# Raising a `--limit` is not a fix: it moves the cliff and hides that there is
# one. A larger number is still a number a listing can reach.
#
# Classification is MANDATORY, not opt-in. An exemption list is exact about the
# sites it knows and silent about the ones it does not, and silence reads
# identically to complete coverage (#492) -- so an unlisted live listing is a
# REFUSAL, and each exemption carries its reason.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
selftest=0
[ "${1:-}" = "--self-test" ] && selftest=1

Seam="scripts/ci-report-issue.sh"

# One row per exempt site: an anchor substring, and why it is allowed.
#   * the SEAM itself is where the one guarded listing lives.
#   * `--paginate` exhausts the endpoint, so there is no cap to mistake.
is_exempt() {
    case "$1" in
        *--paginate*) return 0 ;;
    esac
    return 1
}

scanned_file=""
findings=""

scan_tree() {
    local tree="$1" f rel
    for f in "$tree"/scripts/*.sh "$tree"/.github/workflows/*.yml; do
        [ -f "$f" ] || continue
        rel="${f#"$tree"/}"
        [ "$rel" = "$Seam" ] && continue
        echo x >> "$scanned_file"
        # Comments stripped first: a COMMENT is not a call site, and checks in
        # this tree have twice matched their own explanatory headers.
        awk -v FNAME="$rel" '
            /gh-listing-scan: data-begin/ { inData = 1 }
            /gh-listing-scan: data-end/   { inData = 0; next }
            inData { next }
            { line = $0; sub(/^[ \t]*#.*$/, "", line) }
            line ~ /gh[ \t]+(issue|pr|run|project)[ \t]+(list|item-list)/ { print FNAME "|" FNR "|" line }
        ' "$f"
    done
}

run_scan() {
    local tree="$1" rc=0
    scanned_file="$(mktemp)"
    findings="$(scan_tree "$tree")"
    local n
    n="$(wc -l < "$scanned_file" | tr -d ' ')"
    rm -f "$scanned_file"
    if [ "$n" -eq 0 ]; then
        echo "REFUSED: the scan read no scripts or workflows under $tree -- an empty scan is not a pass"
        return 2
    fi
    # The seam must EXIST, or this check is guarding a rule nothing implements.
    if [ ! -f "$tree/$Seam" ]; then
        echo "REFUSED: $Seam is missing; the seam every other call site is required to use is not there"
        return 2
    fi
    while IFS='|' read -r file lineno body; do
        [ -z "${file:-}" ] && continue
        if is_exempt "$body"; then continue; fi
        echo "  UNGUARDED  $file:$lineno"
        rc=1
    done <<EOF
$findings
EOF
    echo "  scanned $n file(s)"
    return $rc
}

if [ "$selftest" -eq 1 ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    fail=0; cases=0
    mk() { mkdir -p "$tmp/$1/scripts" "$tmp/$1/.github/workflows"; : > "$tmp/$1/$Seam"; }

    mk clean
    printf 'echo hello\n' > "$tmp/clean/scripts/a.sh"
    cases=$((cases+1))
    if run_scan "$tmp/clean" >/dev/null 2>&1; then echo "  case 1 (no listings)        PASS"; else echo "  case 1 (no listings)        FAIL"; fail=1; fi

    mk bad
    # gh-listing-scan: data-begin -- fixture text below CONTAINS listings by
    # construction. A REGION, never a whole-file exemption: a file excused
    # entirely stops being scanned everywhere else in it too.
    printf 'x="$(gh issue list --state open --limit 500)"\n' > "$tmp/bad/scripts/a.sh"
    cases=$((cases+1))
    if run_scan "$tmp/bad" >/dev/null 2>&1; then echo "  case 2 (unguarded listing)  FAIL -- not caught"; fail=1; else echo "  case 2 (unguarded listing)  PASS"; fi

    mk paged
    printf 'x="$(gh issue list --paginate --state open)"\n' > "$tmp/paged/scripts/a.sh"
    cases=$((cases+1))
    if run_scan "$tmp/paged" >/dev/null 2>&1; then echo "  case 3 (--paginate exempt)  PASS"; else echo "  case 3 (--paginate exempt)  FAIL"; fail=1; fi

    mk cmt
    printf '# gh issue list --limit 500 would be unguarded here\necho ok\n' > "$tmp/cmt/scripts/a.sh"
    # gh-listing-scan: data-end
    cases=$((cases+1))
    if run_scan "$tmp/cmt" >/dev/null 2>&1; then echo "  case 4 (comment ignored)    PASS"; else echo "  case 4 (comment ignored)    FAIL"; fail=1; fi

    mkdir -p "$tmp/empty"
    cases=$((cases+1))
    run_scan "$tmp/empty" >/dev/null 2>&1
    [ $? -eq 2 ] && echo "  case 5 (empty tree refused) PASS" || { echo "  case 5 (empty tree refused) FAIL"; fail=1; }

    # The seam going missing must REFUSE, not pass: a check whose subject has
    # been deleted is guarding nothing, and silence there reads like a clean tree.
    mkdir -p "$tmp/noseam/scripts" "$tmp/noseam/.github/workflows"
    printf 'echo hello\n' > "$tmp/noseam/scripts/a.sh"
    cases=$((cases+1))
    run_scan "$tmp/noseam" >/dev/null 2>&1
    [ $? -eq 2 ] && echo "  case 6 (seam missing)       PASS" || { echo "  case 6 (seam missing)       FAIL"; fail=1; }

    echo "self-test: $cases cases run"
    [ "$fail" -eq 0 ] && echo "SELFTEST OK" || echo "SELFTEST FAILED"
    exit "$fail"
fi

echo "checking that every gh listing goes through $Seam"
run_scan "$root"
rc=$?
[ "$rc" -eq 2 ] && exit 1
if [ "$rc" -ne 0 ]; then
    echo "FAILED: a gh listing does not go through $Seam and does not --paginate."
    echo "        A listing that came back AT its --limit reads as the whole set."
    echo "        Route it through the reporter, add --paginate, or exempt it with a reason."
    exit 1
fi
echo "OK"
