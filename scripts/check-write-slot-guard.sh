#!/usr/bin/env bash
# Every socket that PARKS a write claims the write slot; every one that cannot says so.
#
# `Net/ReadSlot.hpp` states the rule for reads: a socket has ONE operation per
# direction, so arming over a parked one drops that coroutine -- never resumed,
# never freed, no assertion, no error, no log (#663). The write side is the same
# machine with none of the machinery (#893): `TlsSocket::CancelRead` retires a pump
# parked on the READ slot, and a pump suspended in `FlushOutgoing`'s `_raw->Write`
# is not retired at all, so the caller's next `write()` arms a SECOND pump over the
# same raw write slot.
#
# `Detail::ClaimWriteSlot` folds the claim and the assertion into one expression, so
# an arm site cannot omit it without omitting the clear. This checks that every arm
# site actually goes through it -- the half that rots when a seventh transport is
# added, and the half a canary cannot cover: a canary aborts at the FIRST violation,
# so it watches one site whatever it picks and is silent about the rest (#492).
#
# It DERIVES the transports rather than tabulating them. A hand-kept list is the
# same defect one level up: a seventh transport joins no list and passes.
#
# Two states per transport, and BOTH must be stated:
#   * parks a write  -> its Write/WriteVectored bodies must call ClaimWriteSlot
#   * cannot park    -> a row here saying why, because an unstated impossibility is
#                       indistinguishable from an unexamined one (#892)
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
selftest=0
[ "${1:-}" = "--self-test" ] && selftest=1

# Transports that CANNOT park a write, each with the reason. A file here is not
# scanned for the guard; a file NOT here must call it.
#
# write-slot-scan: data-begin
NoWriteSlot='
InMemorySocket|writes complete inline into the peer buffer; there is no slot to park on
BlockingSocket|blocking send() returns before the awaitable exists; nothing is parked
TlsSocket|owns no slot of its own -- it drives the RAW socket, whose guard is the one that fires
UdpSocket|a datagram transport (IDatagramSocket): it holds no awaitable at all, so there is no slot
'
# write-slot-scan: data-end

reason_for() {
    printf '%s\n' "$NoWriteSlot" | while IFS='|' read -r name why; do
        [ -z "${name:-}" ] && continue
        [ "$name" = "$1" ] && { printf '%s' "$why"; return; }
    done
}

run_scan() {
    local tree="$1" rc=0 seen=0 f base body
    # EVERY .cpp that defines an ISocket, not a `*Socket.cpp` glob: `InMemorySocket`
    # lives in `InMemoryTransport.cpp`, so the obvious glob examined five transports
    # and was silent about a sixth -- exact about what it knew, silent about what it
    # did not, which is the defect this check exists to prevent, in the check itself.
    # The CLASS name is derived from the definitions found, never from the filename.
    for f in "$tree"/src/FastCache/Net/*.cpp; do
        [ -f "$f" ] || continue
        base="$(sed -n 's/^[A-Za-z_][A-Za-z0-9_:<>, ]*[ *&]\([A-Za-z]*Socket\)::Write.*/\1/p' "$f" | head -1)"
        [ -n "$base" ] || continue
        seen=$((seen + 1))
        local why; why="$(reason_for "$base")"
        # Bodies of Write / WriteVectored only, comments stripped -- a COMMENT is not
        # a call site, and checks in this tree have twice matched their own headers.
        body="$(awk -v N="$base" '
            { line = $0; sub(/^[ \t]*\/\/.*$/, "", line) }
            line ~ (N "::Write") { inFn = 1 }
            inFn { print line; if (line ~ /^\}/) inFn = 0 }
        ' "$f")"
        local claims; claims="$(printf '%s' "$body" | grep -c 'ClaimWriteSlot' || true)"
        local arms;   arms="$(printf '%s' "$body" | grep -cE '(writeOp|op)\.awaitable[ \t]*=[ \t]*nullptr' || true)"
        if [ -n "$why" ]; then
            if [ "${claims:-0}" -gt 0 ]; then
                echo "  CONTRADICTION  $base is listed as unable to park a write, yet claims the slot"
                rc=1
            else
                echo "  exempt   $base -- $why"
            fi
            continue
        fi
        if [ "${claims:-0}" -eq 0 ]; then
            echo "  UNGUARDED  $base parks a write and never calls Detail::ClaimWriteSlot"
            echo "             add the claim, or add a row saying why it cannot park"
            rc=1
        elif [ "${arms:-0}" -gt 0 ]; then
            echo "  BARE CLEAR $base still clears the slot directly somewhere in Write/WriteVectored"
            echo "             that is the line the guard exists to remove"
            rc=1
        else
            echo "  guarded  $base ($claims claim(s))"
        fi
    done
    if [ "$seen" -eq 0 ]; then
        echo "REFUSED: no *Socket.cpp under $tree/src/FastCache/Net -- the scan found no subject at all"
        return 2
    fi
    echo "  $seen transport(s) examined"
    return $rc
}

if [ "$selftest" -eq 1 ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    fail=0; cases=0
    mk() { mkdir -p "$tmp/$1/src/FastCache/Net"; }

    # write-slot-scan: data-begin
    mk guarded
    cat > "$tmp/guarded/src/FastCache/Net/EpollSocket.cpp" <<'SRC'
IoAwaitable EpollSocket::Write(std::span<std::byte const> buffer)
{
    Detail::ClaimWriteSlot(_impl->writeOp.awaitable);
}
SRC
    cases=$((cases+1))
    if run_scan "$tmp/guarded" >/dev/null 2>&1; then echo "  case 1 (guarded)            PASS"; else echo "  case 1 (guarded)            FAIL"; fail=1; fi

    mk bare
    cat > "$tmp/bare/src/FastCache/Net/EpollSocket.cpp" <<'SRC'
IoAwaitable EpollSocket::Write(std::span<std::byte const> buffer)
{
    _impl->writeOp.awaitable = nullptr;
}
SRC
    cases=$((cases+1))
    if run_scan "$tmp/bare" >/dev/null 2>&1; then echo "  case 2 (bare clear caught)  FAIL -- not caught"; fail=1; else echo "  case 2 (bare clear caught)  PASS"; fi

    mk newtransport
    cat > "$tmp/newtransport/src/FastCache/Net/SeventhSocket.cpp" <<'SRC'
IoAwaitable SeventhSocket::Write(std::span<std::byte const> buffer)
{
    return IoAwaitable {};
}
SRC
    cases=$((cases+1))
    if run_scan "$tmp/newtransport" >/dev/null 2>&1; then echo "  case 3 (new transport)      FAIL -- joined no list and passed"; fail=1; else echo "  case 3 (new transport)      PASS"; fi

    mk exempt
    cat > "$tmp/exempt/src/FastCache/Net/InMemorySocket.cpp" <<'SRC'
IoAwaitable InMemorySocket::Write(std::span<std::byte const> buffer)
{
    return IoAwaitable {};
}
SRC
    cases=$((cases+1))
    if run_scan "$tmp/exempt" >/dev/null 2>&1; then echo "  case 4 (stated exemption)   PASS"; else echo "  case 4 (stated exemption)   FAIL"; fail=1; fi

    mk contradiction
    cat > "$tmp/contradiction/src/FastCache/Net/InMemorySocket.cpp" <<'SRC'
IoAwaitable InMemorySocket::Write(std::span<std::byte const> buffer)
{
    Detail::ClaimWriteSlot(_impl->writeOp.awaitable);
}
SRC
    cases=$((cases+1))
    if run_scan "$tmp/contradiction" >/dev/null 2>&1; then echo "  case 5 (contradiction)      FAIL -- not caught"; fail=1; else echo "  case 5 (contradiction)      PASS"; fi
    # write-slot-scan: data-end

    mkdir -p "$tmp/empty/src/FastCache/Net"
    cases=$((cases+1))
    run_scan "$tmp/empty" >/dev/null 2>&1
    [ $? -eq 2 ] && echo "  case 6 (empty tree refused) PASS" || { echo "  case 6 (empty tree refused) FAIL"; fail=1; }

    echo "self-test: $cases cases run"
    [ "$fail" -eq 0 ] && echo "SELFTEST OK" || echo "SELFTEST FAILED"
    exit "$fail"
fi

echo "checking the write-slot discipline across src/FastCache/Net"
run_scan "$root"
rc=$?
[ "$rc" -eq 2 ] && exit 1
if [ "$rc" -ne 0 ]; then
    echo "FAILED: a transport parks a write without claiming the slot, or contradicts its own exemption."
    echo "        See FastCache/Net/WriteSlot.hpp and issue #893."
    exit 1
fi
echo "OK"
