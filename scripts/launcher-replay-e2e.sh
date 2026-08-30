#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a REAL target through `fastcache-cc` and run that target's own tests.
#
# ## Why this exists when two launcher fixtures already do
#
# `compile-cache-e2e.sh` and the `fastcache-cc smoke` job prove the launcher RUNS
# and produces AN object. Neither proves it is the RIGHT object, because both
# compile synthetic single files and neither ever executes what came out. That is
# the only shape that catches a launcher handing back an object which does not
# match the source tree — and it stopped being theoretical with #319, where a
# cache-backed build of a test binary segfaulted and the same commit built with
# `-DUSE_COMPILER_CACHE=OFF` passed. Nothing in CI could have reported it.
#
# So: compile a target with many translation units and real argument diversity,
# twice, and RUN it.
#
# ## The three builds, and why each one is needed
#
#   control  `-DUSE_COMPILER_CACHE=OFF`. The reference. Says the source is good
#            and the suite passes when no launcher is involved at all.
#   cold     launcher on, empty cache. Every unit MISSES, so every object is
#            really compiled — and stored.
#   warm     launcher on, same cache. Every unit HITS, so every object is
#            REPLAYED. This is the build under test; replay is where a wrong
#            object comes from, and a job that only builds cold replays nothing.
#
# The objects are byte-compared between **cold and warm**, not between control
# and warm, and that is deliberate rather than lazy: `CompileCache.cmake` sets
# `CMAKE_DISABLE_PRECOMPILE_HEADERS` and `CMAKE_CXX_SCAN_FOR_MODULES OFF` when a
# launcher is active, so a cache-on and a cache-off build are configured
# differently and their objects legitimately differ. Cold against warm is the
# only apples-to-apples comparison available, and it is the one that matters.
# Control participates at the level it can: its suite result must agree.
#
# ## What makes a green run mean something
#
# Two guards, and without either the whole fixture is decorative:
#
#   - the cold build must be observed using the launcher at all (`LAUNCHER = ` in
#     `build.ninja`, the same check the standing `-DUSE_COMPILER_CACHE=OFF` rule
#     is verified by from the other side);
#   - the warm build must be observed HITTING. A warm build that missed
#     everything compiles correctly, passes every assertion below, and has
#     replayed nothing. That is this fixture's own "did the tool actually run"
#     failure, and it is checked by counting.
#
# `--canary` proves the fixture bites: it puts a deliberately wrong object into
# the replayed build and requires the suite to go red. A verifier nobody has seen
# fail is worth nothing.

set -uo pipefail

fastcached=""
launcher=""
compiler=""
port=""
target="fastcache-cc-tests"
canary=0
source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [ $# -gt 0 ]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        --target)     target="$2";     shift 2 ;;
        --canary)     canary=1;        shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

SKIP=77
top_pid=$$

# `fail` is reached from inside subshells -- the control build runs in one so
# it can unset the launcher environment without disturbing the two builds
# after it -- and there a bare `exit` ends the subshell only. The script then
# carries on and reports a SECOND failure about the artefacts the first one
# explains: a configure error was followed by `no build.ninja` and `produced
# no binary`, which names the wrong thing twice and buries the cause.
#
# So a stop is a stop wherever it is raised. `$$` is no help: bash keeps it at
# the parent's value inside a subshell, and the comparison would silently
# always hold. `BASHPID` is the running shell's own pid, which is the whole
# point of the check.
fail() {
    echo "launcher-replay-e2e FAILED: $*" >&2
    [ "${BASHPID:-$$}" = "$top_pid" ] || kill -TERM "$top_pid" 2>/dev/null
    exit 1
}
skip() { echo "launcher-replay-e2e: $* -- skipping"; exit "$SKIP"; }
note() { echo "   $*"; }

for pair in "fastcached:$fastcached" "launcher:$launcher"; do
    path="${pair#*:}"
    [ -n "$path" ] && [ -x "$path" ] || skip "${pair%%:*} was not given an executable"
done
[ -n "$compiler" ] && command -v "$compiler" >/dev/null 2>&1 || skip "no usable compiler ($compiler)"
[ -n "$port" ] || skip "no port given"
command -v cmake  >/dev/null 2>&1 || skip "cmake is not on PATH"
command -v ninja  >/dev/null 2>&1 || skip "ninja is not on PATH"
command -v python3 >/dev/null 2>&1 || skip "python3 is not on PATH"

fastcached="$(cd "$(dirname "$fastcached")" && pwd)/$(basename "$fastcached")"
launcher="$(cd "$(dirname "$launcher")" && pwd)/$(basename "$launcher")"

workdir="$(mktemp -d)"
daemon_pid=""
cleanup() {
    [ -n "$daemon_pid" ] && kill "$daemon_pid" >/dev/null 2>&1
    rm -rf "$workdir"
}
trap cleanup EXIT
# A `fail` raised in a subshell reaches the top-level shell as SIGTERM, whose
# default disposition would report 143. One failing status whichever shell
# raised it; the EXIT trap above still runs and still removes the workdir.
trap 'exit 1' TERM

# ---------------------------------------------------------------------------
echo "== the daemon this build will cache through"
# The flag spellings come from `CliOptions()` rather than from memory. An
# unknown flag makes the daemon print usage and exit, and this script would
# then report `never accepted a connection` for what is really a typo --
# `--memory-limit` was exactly that, the option being `--max-memory`. Its
# suffix is a single character, so `2048mb` is an unknown unit where `2048m`
# is the spelling, and `--log-level=info` makes daemon.log worth catting when
# the readiness loop does give up.
"$fastcached" --bind=127.0.0.1 --port="$port" --max-memory=2048m --log-level=info \
    > "${workdir}/daemon.log" 2>&1 &
daemon_pid=$!

ready=0
for _ in $(seq 1 100); do
    if (exec 3<>/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then ready=1; break; fi
    kill -0 "$daemon_pid" 2>/dev/null || break
    sleep 0.2
done
[ "$ready" = "1" ] || { cat "${workdir}/daemon.log" >&2; fail "the daemon never accepted a connection on ${port}"; }
note "daemon up on 127.0.0.1:${port}"

# ---------------------------------------------------------------------------
# One place that knows how to configure and build, so the three builds cannot
# drift apart in anything but the one variable under test.
configure_and_build() {
    local dir="$1" cache="$2" log="$3"
    local args=(-S "$source_dir" -B "$dir" -G Ninja
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_CXX_COMPILER="$compiler"
                -DFASTCACHED_BUILD_DAEMON=OFF
                -DFASTCACHED_BUILD_NODE=OFF
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
                "-DUSE_COMPILER_CACHE=${cache}")
    [ "$cache" = "ON" ] && args+=("-DFASTCACHE_CC=${launcher}")
    cmake "${args[@]}" > "${log}.configure" 2>&1 \
        || { tail -40 "${log}.configure" >&2; fail "configure failed for ${dir}"; }
    cmake --build "$dir" --target "$target" > "${log}" 2>&1 \
        || { tail -60 "${log}" >&2; fail "build failed for ${dir}"; }
}

# The launcher is reached through the environment for the cache-on builds, and
# the environment is set for the BUILD as well as the configure, because
# `CompileCache.cmake` proves the cache works at configure time by compiling one
# file through it.
export FASTCACHE_ADDR="127.0.0.1:${port}"
export FASTCACHE_VERBOSE=1

echo "== control: the same target with no launcher at all"
(unset FASTCACHE_ADDR FASTCACHE_VERBOSE
 configure_and_build "${workdir}/control" OFF "${workdir}/control.build")
grep -q "LAUNCHER = " "${workdir}/control/build.ninja" \
    && fail "the control build has a compiler launcher configured; it is not a control"
note "control built, and build.ninja confirms no launcher"

control_bin="${workdir}/control/target/${target}"
[ -x "$control_bin" ] || control_bin="$(find "${workdir}/control" -name "$target" -type f -perm -u+x | head -1)"
[ -n "$control_bin" ] && [ -x "$control_bin" ] || fail "the control build produced no ${target} binary"
"$control_bin" > "${workdir}/control.tests" 2>&1 \
    || { tail -30 "${workdir}/control.tests" >&2; fail "the control suite failed; the source itself is not good"; }
control_summary="$(grep -E "All tests passed|assertions in" "${workdir}/control.tests" | tail -1)"
note "control suite: ${control_summary}"

echo "== cold: every unit misses, so every object is really compiled and stored"
configure_and_build "${workdir}/cold" ON "${workdir}/cold.build"
grep -q "LAUNCHER = " "${workdir}/cold/build.ninja" \
    || fail "the cold build has NO compiler launcher: nothing was cached and this fixture verified nothing"
cold_hits="$(grep -c "fastcache-cc: HIT" "${workdir}/cold.build" || true)"
cold_misses="$(grep -c "fastcache-cc: MISS" "${workdir}/cold.build" || true)"
note "cold: ${cold_misses} miss(es), ${cold_hits} hit(s)"
[ "$cold_misses" -gt 0 ] || fail "the cold build missed nothing; the cache was not empty and nothing was stored from this source"

echo "== warm: every unit hits, so every object is REPLAYED"
configure_and_build "${workdir}/warm" ON "${workdir}/warm.build"
warm_hits="$(grep -c "fastcache-cc: HIT" "${workdir}/warm.build" || true)"
warm_misses="$(grep -c "fastcache-cc: MISS" "${workdir}/warm.build" || true)"
note "warm: ${warm_hits} hit(s), ${warm_misses} miss(es)"

# THE guard. A warm build that missed compiles correctly, passes everything
# below, and has replayed nothing -- a green run proving only that the compiler
# works. Replay is the entire subject of this fixture.
[ "$warm_hits" -gt 0 ] || fail "the warm build replayed NOTHING (${warm_hits} hits); every assertion below would have tested the compiler rather than the cache"
if [ "$warm_misses" -gt 0 ]; then
    note "note: ${warm_misses} unit(s) missed on the warm build; the replay is partial"
fi

echo "== the replayed objects must be the objects that were compiled"
differing=0
compared=0
while IFS= read -r cold_obj; do
    rel="${cold_obj#${workdir}/cold/}"
    warm_obj="${workdir}/warm/${rel}"
    [ -f "$warm_obj" ] || continue
    compared=$((compared + 1))
    cmp -s "$cold_obj" "$warm_obj" || { echo "   DIFFERS: ${rel}" >&2; differing=$((differing + 1)); }
done < <(find "${workdir}/cold" -name '*.o' -type f)
note "compared ${compared} object(s)"
[ "$compared" -gt 0 ] || fail "no objects were compared; the layout assumption in this fixture is wrong"
[ "$differing" = "0" ] || fail "${differing} replayed object(s) differ from the object that was compiled from this source"

echo "== and the replayed build must pass its own tests"
warm_bin="${workdir}/warm/target/${target}"
[ -x "$warm_bin" ] || warm_bin="$(find "${workdir}/warm" -name "$target" -type f -perm -u+x | head -1)"
[ -n "$warm_bin" ] && [ -x "$warm_bin" ] || fail "the warm build produced no ${target} binary"
"$warm_bin" > "${workdir}/warm.tests" 2>&1 \
    || { tail -40 "${workdir}/warm.tests" >&2; fail "the suite FAILED on replayed objects while passing on freshly compiled ones -- this is the #319 shape"; }
warm_summary="$(grep -E "All tests passed|assertions in" "${workdir}/warm.tests" | tail -1)"
note "replayed suite: ${warm_summary}"

[ "$control_summary" = "$warm_summary" ] \
    || fail "the cached and uncached builds do not agree: control '${control_summary}' vs replayed '${warm_summary}'"
note "the cached and uncached builds agree"

# ---------------------------------------------------------------------------
if [ "$canary" = "1" ]; then
    echo "== canary: a wrong object in the replayed build must turn this red"
    # What this proves and what it does not, stated rather than implied. It
    # proves the CHAIN: an object that does not match its source reaches the
    # link, the binary runs, and the suite reports it. It does not simulate the
    # cache producing that object -- nothing external can arrange that without
    # forging a key -- so the injection is done where the effect is identical.
    victim_src="$(python3 - "${workdir}/warm/compile_commands.json" <<'PY'
import json, sys
db = json.load(open(sys.argv[1]))
for entry in db:
    if entry["file"].endswith("_test.cpp"):
        print(entry["file"]); break
PY
)"
    [ -n "$victim_src" ] || fail "canary: no test translation unit found in the compile database"

    victim_cmd="$(python3 - "${workdir}/warm/compile_commands.json" "$victim_src" <<'PY'
import json, sys
db = json.load(open(sys.argv[1]))
for entry in db:
    if entry["file"] == sys.argv[2]:
        print(entry.get("command") or " ".join(entry["arguments"])); break
PY
)"
    victim_obj="$(python3 - "${workdir}/warm/compile_commands.json" "$victim_src" <<'PY'
import json, re, sys
db = json.load(open(sys.argv[1]))
for entry in db:
    if entry["file"] == sys.argv[2]:
        cmd = entry.get("command") or " ".join(entry["arguments"])
        m = re.search(r"-o\s+(\S+)", cmd)
        print(m.group(1) if m else ""); break
PY
)"
    [ -n "$victim_cmd" ] && [ -n "$victim_obj" ] || fail "canary: could not read the compile command for ${victim_src}"

    # A copy that differs from the source the object claims to be built from.
    # Appended rather than edited, so it cannot depend on the file's contents.
    cp "$victim_src" "${workdir}/canary.cpp"
    cat >> "${workdir}/canary.cpp" <<'EOF'

#include <catch2/catch_test_macros.hpp>
TEST_CASE("launcher-replay canary: this object does not match its source", "[canary]")
{
    FAIL("the canary object was linked, which is what this fixture must notice");
}
EOF

    # Compiled with the build's own command line, minus the launcher, so the only
    # difference from the real object is the source it came from.
    canary_cmd="${victim_cmd/${victim_src}/${workdir}/canary.cpp}"
    canary_cmd="${canary_cmd//${launcher} /}"
    (cd "${workdir}/warm" && eval "$canary_cmd") > "${workdir}/canary.compile" 2>&1 \
        || { tail -20 "${workdir}/canary.compile" >&2; fail "canary: the wrong object would not compile"; }

    # Linked directly, NOT through `cmake --build`, and this was found by probing
    # rather than reasoning: ninja records each output's mtime in `.ninja_log`, so
    # an object replaced underneath it reads as DIRTY and is REBUILT -- silently
    # undoing the injection and leaving a canary that always passes. The first
    # version of this did exactly that, and a throwaway probe caught it. Taking the
    # link command ninja would run and running only that bypasses the dirty check,
    # which is also the truer simulation: a wrong cached object is one the build
    # system has no reason to touch again.
    link_cmd="$(ninja -C "${workdir}/warm" -t commands "$target" | tail -1)"
    [ -n "$link_cmd" ] || fail "canary: could not obtain the link command for ${target}"
    (cd "${workdir}/warm" && eval "$link_cmd") > "${workdir}/canary.link" 2>&1 \
        || { tail -20 "${workdir}/canary.link" >&2; fail "canary: the relink failed, so the suite never ran"; }

    if "$warm_bin" > "${workdir}/canary.tests" 2>&1; then
        tail -20 "${workdir}/canary.tests" >&2
        fail "canary: the suite PASSED with a wrong object linked in. This fixture cannot detect the thing it exists to detect."
    fi
    note "canary: the suite went red on a wrong object, as it must"
fi

echo "launcher-replay-e2e: a real target replayed from cache passes its own tests"
