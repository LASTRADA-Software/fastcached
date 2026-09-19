#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Refuse a shipping ELF binary that links a library this project VENDORS as a shared
# object (#1538).
#
# ## The defect this exists for
#
# `CMakeLists.txt` sets `CPM_USE_LOCAL_PACKAGES ON` and asks CPM for yaml-cpp with
# `YAML_BUILD_SHARED_LIBS OFF`. Those two do not compose: with local packages preferred,
# CPM runs `find_package(yaml-cpp)` FIRST, and on a runner where `libyaml-cpp-dev` is
# installed that succeeds -- so the option naming a static build is never read by
# anything and the binary links `libyaml-cpp.so.0.8`.
#
# For the `.deb` and the `.rpm` that is survivable: `CPACK_DEBIAN_PACKAGE_SHLIBDEPS` and
# `CPACK_RPM_PACKAGE_AUTOREQ` derive a dependency on the runtime package. The TGZ carries
# no dependency metadata at ALL, and the TGZ is what `FASTCACHE_AUTO_INSTALL` unpacks
# onto a developer's machine. There the daemon dies in the dynamic loader with
# `error while loading shared libraries: libyaml-cpp.so.0.8` and exit 127, which the
# configure reported as `fastcached exited immediately (127)` -- a number that reads as
# *not found* for a binary CMake has just staged and knows the path of.
#
# The `Package (macOS .pkg)` job already passes `-DCPM_USE_LOCAL_PACKAGES=OFF` and says
# why in its own comment. The Linux one did not, and nothing was watching, which is what
# this is.
#
# ## What it does NOT cover, stated rather than omitted
#
# Only the libraries this project VENDORS -- the ones a `find_package` hijack turns from
# a static archive into a runtime dependency nobody declared. A GENUINE system library
# (libc, libstdc++, libssl, libcrypto) is a legitimate `NEEDED` entry here and is not
# examined: whether those may be dynamic is a packaging policy question about the whole
# distribution channel, not this defect, and an over-broad rule would refuse every
# correct build. So a clean run says *no vendored library is dynamic*, and it does NOT
# say *this binary is self-contained*.
#
# ELF only. The macOS `.pkg` job closes the same hole through its configure flag, and
# Windows has no shared yaml-cpp in play.
#
# ## Three outcomes, because a reader that could not read is not a clean reading
#
#   0  clean            every binary was read, every one yielded NEEDED entries, and none
#                       of them names a vendored library
#   1  refused          a binary names a vendored library as a runtime dependency
#   2  did not conclude no binary-inspection tool, no binaries at the path, or a binary
#                       whose NEEDED list came back EMPTY -- which is the reader failing,
#                       not a statically linked program: every dynamically linked ELF
#                       here needs at least libc. That is the positive control, and
#                       without it a reader that prints nothing at all reports clean.
#
# A run can produce both of the last two, and then 1 wins: a refusal is a finding about
# the artefact that names the library and the remedy, while 2 sends the reader to the
# instrument instead. Reported the other way round, one unreadable file turns a real
# violation into a question about the tool.
#
# Usage:
#   bash scripts/check-vendored-libraries-static.sh <dir-or-binary> [<dir-or-binary>...]
#   bash scripts/check-vendored-libraries-static.sh --self-test
#
# bash 3.2: no mapfile, no associative arrays, no ${var^^}.

set -u

# The libraries CPM builds into this project, by the SONAME prefix each would appear
# under. One row per vendored dependency that can exist as a system shared library; a
# new CPM dependency that a distribution also packages is a new row.
vendored_prefixes="libyaml-cpp libzstd liblz4"

# What read the binaries, so a clean verdict names the path it was reached by rather
# than leaving the reader to assume one. Set by ChooseReader.
reader_name=""
reader_kind=""

# Pick a binary-inspection tool.
# Sets reader_name and reader_kind; leaves both empty when there is none.
ChooseReader() {
    if command -v readelf > /dev/null 2>&1; then
        reader_name="readelf"
        reader_kind="readelf"
        return
    fi
    if command -v objdump > /dev/null 2>&1; then
        reader_name="objdump"
        reader_kind="objdump"
        return
    fi
    if command -v llvm-readelf > /dev/null 2>&1; then
        reader_name="llvm-readelf"
        reader_kind="readelf"
        return
    fi
    reader_name=""
    reader_kind=""
}

# The NEEDED library names one tool's dynamic-section dump lists, one per line.
#
# The DECISION is kept apart from the acquisition so the self-test can drive it over
# staged text on any host, including one with no ELF binary to point at. Both tools
# spell the line differently and both are parsed here rather than at the call site.
#
# @param 1 The dump text.
# @param 2 The reader kind (`readelf` or `objdump`).
NeededFrom() {
    local text="$1"
    local kind="$2"
    case "$kind" in
        readelf)
            # `0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]`
            printf '%s\n' "$text" | sed -n 's/.*(NEEDED).*Shared library: \[\([^]]*\)\].*/\1/p'
            ;;
        objdump)
            # `  NEEDED               libc.so.6`
            printf '%s\n' "$text" | sed -n 's/^[[:space:]]*NEEDED[[:space:]][[:space:]]*\([^[:space:]]*\).*/\1/p'
            ;;
        *)
            # An unknown kind produces nothing, which the caller reads as an unread
            # binary and escalates -- never as a binary with no dependencies.
            ;;
    esac
}

# Which vendored prefixes a NEEDED list violates, one per line, or nothing.
# @param 1 The NEEDED names, newline-separated.
ViolationsIn() {
    local needed="$1"
    local prefix name
    for prefix in $vendored_prefixes; do
        printf '%s\n' "$needed" | while IFS= read -r name; do
            [ -n "$name" ] || continue
            case "$name" in
                "$prefix"*) printf '%s\n' "$name" ;;
            esac
        done
    done
}

fail_count=0
inconclusive_count=0
checked_count=0

# Report and tally one binary.
# @param 1 The path.
CheckBinary() {
    local path="$1"
    local dump needed violations

    if [ "$reader_kind" = "readelf" ]; then
        dump="$("$reader_name" -d "$path" 2> /dev/null || true)"
    else
        dump="$("$reader_name" -p "$path" 2> /dev/null || true)"
    fi

    needed="$(NeededFrom "$dump" "$reader_kind")"
    if [ -z "$needed" ]; then
        # The positive control. A dynamically linked ELF on this platform needs libc at
        # the very least, so an empty list means the tool did not read the file -- a
        # wrong architecture, a script, a truncated artefact -- and reporting that as
        # "no vendored library found" is the failure this whole file is about.
        printf 'INCONCLUSIVE: %s -- %s listed no NEEDED entry at all, so nothing was judged.\n' \
            "$path" "$reader_name"
        printf '  Every dynamically linked ELF here needs libc, so an empty list means the\n'
        printf '  file was not read: not an ELF, a different architecture, or an artefact\n'
        printf '  sampled mid-build. Check what it is before believing anything about it --\n'
        printf '  a reader that could not read is not a clean reading.\n'
        inconclusive_count=$((inconclusive_count + 1))
        return
    fi

    violations="$(ViolationsIn "$needed")"
    if [ -n "$violations" ]; then
        printf 'FAIL: %s links a VENDORED library dynamically:\n' "$path"
        printf '%s\n' "$violations" | sed 's/^/    /'
        printf '  This build resolved it with find_package instead of building it.\n'
        printf '  Configure that job with -DCPM_USE_LOCAL_PACKAGES=OFF, as the macOS\n'
        printf '  package job does. The .deb and .rpm survive it through SHLIBDEPS;\n'
        printf '  the TGZ that FASTCACHE_AUTO_INSTALL unpacks does not (#1538).\n'
        fail_count=$((fail_count + 1))
        return
    fi

    printf 'ok: %s (%s NEEDED entries, no vendored library among them)\n' \
        "$path" "$(printf '%s\n' "$needed" | grep -c .)"
    checked_count=$((checked_count + 1))
}

SelfTest() {
    local ran=0
    local failed=0

    # A reader's own parse, both spellings, against text this script did not produce.
    local readelfText objdumpText got
    readelfText=' 0x0000000000000001 (NEEDED)             Shared library: [libyaml-cpp.so.0.8]
 0x0000000000000001 (NEEDED)             Shared library: [libstdc++.so.6]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]'
    objdumpText='Dynamic Section:
  NEEDED               libyaml-cpp.so.0.8
  NEEDED               libc.so.6'

    ran=$((ran + 1))
    got="$(NeededFrom "$readelfText" readelf | tr '\n' ' ')"
    if [ "$got" != "libyaml-cpp.so.0.8 libstdc++.so.6 libc.so.6 " ]; then
        printf 'self-test FAIL: readelf parse got <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    ran=$((ran + 1))
    got="$(NeededFrom "$objdumpText" objdump | tr '\n' ' ')"
    if [ "$got" != "libyaml-cpp.so.0.8 libc.so.6 " ]; then
        printf 'self-test FAIL: objdump parse got <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    # The refusing direction.
    ran=$((ran + 1))
    got="$(ViolationsIn "$(NeededFrom "$readelfText" readelf)")"
    if [ "$got" != "libyaml-cpp.so.0.8" ]; then
        printf 'self-test FAIL: a vendored NEEDED was not refused, got <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    # And the ACCEPTING direction, which is the half a guard nobody has watched accept
    # is not known to have: the same reader over a list that is entirely system
    # libraries must find nothing.
    ran=$((ran + 1))
    got="$(ViolationsIn 'libstdc++.so.6
libm.so.6
libc.so.6')"
    if [ -n "$got" ]; then
        printf 'self-test FAIL: a clean NEEDED list was refused, got <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    # An unreadable binary must produce nothing, so the caller's empty-list arm fires.
    ran=$((ran + 1))
    got="$(NeededFrom 'this is not a dynamic section' readelf)"
    if [ -n "$got" ]; then
        printf 'self-test FAIL: unreadable text yielded <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    # A name that CONTAINS a vendored prefix without starting with it is a different
    # library and must be accepted -- a pattern is broader than its author reads it as.
    ran=$((ran + 1))
    got="$(ViolationsIn 'libfoolibyaml-cpp.so.1')"
    if [ -n "$got" ]; then
        printf 'self-test FAIL: a name merely CONTAINING a prefix was refused, got <%s>\n' "$got"
        failed=$((failed + 1))
    fi

    printf 'self-test: %d case(s) ran, %d failed\n' "$ran" "$failed"
    [ "$failed" -eq 0 ] || return 1
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    SelfTest
    exit $?
fi

if [ "$#" -eq 0 ]; then
    printf 'INCONCLUSIVE: no path given; nothing was judged\n'
    exit 2
fi

ChooseReader
if [ -z "$reader_kind" ]; then
    printf 'INCONCLUSIVE: no readelf, llvm-readelf or objdump on PATH; nothing was judged\n'
    exit 2
fi
printf 'vendored-libraries-static: reading with %s; refusing %s\n' "$reader_name" "$vendored_prefixes"

targets=""
for arg in "$@"; do
    if [ -d "$arg" ]; then
        found="$(find "$arg" -maxdepth 1 -type f -perm -u+x -print 2> /dev/null)"
        targets="$targets
$found"
    elif [ -f "$arg" ]; then
        targets="$targets
$arg"
    else
        printf 'INCONCLUSIVE: %s is neither a file nor a directory\n' "$arg"
        inconclusive_count=$((inconclusive_count + 1))
    fi
done

while IFS= read -r target; do
    [ -n "$target" ] || continue
    CheckBinary "$target"
done <<EOF
$targets
EOF

if [ "$checked_count" -eq 0 ] && [ "$fail_count" -eq 0 ]; then
    printf 'INCONCLUSIVE: no binary was read; nothing was judged\n'
    exit 2
fi
printf 'vendored-libraries-static: %d clean, %d refused, %d inconclusive\n' \
    "$checked_count" "$fail_count" "$inconclusive_count"
# A REFUSAL outranks a failure to read, and the order here is the whole of that rule.
# The two are not the same news: a refusal is a finding about the artefact and names the
# library and the flag that fixes it, while `did not conclude` sends whoever meets it to
# look at the instrument. Asked the other way round, one unreadable file downgraded a
# genuine violation into a question about the reader -- the state collapse this script's
# own header is written against, arriving from inside it.
[ "$fail_count" -eq 0 ] || exit 1
[ "$inconclusive_count" -eq 0 ] || exit 2
exit 0
