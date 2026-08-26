#!/bin/bash
# Sweep this branch's changed sources with the PINNED clang-tidy.
#
# `scripts/local-gate.sh` runs clang-tidy through the `clang-debug` preset, which
# needs a toolchain that can build the project. Where that is unavailable, this
# gets the same checks out of a compile database alone.
#
# **It verifies the tool actually RUNS before believing a clean result.** That is
# the whole reason this is a script rather than a one-line loop: a wrapper that
# cannot exec, a binary extracted without its execute bit, or one that cannot find
# its own resource headers all produce *silence*, and silence filtered through a
# grep for "error:" reads exactly like success. That mistake has already sent a
# branch to CI twice with findings a local sweep had reported clean.
#
# Getting the pinned binary (no root needed; see
# .agent/rules/build-and-toolchain.md):
#
#   pip download "clang-tidy==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/ct --no-deps
#   python3 -m zipfile -e /tmp/ct/clang_tidy-*.whl /tmp/ct22
#   chmod +x /tmp/ct22/clang_tidy/data/bin/clang-tidy    # the wheel loses the bit
#
# Run it from where it was unpacked, or through a wrapper that does: clang-tidy
# resolves its resource headers relative to the binary.
#
# And the database must be a CLANG one with module scanning off -- the `@…modmap`
# flags a module-scanning generator emits do not exist until that target has been
# built, and a translation unit that fails to parse reports nothing:
#
#   cmake -S . -B out/build/tidy22 -G Ninja \
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
#         -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
#         -DFASTCACHED_ENABLE_TLS=ON
#
# Usage:  [TIDY=clang-tidy-22] [DB=out/build/tidy22] [BASE=origin/master] \
#         scripts/tidy-sweep.sh
set -u

TIDY="${TIDY:-clang-tidy-22}"
DB="${DB:-out/build/tidy22}"
BASE="${BASE:-origin/master}"

fatal() { echo "TIDY SWEEP FATAL: $*" >&2; exit 2; }

command -v "$TIDY" >/dev/null 2>&1 || fatal "$TIDY is not on PATH"
"$TIDY" --version >/dev/null 2>&1 || fatal "$TIDY will not run"
[[ -f "${DB}/compile_commands.json" ]] || fatal "no compile database at ${DB}"

# A canary against a real file. Anything that stops clang-tidy from parsing shows
# up here rather than as an entire branch reported clean.
probe_file="$(git ls-files 'src/FastCache/Core/*.cpp' | head -1)"
[[ -n "$probe_file" ]] || fatal "no probe file to canary against"
probe="$("$TIDY" -p "$DB" --quiet "$probe_file" 2>&1)"
probe_rc=$?
[[ "$probe_rc" -ge 126 ]] && fatal "$TIDY could not be executed (exit ${probe_rc})"
case "$probe" in
    *"Permission denied"*|*"error: no such file or directory: '@"*|*"'stddef.h' file not found"*)
        fatal "$TIDY is not parsing: ${probe}" ;;
esac

# Committed changes AND files not yet added. The second half is not a nicety: a
# brand-new source file is untracked until `git add`, so a list built from
# `git diff` alone skips exactly the code that has never been checked by anything
# -- and reports a confident count while doing it.
status=0
mapfile -t files < <({ git diff --name-only "${BASE}...HEAD" -- '*.cpp'
                       git diff --name-only -- '*.cpp'
                       git ls-files --others --exclude-standard -- '*.cpp'; } | sort -u)
[[ "${#files[@]}" -gt 0 ]] || { echo "TIDY SWEEP: nothing changed against ${BASE}"; exit 0; }

for file in "${files[@]}"; do
    [[ -f "$file" ]] || continue
    out="$("$TIDY" -p "$DB" --quiet "$file" 2>&1)"
    rc=$?
    [[ "$rc" -ge 126 ]] && fatal "$TIDY failed to run on ${file} (exit ${rc})"
    # Unknown *warning options* are the GCC-only flags a clang build has no use
    # for; everything else is a finding.
    hits="$(printf '%s\n' "$out" | grep -E 'error:|warning:' | grep -v 'unknown-warning-option')"
    if [[ -n "$hits" ]]; then
        echo "=== ${file}"
        printf '%s\n' "$hits"
        status=1
    fi
done

[[ "$status" -eq 0 ]] && echo "TIDY SWEEP CLEAN (${#files[@]} file(s), ${TIDY})"
exit "$status"
