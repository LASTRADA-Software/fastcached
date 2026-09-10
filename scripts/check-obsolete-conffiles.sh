#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Dropping a config file from the packaging asset table stops NEW packages
# shipping it. It does not remove the copy an older package already put on disk:
# a package manager will not silently delete something it has classified as an
# operator's to edit. So retiring a config file is TWO edits -- delete the asset
# row, and add the path to FASTCACHED_PLATFORM_OBSOLETE_CONFIG -- and the second
# is the one nobody does, because the first one makes the tree look finished.
# `compile-node.env` survived every upgrade for four releases that way (#291,
# #383).
#
# ## What is asserted, and why it is split across two modes
#
# The DEFAULT mode reads source and runs everywhere, including the platforms
# that have no dpkg at all. It cannot see a package, so it asserts the two
# things that are true of the tree:
#
#   * a path is never in BOTH tables. dpkg-deb REFUSES to build a package whose
#     conffiles flags a file that is in the payload, so this mistake surfaces as
#     a packaging-job failure with a message about the archive rather than about
#     the table -- and only on the one job that builds a .deb.
#   * `cmake/Packaging.cmake` actually emits the flag. Without it the export is
#     computed, threaded through, and dropped, which is invisible everywhere.
#
# `--deb <file>` is the behavioural half and needs a real package, so it runs in
# the packaging job. It performs a REAL dpkg upgrade of the REAL package in a
# throwaway root: install a synthesised predecessor that ships the retired path
# as an ordinary conffile, plant an operator edit in it, then unpack the built
# .deb over it. `dpkg --unpack` is enough -- measured: the obsolete conffile is
# retired during UNPACK, before any maintainer script runs -- which is what lets
# this check skip dependency resolution and scriptlets entirely.
#
# ## The control is the point
#
# Both halves carry a positive control, because every refusal here has a way of
# passing for the wrong reason. A parser that matched nothing would report an
# empty intersection and call the tree clean, so the default mode refuses when
# it found no shipped config row at all. And an upgrade that emptied /etc would
# satisfy "the retired file is gone", so `--deb` asserts a config file the new
# package STILL ships is still there afterwards. Removing the right file and
# removing everything are the same assertion without it.
#
# Exit: 0 clean, 1 a rule was broken, 2 usage, 77 a prerequisite is missing.

set -uo pipefail

Refusals=0
refuse() { printf '%s\n' "$*" >&2; Refusals=$((Refusals + 1)); }

# Quoted entries of a `set(<name> ...)` block, empty strings dropped. Every
# block of that name is read and unioned: the obsolete table is declared once as
# an empty initialiser -- so the export below always reads a value this
# configure wrote -- and once as the table proper, and a reader that took only
# the first would see nothing and report clean.
ListEntries() { # ListEntries <file> <variable-name>
    awk -v want="$2" '
        index($0, "set(" want) > 0 { inblock = 1 }
        inblock {
            line = $0
            while (match(line, /"[^"]*"/)) {
                entry = substr(line, RSTART + 1, RLENGTH - 2)
                if (entry != "") print entry
                line = substr(line, RSTART + RLENGTH)
            }
            if (index($0, ")") > 0 && index($0, "set(" want) == 0) inblock = 0
            if (index($0, "set(" want) > 0 && index($0, ")") > 0) inblock = 0
        }
    ' "$1"
}

# The installed path of every `config`-kind asset row, in the SAME unexpanded
# spelling the obsolete table uses (`/${FASTCACHED_SYSCONF_DIR}/name`), so the
# two are comparable without resolving any CMake variable. Resolving would be a
# second implementation of packaging/CMakeLists.txt's own loop.
ShippedConfigPaths() { # ShippedConfigPaths <packaging-cmakelists>
    ListEntries "$1" "FASTCACHED_PLATFORM_ASSETS" \
        | awk -F'|' 'NF == 5 && $3 == "config" { print "/" $2 "/" $4 }'
}

ObsoletePaths() { # ObsoletePaths <packaging-cmakelists>
    ListEntries "$1" "FASTCACHED_PLATFORM_OBSOLETE_CONFIG"
}

RunCheck() { # RunCheck <repo-root>
    local root="$1"
    local packaging="$root/packaging/CMakeLists.txt"
    local generator="$root/cmake/Packaging.cmake"

    local f
    for f in "$packaging" "$generator"; do
        [[ -f "$f" ]] || { refuse "check-obsolete-conffiles: missing $f"; return; }
    done

    local shipped obsolete
    shipped="$(ShippedConfigPaths "$packaging")"
    obsolete="$(ObsoletePaths "$packaging")"

    # Positive control. An empty obsolete table is legitimate -- nothing is
    # retired right now -- but a tree with no SHIPPED config file means the
    # parser stopped matching, and every assertion below would then pass over
    # nothing. Zero rows is not a verdict.
    if [[ -z "$shipped" ]]; then
        refuse "check-obsolete-conffiles: found no config asset rows in $packaging" \
               " -- the reader matched nothing, so nothing below was actually checked"
        return
    fi

    # The table must EXIST even when it is empty, or "nothing is retired" and
    # "the table was deleted" read alike.
    if ! grep -q 'FASTCACHED_PLATFORM_OBSOLETE_CONFIG' "$packaging"; then
        refuse "check-obsolete-conffiles: $packaging declares no FASTCACHED_PLATFORM_OBSOLETE_CONFIG table"
    fi

    # A path cannot be shipped and retired at once.
    local path
    for path in $obsolete; do
        if grep -Fxq -- "$path" <<< "$shipped"; then
            refuse "check-obsolete-conffiles: $path is in BOTH the asset table and the obsolete table" \
                   " -- dpkg-deb refuses to build a package that flags a file it also ships"
        fi
    done

    # The export has to be written on every configure, including the branch that
    # installs nothing: it is CACHE INTERNAL, so a stale value survives a
    # reconfigure that turns packaging off. Two writes -- the fill and the reset
    # -- is the shape packaging/CMakeLists.txt documents for its siblings.
    local writes
    writes="$(grep -c 'set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES' "$packaging")"
    if [[ "$writes" -lt 2 ]]; then
        refuse "check-obsolete-conffiles: FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES is written $writes time(s)" \
               " -- it must also be reset on the branch that installs nothing, or a stale cache entry survives"
    fi

    # The flag must actually be emitted. Computing the list and never writing it
    # is silent in every build and in every test that does not open a .deb.
    if ! grep -q 'FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES' "$generator"; then
        refuse "check-obsolete-conffiles: $generator never reads FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES"
    fi
    if ! grep -q 'remove-on-upgrade' "$generator"; then
        refuse "check-obsolete-conffiles: $generator emits no remove-on-upgrade flag" \
               " -- an unflagged path that is not in the payload is IGNORED by dpkg, not removed"
    fi
}

# --- the behavioural half --------------------------------------------------
#
# One cross: {dpkg, rpm} x {pristine, edited}.
#
# BOTH package managers, because the asymmetry is the thing most likely to go
# false quietly. dpkg needs the flag and rpm needs nothing, and the second half
# of that is a claim about somebody else's tool that this repository relies on
# and does not control. Left unchecked it would stop being true without anything
# here changing, and the residue would come back on rpm alone.
#
# BOTH operator states, because they are different code paths in both tools: a
# pristine file is removed outright, an edited one is kept under a new name. A
# check that always plants an edit never exercises the first, and one that never
# does cannot see the preservation. The pristine arm therefore asserts that
# there is NO preserved sibling -- without that it is the edited arm again,
# passing for the wrong reason.

OperatorMark='operator edit that must survive'

# The retired paths as they appear on an installed system. Only
# FASTCACHED_SYSCONF_DIR occurs in them, and it is declared once at the top
# level.
ResolvedObsoletePaths() { # ResolvedObsoletePaths <repo-root>
    local root="$1"
    local sysconf obsolete path out=""
    sysconf="$(awk '/^set\(FASTCACHED_SYSCONF_DIR/ { if (match($0, /"[^"]*"/)) print substr($0, RSTART + 1, RLENGTH - 2) }' \
        "$root/CMakeLists.txt")"
    [[ -n "$sysconf" ]] || return 1
    obsolete="$(ObsoletePaths "$root/packaging/CMakeLists.txt")"
    for path in $obsolete; do
        out="${out}${path//\$\{FASTCACHED_SYSCONF_DIR\}/$sysconf}"$'\n'
    done
    printf '%s' "$out" | sed '/^$/d'
}

# The outcome every arm must show. `keeper` is a config file the NEW package
# still ships: without it, "the retired file is gone" is equally what an upgrade
# that emptied /etc would report.
AssertRetirement() { # AssertRetirement <label> <fake-root> <paths> <keeper> <state> <sibling-suffix>
    local label="$1"; local r="$2"; local paths="$3"
    local keeper="$4"; local state="$5"; local suffix="$6"
    local path

    for path in $paths; do
        if [[ -e "$r$path" ]]; then
            refuse "$label: $path survived the upgrade -- it is still on operators' disks"
            continue
        fi
        if [[ "$state" == edited ]]; then
            if [[ ! -e "$r$path$suffix" ]]; then
                refuse "$label: $path was removed WITHOUT preserving the operator's copy as $path$suffix"
            elif ! grep -Fxq "$OperatorMark" "$r$path$suffix"; then
                refuse "$label: $path$suffix does not carry the operator's content"
            fi
        else
            # An untouched file must be removed outright. A sibling here means
            # the tool considered it modified, which would mean this arm silently
            # re-ran the edited one.
            if [[ -e "$r$path$suffix" ]]; then
                refuse "$label: $path left a $suffix copy although it was never edited" \
                       " -- this arm did not exercise the pristine path"
            fi
        fi
    done

    [[ -e "$r$keeper" ]] \
        || refuse "$label: the still-shipped config file $keeper did not survive" \
                  " -- the retirement is removing more than the paths it names"
}

DebArms() { # DebArms <deb> <repo-root> <paths>
    local deb="$1"; local root="$2"; local paths="$3"

    command -v dpkg-deb >/dev/null 2>&1 || { echo "check-obsolete-conffiles: no dpkg-deb; skipping"; exit 77; }
    command -v dpkg     >/dev/null 2>&1 || { echo "check-obsolete-conffiles: no dpkg; skipping"; exit 77; }

    local work; work="$(mktemp -d)" || { refuse "check-obsolete-conffiles: mktemp failed"; return; }
    # shellcheck disable=SC2064
    trap "rm -rf '$work'" RETURN

    local control="$work/control"; mkdir -p "$control"
    dpkg-deb -e "$deb" "$control" || { refuse "check-obsolete-conffiles: dpkg-deb -e failed on $deb"; return; }
    [[ -f "$control/conffiles" ]] || { refuse "check-obsolete-conffiles: $deb ships no conffiles"; return; }

    # The artefact declares the flag. Without this the two arms below would still
    # pass on a package that simply never shipped the file.
    local path
    for path in $paths; do
        grep -Fxq -- "remove-on-upgrade $path" "$control/conffiles" \
            || refuse "check-obsolete-conffiles [deb]: $deb does not flag '$path' remove-on-upgrade"
    done

    # Captured, then the first line taken: `producer | head -n 1` is the SIGPIPE
    # shape `check-e2e-helpers.sh`'s early-exit scan refuses (#1181).
    local keeper keepers
    keepers="$(grep -v '^remove-on-upgrade ' "$control/conffiles" || true)"
    keeper="${keepers%%$'\n'*}"
    [[ -n "$keeper" ]] || { refuse "check-obsolete-conffiles [deb]: no ordinary conffile to use as a control"; return; }

    local pkgname; pkgname="$(awk '/^Package:/ { print $2; exit }' "$control/control")"

    local state
    for state in pristine edited; do
        local old="$work/old-$state"
        rm -rf "$old"; mkdir -p "$old/DEBIAN"
        {
            echo "Package: $pkgname"
            echo "Version: 0.0.0-obsolete-conffile-probe"
            echo "Section: misc"
            echo "Priority: optional"
            echo "Architecture: all"
            echo "Maintainer: fastcached packaging check <noreply@example.invalid>"
            echo "Description: synthesised predecessor for the #383 upgrade check"
        } > "$old/DEBIAN/control"
        : > "$old/DEBIAN/conffiles"
        for path in $paths $keeper; do
            mkdir -p "$old$(dirname "$path")"
            printf 'predecessor content\n' > "$old$path"
            printf '%s\n' "$path" >> "$old/DEBIAN/conffiles"
        done
        dpkg-deb --build --root-owner-group "$old" "$work/old-$state.deb" >/dev/null \
            || { refuse "check-obsolete-conffiles [deb]: could not build the synthesised predecessor"; return; }

        local r="$work/root-$state"
        rm -rf "$r"
        mkdir -p "$r/var/lib/dpkg/updates" "$r/var/lib/dpkg/info" \
                 "$r/var/lib/dpkg/triggers" "$r/var/lib/dpkg/alternatives" "$r/etc"
        : > "$r/var/lib/dpkg/status"
        : > "$r/var/lib/dpkg/available"
        printf 'all\n' > "$r/var/lib/dpkg/arch"

        dpkg --root="$r" --force-not-root --install "$work/old-$state.deb" >/dev/null 2>&1 \
            || { refuse "check-obsolete-conffiles [deb]: could not install the synthesised predecessor"; return; }

        if [[ "$state" == edited ]]; then
            for path in $paths; do printf '%s\n' "$OperatorMark" > "$r$path"; done
        fi

        # --unpack is enough: dpkg retires an obsolete conffile during UNPACK,
        # before any maintainer script runs. That is what lets this check skip
        # dependency resolution and scriptlets, and stay clear of the live
        # install steps around it.
        dpkg --root="$r" --force-not-root --force-confold --unpack "$deb" > "$work/unpack-$state.log" 2>&1
        local rc=$?
        if [[ "$rc" -ne 0 ]]; then
            refuse "check-obsolete-conffiles [deb/$state]: unpacking $deb over the predecessor failed (rc=$rc)"
            sed 's/^/    /' "$work/unpack-$state.log" >&2
            continue
        fi

        AssertRetirement "check-obsolete-conffiles [deb/$state]" "$r" "$paths" "$keeper" "$state" ".dpkg-old"
    done
}

RpmArms() { # RpmArms <rpm> <repo-root> <paths>
    local rpmfile="$1"; local root="$2"; local paths="$3"

    command -v rpm      >/dev/null 2>&1 || { echo "check-obsolete-conffiles: no rpm; skipping"; exit 77; }
    command -v rpmbuild >/dev/null 2>&1 || { echo "check-obsolete-conffiles: no rpmbuild; skipping"; exit 77; }

    local work; work="$(mktemp -d)" || { refuse "check-obsolete-conffiles: mktemp failed"; return; }
    # shellcheck disable=SC2064
    trap "rm -rf '$work'" RETURN

    # rpm needs NO retirement mechanism of its own -- that is the claim under
    # test here, so the retired paths must be absent from the package's %config
    # list, and rpm must still take them off disk.
    local configs path
    configs="$(rpm -qcp "$rpmfile" 2>/dev/null)"
    [[ -n "$configs" ]] || { refuse "check-obsolete-conffiles [rpm]: $rpmfile marks no %config files"; return; }
    for path in $paths; do
        ! grep -Fxq -- "$path" <<< "$configs" \
            || refuse "check-obsolete-conffiles [rpm]: $rpmfile still ships '$path' as %config"
    done

    local keeper; keeper="$(head -n 1 <<< "$configs")"
    local pkgname; pkgname="$(rpm -qp --qf '%{NAME}' "$rpmfile" 2>/dev/null)"
    [[ -n "$pkgname" ]] || { refuse "check-obsolete-conffiles [rpm]: could not read the package name"; return; }

    # The predecessor: same name, lower version, shipping every retired path and
    # the control file as %config(noreplace) -- the state a pre-retirement
    # package left on disk.
    local spec="$work/old.spec" install="" files=""
    for path in $paths $keeper; do
        install="${install}mkdir -p %{buildroot}$(dirname "$path")"$'\n'
        install="${install}echo 'predecessor content' > %{buildroot}$path"$'\n'
        files="${files}%config(noreplace) $path"$'\n'
    done
    {
        echo "Name:           $pkgname"
        echo "Version:        0.0.0"
        echo "Release:        obsoleteprobe"
        echo "Summary:        synthesised predecessor for the #383 upgrade check"
        echo "License:        Apache-2.0"
        echo "BuildArch:      noarch"
        echo "%description"
        echo "Synthesised predecessor."
        echo "%install"
        printf '%s' "$install"
        echo "%files"
        printf '%s' "$files"
    } > "$spec"
    rpmbuild -bb --define "_topdir $work/top" "$spec" > "$work/rpmbuild.log" 2>&1 \
        || { refuse "check-obsolete-conffiles [rpm]: could not build the synthesised predecessor"; \
             tail -20 "$work/rpmbuild.log" >&2; return; }
    local oldrpms; oldrpms="$(find "$work/top/RPMS" -name '*.rpm')"
    local oldrpm="${oldrpms%%$'\n'*}"
    [[ -n "$oldrpm" ]] || { refuse "check-obsolete-conffiles [rpm]: rpmbuild produced no package"; return; }

    local state
    for state in pristine edited; do
        local r="$work/root-$state"
        rm -rf "$r"; mkdir -p "$r"
        rpm --root="$r" --initdb > /dev/null 2>&1 \
            || { refuse "check-obsolete-conffiles [rpm]: rpm --initdb failed"; return; }
        rpm --root="$r" -i --nodeps "$oldrpm" > /dev/null 2>&1 \
            || { refuse "check-obsolete-conffiles [rpm]: could not install the synthesised predecessor"; return; }

        if [[ "$state" == edited ]]; then
            for path in $paths; do printf '%s\n' "$OperatorMark" > "$r$path"; done
        fi

        # Scriptlets are chrooted and this root has no shell, so %post fails
        # loudly and harmlessly; the file disposition under test is rpm's own
        # and happens regardless. --noscripts keeps the log readable.
        rpm --root="$r" -U --nodeps --noscripts "$rpmfile" > "$work/upgrade-$state.log" 2>&1
        local rc=$?
        if [[ "$rc" -ne 0 ]]; then
            refuse "check-obsolete-conffiles [rpm/$state]: upgrading to $rpmfile failed (rc=$rc)"
            sed 's/^/    /' "$work/upgrade-$state.log" >&2
            continue
        fi

        AssertRetirement "check-obsolete-conffiles [rpm/$state]" "$r" "$paths" "$keeper" "$state" ".rpmsave"
    done
}

# `kind` is deb or rpm; both take the same shape so the cross is one code path.
CheckPackage() { # CheckPackage <kind> <file> <repo-root>
    local kind="$1"; local file="$2"; local root="$3"
    [[ -f "$file" ]] || { refuse "check-obsolete-conffiles: no such package: $file"; return; }

    local paths
    paths="$(ResolvedObsoletePaths "$root")" \
        || { refuse "check-obsolete-conffiles: could not read FASTCACHED_SYSCONF_DIR"; return; }
    if [[ -z "$paths" ]]; then
        echo "check-obsolete-conffiles: no obsolete config files declared; nothing to verify"
        return
    fi

    local before="$Refusals"
    case "$kind" in
        deb) DebArms "$file" "$root" "$paths" ;;
        rpm) RpmArms "$file" "$root" "$paths" ;;
    esac

    # Said only when it is true. A summary that prints regardless is how a failed
    # run comes to carry a sentence claiming it verified something.
    if [[ "$Refusals" -eq "$before" ]]; then
        echo "check-obsolete-conffiles: $(printf '%s\n' $paths | wc -l | tr -d ' ')" \
             "retired config file(s) verified against a real $kind upgrade" \
             "of $(basename "$file"), pristine and edited"
    fi
}

# --- self-test -------------------------------------------------------------

SelfTest() {
    local ran=0 failures=0 tmp
    tmp="$(mktemp -d)" || exit 1
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" EXIT

    Case() { # Case <name> <clean|refused> <packaging-body> <generator-body>
        local name="$1" want="$2" packaging="$3" generator="$4"
        local d="$tmp/case-$ran"
        mkdir -p "$d/packaging" "$d/cmake"
        printf '%s\n' "$packaging" > "$d/packaging/CMakeLists.txt"
        printf '%s\n' "$generator" > "$d/cmake/Packaging.cmake"
        printf 'set(FASTCACHED_SYSCONF_DIR "etc/fastcached")\n' > "$d/CMakeLists.txt"

        Refusals=0
        RunCheck "$d" >/dev/null 2>&1
        local got=clean
        [[ "$Refusals" -gt 0 ]] && got=refused
        ran=$((ran + 1))
        if [[ "$got" != "$want" ]]; then
            echo "  FAIL: $name -- wanted $want, got $got"
            failures=$((failures + 1))
        fi
    }

    local goodPackaging='set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG "")
set(FASTCACHED_PLATFORM_ASSETS
    "config/a.yaml|${FASTCACHED_SYSCONF_DIR}|config|a.yaml|Runtime"
)
set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG
    "/${FASTCACHED_SYSCONF_DIR}/gone.env"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "x" CACHE INTERNAL "d")
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")'
    local goodGenerator='foreach(_o IN LISTS FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES)
    list(APPEND _lines "remove-on-upgrade ${_o}")
endforeach()'

    Case "a correct tree is clean" clean "$goodPackaging" "$goodGenerator"

    Case "a path in both tables is refused" refused \
'set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG "")
set(FASTCACHED_PLATFORM_ASSETS
    "config/a.yaml|${FASTCACHED_SYSCONF_DIR}|config|a.yaml|Runtime"
)
set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG
    "/${FASTCACHED_SYSCONF_DIR}/a.yaml"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "x" CACHE INTERNAL "d")
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")' \
        "$goodGenerator"

    Case "a generator that never emits the flag is refused" refused \
        "$goodPackaging" \
'foreach(_o IN LISTS FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES)
    list(APPEND _lines "${_o}")
endforeach()'

    # Emits the flag, but from a hardcoded literal rather than from the export --
    # the table computed, threaded through and ignored. This body must NOT also
    # trip the emits-the-flag assertion, or the reads-the-export one is never
    # exercised: measured, a body of `# nothing here` trips both, and deleting
    # the reads-the-export assertion then left the self-test fully green.
    Case "a generator that emits the flag but never reads the export is refused" refused \
        "$goodPackaging" \
'file(WRITE "${OUT}/conffiles" "remove-on-upgrade /etc/hardcoded.env\n")'

    Case "an export written only once is refused" refused \
"set(FASTCACHED_PLATFORM_ASSETS
    \"config/a.yaml|\${FASTCACHED_SYSCONF_DIR}|config|a.yaml|Runtime\"
)
set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG
    \"/\${FASTCACHED_SYSCONF_DIR}/gone.env\"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES \"x\" CACHE INTERNAL \"d\")" \
        "$goodGenerator"

    # The two ways this check can fail to LOOK, both of which read exactly like a
    # clean tree.
    # Everything else in this tree is correct, so only the control can refuse it.
    # A case that refuses for two independent reasons cannot show which
    # assertion it exercises, and stays green when that assertion is deleted.
    Case "a tree with no config asset row is refused, not reported clean" refused \
'set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG "")
set(FASTCACHED_PLATFORM_ASSETS
    "linux/x.service|usr/lib/systemd/system|data|x.service|Runtime"
)
set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG
    "/${FASTCACHED_SYSCONF_DIR}/gone.env"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "x" CACHE INTERNAL "d")
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")' \
        "$goodGenerator"

    Case "a tree whose obsolete table was deleted is refused" refused \
'set(FASTCACHED_PLATFORM_ASSETS
    "config/a.yaml|${FASTCACHED_SYSCONF_DIR}|config|a.yaml|Runtime"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "x" CACHE INTERNAL "d")
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")' \
        "$goodGenerator"

    # An EMPTY table is legitimate and must not be refused, or the check forces a
    # retired path to exist forever.
    Case "an empty obsolete table is clean" clean \
'set(FASTCACHED_PLATFORM_OBSOLETE_CONFIG "")
set(FASTCACHED_PLATFORM_ASSETS
    "config/a.yaml|${FASTCACHED_SYSCONF_DIR}|config|a.yaml|Runtime"
)
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")
set(FASTCACHED_PACKAGE_OBSOLETE_CONFIG_FILES "" CACHE INTERNAL "d")' \
        "$goodGenerator"

    echo "check-obsolete-conffiles --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

case "${1:-}" in
    --self-test) SelfTest ;;
    --deb|--rpm)
        [[ $# -ge 2 ]] || { echo "usage: $0 ${1} <file> [<repo-root>]" >&2; exit 2; }
        Kind="${1#--}"
        Root="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
        CheckPackage "$Kind" "$2" "$Root"
        [[ "$Refusals" -eq 0 ]] || exit 1
        exit 0
        ;;
    -h|--help)
        echo "usage: $0 [<repo-root>] | --deb <file> [<repo-root>] | --rpm <file> [<repo-root>] | --self-test"
        exit 0
        ;;
esac

Root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
RunCheck "$Root"
[[ "$Refusals" -eq 0 ]] || exit 1
echo "check-obsolete-conffiles: obsolete config table and its dpkg emission agree"
exit 0
