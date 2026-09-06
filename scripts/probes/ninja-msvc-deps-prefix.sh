#!/usr/bin/env bash
# Determine what Ninja's `deps = msvc` actually matches a /showIncludes note against.
#
# This is the measurement behind #700 and behind `RenderShowIncludes`' marker
# parameter. It is committed so the claim can be re-run rather than re-argued: a
# citation people can check is one they stop litigating.
#
# The question (#700): the launcher synthesises /showIncludes notes for a DISPATCHED
# compile, because a worker compiles preprocessed text and reports no dependencies.
# It wrote the literal English `Note: including file:`. Ninja matches
# `msvc_deps_prefix`, which CMake took from the ACTUAL compiler and which a Visual
# Studio carrying a language pack makes a localized sentence.
#
# Two things needed settling, and only the second is obvious in hindsight:
#
#   1. Does Ninja match the prefix LITERALLY, or does it know something about
#      languages?  (If it normalized, there would be no bug.)
#   2. Is the defect reachable, and is the FIX observable, WITHOUT a localized
#      compiler?  A prior session recorded #700 as "genuinely blocked on a localized
#      cl" and it was not: `msvc_deps_prefix` is a string the BUILD holds, so both
#      the bug and its repair are reachable with any compiler at all -- including no
#      compiler, as here.
#
# Case 1 and case 3 are controls. Case 3 is the one that matters most: reproducing a
# bug proves it is real, but only showing that the right prefix records the
# dependency proves you can tell when it is fixed.
#
#     Usage:
#         bash scripts/probes/ninja-msvc-deps-prefix.sh
#
# Needs nothing but `ninja`. No compiler is involved -- the "compiler" is a shell
# script that prints one note-shaped line, because the subject is Ninja's parser and
# not any compiler's output.
#
# Recorded result, `ninja 1.13.2` on `Linux 7.1.8-200.fc44.x86_64`:
#
#     case                       msvc_deps_prefix   note emitted   deps recorded   rebuilds on header edit
#     1-control-english-english  English            English        1               yes
#     2-defect-german-english    German             English        0               NO
#     3-control-german-german    German             German         1               yes
#
# which is the rule: Ninja matches the literal string and knows nothing about
# languages, so the emitted prefix must equal the build's `msvc_deps_prefix` or the
# translation unit silently acquires no dependencies at all. Row 2 is #700 -- a wrong
# build under a zero exit code, reached through the build graph rather than the cache.
#
# NOT measured here, and deliberately: which prefix a localized `cl` actually prints,
# and on which stream. Nobody on this team has such a host; that is #878 (discovery)
# and #825 (the channel). This probe's whole point is that neither question has to be
# answered to fix #700 or to test the fix.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/ninja-msvc-deps-prefix.XXXXXX")"
trap 'rm -rf "$work"' EXIT

if ! command -v ninja > /dev/null 2>&1; then
    echo "ninja is not on PATH; nothing measured" >&2
    exit 77
fi

ENGLISH='Note: including file:'
GERMAN='Hinweis: Einlesen der Datei:'

failures=0

# Run one case and print its row.
#
# $1 case name, $2 the prefix the build EXPECTS, $3 the prefix the "compiler" EMITS,
# $4 the deps count this case must produce, $5 whether it must rebuild after a header
# edit.  The expectations are arguments rather than an afterthought so that a row
# quietly changing its answer is a failure here and not a table somebody re-reads.
run_case() {
    local name="$1" expect_prefix="$2" emit_prefix="$3" want_deps="$4" want_rebuild="$5"
    local d="$work/$name"
    mkdir -p "$d"
    printf '#define A 1\n' > "$d/dep.h"
    printf 'int main(void){return 0;}\n' > "$d/tu.c"

    # A stand-in for a compiler: prints one /showIncludes-shaped note naming the
    # header, one line of ordinary output, and touches the object. Nothing here is a
    # compiler and nothing needs to be -- the subject is what Ninja does with the
    # note line.
    cat > "$d/fakecl.sh" << 'SH'
#!/usr/bin/env bash
printf '%s %s\r\n' "$1" "$2"
printf 'tu.c\r\n'
: > "$3"
SH
    chmod +x "$d/fakecl.sh"

    cat > "$d/build.ninja" << NINJA
msvc_deps_prefix = $expect_prefix
rule cc
  command = ./fakecl.sh "\$emit" "\$hdr" \$out
  deps = msvc
  description = CC \$out
build tu.o: cc tu.c
  emit = $emit_prefix
  hdr = $d/dep.h
NINJA

    if ! (cd "$d" && ninja > /dev/null 2>&1); then
        printf 'FAIL %-26s the build itself did not succeed\n' "$name"
        failures=$((failures + 1))
        return
    fi

    local deps
    deps="$(cd "$d" && ninja -t deps tu.o 2> /dev/null | grep -c 'dep\.h')"

    # A whole second, because Ninja compares mtimes and a same-second touch is not
    # reliably newer. The probe is three cases; the sleep is not worth optimizing.
    sleep 1.1
    touch "$d/dep.h"
    local rebuild=no
    if (cd "$d" && ninja -n 2> /dev/null | grep -q 'CC tu.o'); then
        rebuild=yes
    fi

    local verdict="ok"
    if [[ "$deps" != "$want_deps" || "$rebuild" != "$want_rebuild" ]]; then
        verdict="UNEXPECTED (wanted deps=$want_deps rebuild=$want_rebuild)"
        failures=$((failures + 1))
    fi
    printf '%-26s expect=%-30s emit=%-30s deps=%s rebuild=%-3s %s\n' \
        "$name" "$expect_prefix" "$emit_prefix" "$deps" "$rebuild" "$verdict"
}

echo "ninja $(ninja --version), $(uname -sr)"
echo
run_case "1-control-english-english" "$ENGLISH" "$ENGLISH" 1 yes
run_case "2-defect-german-english" "$GERMAN" "$ENGLISH" 0 no
run_case "3-control-german-german" "$GERMAN" "$GERMAN" 1 yes
echo

if [[ "$failures" -ne 0 ]]; then
    echo "$failures case(s) did not match the recorded result above." >&2
    echo "Either this ninja behaves differently, or the recorded table is stale." >&2
    exit 1
fi
echo "All three cases match the recorded result: ninja matches msvc_deps_prefix literally."
