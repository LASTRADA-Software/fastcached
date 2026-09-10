#!/usr/bin/env bash
# Every cache-flow fall-back names which continuation it takes, and why.
#
# `RunCached` ends through one of two spellings and the choice is a JUDGEMENT the
# call site makes (#910):
#
#   Warn(reason)            -> std::nullopt -> RunPassthrough -> NO STORE
#   WarnAndCarryOn(reason)  -> the MISS path -> the STORE REPAIRS the entry
#
# The question both answer is "does the cache now hold a value that must be
# REPLACED?". Getting it backwards once cost a permanently dead cache: a
# `CompileValueVersion` bump does not move the key, so a foreign-generation value
# stays under the key the new build computes -- fetched, refused, and under `Warn`
# never overwritten. After a bump that is every key in the cache.
#
# Nothing else can catch it. Both spellings type-check, so the compiler cannot;
# `main.cpp` is in no test target (#909), so the suite cannot; and the symptom is
# a slow build rather than a failure.
#
# So the guard is CLASSIFICATION, and it is mandatory rather than opt-in: a call
# site not in the table below is a REFUSAL. An opt-in list is exact about the
# sites it knows and silent about the ones it does not, and silence reads
# identically to complete coverage (#492).
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
selftest=0
[ "${1:-}" = "--self-test" ] && selftest=1

Subject="src/apps/fastcache-cc/main.cpp"

# One row per classified call site: <reason-substring>|<expected spelling>|<why>.
# The reason string is the anchor because it is what the operator sees and what
# `--show-stats` ranks; a line number moves with every edit.
#
# cache-flow-scan: data-begin
Table='
missing FASTCACHE_ADDR|Warn|cache not configured: nothing was reached, so nothing to replace
preprocess failed|Warn|no preprocessed text means no key, so nothing to replace
could not write object on hit|Warn|the STORED entry is good; the local write failed
DescribeOutcome|WarnAndCarryOn|the daemon refused: carry on so a MISS can store
fetch exchange failed|WarnAndCarryOn|unreached: carry on so a MISS can store
DecodeFailureReason|WarnAndCarryOn|an UNUSABLE value sits under this key and must be overwritten
'
# cache-flow-scan: data-end

scan() {
    # $1 = tree. Prints "<spelling>|<line>|<reason>" per call site, comments stripped.
    awk '
        /cache-flow-scan: data-begin/ { inData = 1 }
        /cache-flow-scan: data-end/   { inData = 0; next }
        inData { next }
        { line = $0; sub(/^[ \t]*\/\/.*$/, "", line) }
        # A DEFINITION is not a call site -- the same trap as a comment, one step
        # along: its signature names the parameter type, which no call site does.
        #
        # Matched ANYWHERE on the line, not immediately after the `(`. The anchored
        # form assumed the reason was the FIRST parameter, which held until #60 gave
        # both helpers an `InvocationRecord&` ahead of it -- and then the definitions
        # read as call sites, went UNCLASSIFIED, and reddened a correct tree. The
        # rationale is unchanged; what was wrong was pinning it to a position.
        line ~ /std::string_view/ { next }
        line ~ /(^|[^A-Za-z_])Warn[ \t]*\(/          { print "Warn|" FNR "|" line; next }
        line ~ /(^|[^A-Za-z_])WarnAndCarryOn[ \t]*\(/ { print "WarnAndCarryOn|" FNR "|" line }
    ' "$1/$Subject" 2>/dev/null
}

classify() {
    # $1 = the call-site line. Echoes "<expected>|<why>" or nothing.
    printf '%s\n' "$Table" | while IFS='|' read -r anchor expected why; do
        [ -z "${anchor:-}" ] && continue
        case "$1" in *"$anchor"*) printf '%s|%s\n' "$expected" "$why"; return ;; esac
    done
}

run_scan() {
    local tree="$1" rc=0 seen=0 found
    if [ ! -f "$tree/$Subject" ]; then
        echo "REFUSED: $Subject is missing; this check has no subject and cannot pass"
        return 2
    fi
    found="$(scan "$tree")"
    while IFS='|' read -r spelling lineno body; do
        [ -z "${spelling:-}" ] && continue
        seen=$((seen + 1))
        local verdict expected
        verdict="$(classify "$body")"
        if [ -z "$verdict" ]; then
            echo "  UNCLASSIFIED  $Subject:$lineno  $spelling(...)"
            echo "                add a row saying whether the cache now holds a value that must be REPLACED"
            rc=1
            continue
        fi
        expected="${verdict%%|*}"
        if [ "$spelling" != "$expected" ]; then
            echo "  WRONG CONTINUATION  $Subject:$lineno  uses $spelling, table says $expected"
            echo "                      because: ${verdict#*|}"
            rc=1
        fi
    done <<EOF
$found
EOF
    if [ "$seen" -eq 0 ]; then
        echo "REFUSED: found no Warn/WarnAndCarryOn call site at all -- the helpers have been"
        echo "         renamed or the scan has stopped seeing them, and it is now guarding nothing"
        return 2
    fi
    if [ "$rc" -eq 0 ]; then
        echo "  $seen call site(s), all classified"
    else
        echo "  $seen call site(s) scanned; see the findings above"
    fi
    return $rc
}

if [ "$selftest" -eq 1 ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    fail=0; cases=0
    stage() { mkdir -p "$tmp/$1/$(dirname "$Subject")"; cat > "$tmp/$1/$Subject"; }

    stage good <<'SRC'
return Warn("missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR");
return Warn("preprocess failed");
return Warn("could not write object on hit");
WarnAndCarryOn(Cc::DescribeOutcome(outcome));
WarnAndCarryOn("fetch exchange failed");
WarnAndCarryOn(DecodeFailureReason(decoded.error()));
SRC
    cases=$((cases+1))
    if run_scan "$tmp/good" >/dev/null 2>&1; then echo "  case 1 (all classified)        PASS"; else echo "  case 1 (all classified)        FAIL"; fail=1; fi

    stage newsite <<'SRC'
return Warn("missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR");
return Warn("a brand new outcome nobody classified");
SRC
    cases=$((cases+1))
    if run_scan "$tmp/newsite" >/dev/null 2>&1; then echo "  case 2 (new site refused)      FAIL -- not caught"; fail=1; else echo "  case 2 (new site refused)      PASS"; fi

    # The defect itself: the undecodable arm choosing no-STORE.
    stage backwards <<'SRC'
return Warn("missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR");
return Warn(DecodeFailureReason(decoded.error()));
SRC
    cases=$((cases+1))
    if run_scan "$tmp/backwards" >/dev/null 2>&1; then echo "  case 3 (wrong continuation)    FAIL -- not caught"; fail=1; else echo "  case 3 (wrong continuation)    PASS"; fi

    stage comment <<'SRC'
// return Warn("a brand new outcome nobody classified");
return Warn("preprocess failed");
SRC
    cases=$((cases+1))
    if run_scan "$tmp/comment" >/dev/null 2>&1; then echo "  case 4 (comment ignored)       PASS"; else echo "  case 4 (comment ignored)       FAIL"; fail=1; fi

    # The DEFINITIONS, which are not call sites. Six cases stood here without one,
    # so the exclusion had never been watched accepting anything -- and when #60
    # added a parameter ahead of the reason it broke in exactly the silent
    # direction, reporting two findings against a correct tree. Both helpers are
    # staged with a real call site, so a rule that stopped excluding definitions
    # fails this case rather than the repository.
    stage definitions <<'SRC'
[[nodiscard]] std::optional<int> Warn(InvocationRecord& record, std::string_view reason)
{
    RecordFallback(record, Fallback::Unavailable, reason);
    return std::nullopt;
}
void WarnAndCarryOn(InvocationRecord& record, std::string_view reason)
{
    RecordFallback(record, Fallback::UnavailableCarryOn, reason);
}
return Warn(record, "preprocess failed");
SRC
    cases=$((cases+1))
    if run_scan "$tmp/definitions" >/dev/null 2>&1; then echo "  case 7 (definitions ignored)   PASS"; else echo "  case 7 (definitions ignored)   FAIL"; fail=1; fi

    # Helpers renamed away: the scan matches nothing and must REFUSE, not pass.
    stage renamed <<'SRC'
return Bail("preprocess failed");
SRC
    cases=$((cases+1))
    run_scan "$tmp/renamed" >/dev/null 2>&1
    [ $? -eq 2 ] && echo "  case 5 (no sites -> refuse)    PASS" || { echo "  case 5 (no sites -> refuse)    FAIL"; fail=1; }

    mkdir -p "$tmp/nosubject"
    cases=$((cases+1))
    run_scan "$tmp/nosubject" >/dev/null 2>&1
    [ $? -eq 2 ] && echo "  case 6 (subject missing)       PASS" || { echo "  case 6 (subject missing)       FAIL"; fail=1; }

    echo "self-test: $cases cases run"
    [ "$fail" -eq 0 ] && echo "SELFTEST OK" || echo "SELFTEST FAILED"
    exit "$fail"
fi

echo "checking cache-flow continuations in $Subject"
run_scan "$root"
rc=$?
[ "$rc" -eq 2 ] && exit 1
if [ "$rc" -ne 0 ]; then
    echo "FAILED: a cache-flow fall-back is unclassified or takes the wrong continuation."
    echo "        Ask: does the cache now hold a value that must be REPLACED?"
    echo "          yes -> WarnAndCarryOn, so the MISS path's STORE overwrites it"
    echo "          no  -> Warn"
    exit 1
fi
echo "OK"
