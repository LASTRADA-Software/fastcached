#!/usr/bin/env bash
# What MemorySanitizer does on this project's toolchain WITHOUT an instrumented
# standard library.
#
# This is the measurement behind #311, which asks for something in CI that catches
# an UNINITIALISED READ -- the defect ASan, UBSan, TSan and a release build all
# miss, and the one that surfaces in a cache daemon as a single wrong byte served
# as a success. #311 names two routes and says of the second that MemorySanitizer
# "needs an instrumented standard library and is a project in itself".
#
# That sentence was an assessment, not a reading. It is committed here as a
# reading, so the cost can be re-run rather than re-argued: a citation people can
# check is one they stop litigating.
#
# ## What it establishes
#
# MSan poisons everything an UNINSTRUMENTED object writes, because it cannot know
# what that code initialised. `libstdc++.so.6` is such an object on every ordinary
# Linux toolchain, and this tree links it. The question is not whether that is
# true in principle -- it is -- but whether the parts of libstdc++ a real test
# binary REACHES are out-of-line enough to poison anything.
#
# The answer is split, and the split is why a one-program probe would mislead:
#
#   * Header-only use is CLEAN. `std::vector`, `std::string`'s inline members and
#     `std::to_string` are templates instantiated into instrumented code, so a
#     program built only of those reports nothing. A probe that stopped here would
#     conclude MSan is usable, and be wrong.
#   * Out-of-line use REPORTS, deterministically. `std::ostringstream`,
#     `std::locale`, `std::filesystem` and throwing an exception each produce a
#     use-of-uninitialized-value inside `libstdc++.so.6` on a program with no
#     uninitialised read in it at all.
#
# The Catch2 test binaries reach the second set on every run, so an MSan job over
# this tree needs an instrumented libc++ (or libstdc++) BEFORE it can report
# anything about the tree. That is the "project in itself", measured.
#
# ## Why every arm carries a control, and why the control comes first
#
# An arm that reports nothing says nothing until something has been seen to
# report. Three drafts of this probe were wrong in exactly that way and each looked
# like a result:
#
#   1. At `-O1` the uninitialised read was FOLDED AWAY, so the positive control was
#      silent and a clean arm beside it read as "MSan is fine here".
#   2. At `-O0` with the value stored into a `volatile int`, the control was silent
#      again -- MSan reports a value that reaches a BRANCH, a syscall argument or
#      `main`'s return, and a STORE is none of those.
#   3. Only when the control branched on the value did it fire, and only then did
#      the arms below it mean anything.
#
# So: `-O0`, the value reaches an `if`, and the control runs first. If the control
# is silent this probe refuses rather than reporting its other arms.
#
# ## What it does NOT establish
#
# Whether the tree itself reports, and how much. That needs an MSan build of
# `FastCacheTest` with every dependency built from source -- this build links
# `libyaml-cpp.so.0.8`, `libssl.so.3` and `libcrypto.so.3` as system shared objects
# besides libstdc++, and each is a second poisoning surface. TLS can be turned off
# and yaml-cpp can be forced to build from source; libstdc++ cannot be waved away.
#
# Nor does it say anything about the valgrind route, which needs no rebuild at all.
# valgrind is not installed on the machine this was taken on; apt offers
# 1:3.22.0-0ubuntu3.
#
#     Usage:
#         bash scripts/probes/msan-uninstrumented-libstdcxx.sh [compiler]
#
#     Default compiler: clang++-22, the version CI pins.
#
# Measured 2026-09-11, Ubuntu 24.04 under WSL2, clang++-22
# (1:22.1.8~++20260714014902+ca7933e47d3a), libstdc++.so.6 from gcc 14:
#
#     control-branch-on-uninit   reports          (the instrument is live)
#     iostreams                  reports          in std::__ostream_insert
#     locale                     reports
#     filesystem                 reports
#     exceptions                 reports
#     string (header-inlined)    clean
#     threads                    clean
#     libc++ instead             reports          in basic_string::__is_long

set -uo pipefail

CXX="${1:-clang++-22}"
if ! command -v "$CXX" > /dev/null 2>&1; then
    echo "msan-probe: $CXX is not on PATH. Pass a compiler as \$1." >&2
    exit 2
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

flags="-std=c++23 -fsanitize=memory -fno-omit-frame-pointer -g -O0"

reported=0
clean=0
buildFailed=0

# @param 1 arm name
# @param 2 program text
# @param 3.. extra compiler flags
Arm() {
    local name="$1" body="$2"; shift 2
    printf '%s\n' "$body" > "$scratch/arm.cpp"
    if ! $CXX $flags "$@" "$scratch/arm.cpp" -o "$scratch/arm" 2> "$scratch/build"; then
        printf '  %-30s BUILD FAILED\n' "$name"
        # `head` FIRST and `sed` second. The other order is `producer | head`,
        # which under `pipefail` reports the producer's SIGPIPE as this script's
        # status -- the idiom this repository has paid for eighteen times, and
        # `check-e2e-helpers.sh` refused this file for it on its first run.
        head -3 "$scratch/build" | sed 's/^/      /'
        buildFailed=$(( buildFailed + 1 ))
        ArmVerdict="build-failed"
        return 0
    fi
    "$scratch/arm" > "$scratch/out" 2>&1
    local rc=$? warnings
    warnings="$(grep -c 'WARNING: MemorySanitizer' "$scratch/out")"
    if [[ "$warnings" -gt 0 ]]; then
        printf '  %-30s REPORTS  (rc=%s, %s warning(s))\n' "$name" "$rc" "$warnings"
        grep -m1 -A1 'WARNING: MemorySanitizer' "$scratch/out" \
            | sed -n '2p' | sed 's/^/      | /'
        reported=$(( reported + 1 ))
        ArmVerdict="reports"
    else
        printf '  %-30s clean    (rc=%s)\n' "$name" "$rc"
        clean=$(( clean + 1 ))
        ArmVerdict="clean"
    fi
    return 0
}

echo "== MemorySanitizer against an uninstrumented standard library"
# Captured, then first-lined by parameter expansion. `$CXX --version | head -1`
# is the same SIGPIPE shape as above.
compilerBanner="$($CXX --version)"
echo "   compiler: ${compilerBanner%%$'\n'*}"
echo ""
echo "== the control, first. Every arm below is unreadable if this one is silent."

Arm "control-branch-on-uninit" '
int main() { int x; if (x) return 1; return 0; }'

if [[ "$ArmVerdict" != "reports" ]]; then
    echo ""
    echo "msan-probe: REFUSING. The positive control did not report, so this run" >&2
    echo "says nothing about libstdc++ -- a silent arm beside a silent control is" >&2
    echo "not evidence of a clean toolchain, it is evidence of no instrument. The" >&2
    echo "two ways this probe has been wrong before are folding (-O1) and a sink" >&2
    echo "MSan does not treat as a use (a store to volatile); check those first." >&2
    exit 1
fi

echo ""
echo "== header-only use: templates land in INSTRUMENTED code, so this is the arm"
echo "   that makes a one-program probe conclude the wrong thing"

Arm "header-only-containers" '
#include <string>
#include <vector>
#include <numeric>
int main()
{
    std::vector<int> v(64);
    std::iota(v.begin(), v.end(), 1);
    std::string s;
    for (int n: v)
        s += std::to_string(n);
    if (s.size() > 1000000) return 1;
    return 0;
}'

Arm "string-append-and-erase" '
#include <string>
int main()
{
    std::string s(4096, 0x41);
    s.append(4096, 0x42);
    s.erase(0, 100);
    if (s.size() > 100000) return 1;
    return 0;
}'

Arm "threads" '
#include <thread>
#include <atomic>
int main()
{
    std::atomic<int> n { 0 };
    std::thread t([&] { n = 1; });
    t.join();
    if (n != 1) return 1;
    return 0;
}'

echo ""
echo "== out-of-line use: this is what libstdc++.so.6 IS, and what a Catch2 binary"
echo "   reaches on every run. None of these programs has an uninitialised read."

Arm "iostreams" '
#include <sstream>
#include <iostream>
int main()
{
    std::ostringstream o;
    o << 42 << " " << 3.5;
    std::cout << o.str() << "\n";
    return 0;
}'

Arm "locale" '
#include <locale>
#include <sstream>
int main()
{
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << 1234567;
    if (o.str().empty()) return 1;
    return 0;
}'

Arm "filesystem" '
#include <filesystem>
int main()
{
    auto const p = std::filesystem::temp_directory_path() / "x";
    if (p.empty()) return 1;
    return 0;
}'

Arm "exceptions" '
#include <stdexcept>
#include <string>
int main()
{
    try
    {
        throw std::runtime_error("boom");
    }
    catch (std::exception const& e)
    {
        if (std::string(e.what()).empty()) return 1;
    }
    return 0;
}'

echo ""
echo "== the same header-only program against libc++, which unlike libstdc++ keeps"
echo "   part of std::string OUT of line -- so the split is a property of the"
echo "   library's layout rather than of the standard"

Arm "header-only-against-libcxx" '
#include <string>
#include <vector>
#include <numeric>
int main()
{
    std::vector<int> v(64);
    std::iota(v.begin(), v.end(), 1);
    std::string s;
    for (int n: v)
        s += std::to_string(n);
    if (s.size() > 1000000) return 1;
    return 0;
}' -stdlib=libc++

echo ""
echo "== $(( reported + clean + buildFailed )) arm(s): ${reported} reported, ${clean} clean, ${buildFailed} failed to build"
echo ""
echo "Read it as: MSan is LIVE on this toolchain and is unusable over this tree"
echo "until the standard library is instrumented, because the reporting arms are"
echo "the ones every test binary reaches. The clean arms are not a mitigation --"
echo "they are the reason a smaller probe would have said the opposite."
