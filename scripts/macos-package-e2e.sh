#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the macOS .pkg (POSIX). Installs the real package onto the
# running machine, exercises it, and removes it again.
#
# This needs root and it modifies the host, so it is deliberately not part of
# `ctest`. CI runs it; a developer can too, on a machine they do not mind
# installing fastcached on.
#
# The properties it asserts, each of which has failed silently at some point in
# a way the unit tests could not see:
#
#   1. Redistributable    — the shipped binaries link nothing outside /usr/lib.
#                           A Homebrew dylib path here means the package works
#                           only on the build machine.
#   2. Correct payload    — the binaries, the PATH entry and the uninstaller are
#                           installed, and the *live* config is NOT payload (or
#                           the seed-if-absent postinstall is dead code and an
#                           upgrade eats the operator's edits).
#   3. Panes are legible  — every installer pane declares the mime-type its
#                           resource actually is. Installer.app renders a pane
#                           by that attribute and not by the file extension, so
#                           an .html pane without it is shown with its tags
#                           visible — which is how the welcome and read-me panes
#                           shipped once, while the .txt license looked fine.
#   4. Service runs       — the selected launchd job is registered and serving.
#   5. Config survives    — an edit made after install is still there after a
#                           reinstall of the same package.
#   6. Uninstall is total — no files, no job, no PATH entry, no receipts.
#
# --scope selects which launchd choice the installer is driven with:
#
#   daemon  installs the system-wide LaunchDaemon. This is the variant CI runs,
#           because it is the only one that can be verified end-to-end without a
#           human: a LaunchDaemon needs no logged-in user, so the account
#           creation, the registration and the serving are all observable on a
#           headless runner.
#   agent   installs the per-user LaunchAgent (the installer's default). Its
#           postinstall deliberately does nothing when there is no console user
#           with a GUI session, which is exactly the situation on a CI runner —
#           so this variant can only assert the payload there. The agent's
#           launchd behaviour is covered instead by macos-service-e2e.sh, which
#           drives the same code path directly.
#
# Usage:
#   macos-package-e2e.sh --pkg <path-to-.pkg> [--port <n>] [--scope agent|daemon]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime
# prerequisite was missing (skip).
set -euo pipefail

pkg=""
port="11211"
scope="daemon"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pkg)   pkg="$2";   shift 2 ;;
        --port)  port="$2";  shift 2 ;;
        --scope) scope="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$scope" in
    agent|daemon) ;;
    *) echo "--scope must be 'agent' or 'daemon', got '$scope'" >&2; exit 2 ;;
esac

readonly SKIP=77

[[ "$(uname -s)" == "Darwin" ]] || { echo "not macOS; skipping"; exit "$SKIP"; }
[[ -n "$pkg" && -f "$pkg" ]] || { echo "package not found: '$pkg'; skipping"; exit "$SKIP"; }
sudo -n true 2>/dev/null || { echo "passwordless sudo unavailable; skipping"; exit "$SKIP"; }

readonly PREFIX="/opt/fastcached"
readonly LABEL="software.lastrada.fastcached"
readonly PATHS_D="/etc/paths.d/fastcached"

workdir="$(mktemp -d)"
cleanup() {
    sudo "${PREFIX}/bin/fastcached-uninstall" >/dev/null 2>&1 || true
    rm -rf "$workdir"
}
trap cleanup EXIT

# On failure, dump what the postinstall scripts recorded before giving up. A
# .pkg postinstall must exit 0 whatever happens, so its diagnostics are the only
# evidence of a step that declined to run -- and without them a failure here
# looks like "the service is just missing".
fail() {
    echo "macOS package E2E FAILED: $*" >&2
    if [[ -r /var/log/fastcached-install.log ]]; then
        echo "--- /var/log/fastcached-install.log ---" >&2
        tail -40 /var/log/fastcached-install.log >&2
    fi
    sudo grep -i fastcached /var/log/install.log 2>/dev/null | tail -20 >&2 || true
    exit 1
}

# --- 1. inspect the payload before touching the machine --------------------
echo "== inspecting the payload"
pkgutil --expand-full "$pkg" "${workdir}/expanded" >/dev/null \
    || fail "pkgutil could not expand $pkg"

payload="$(cd "${workdir}/expanded" && find . -path '*/Payload/*' -type f | sed 's|.*/Payload/||' | sort -u)"

for expected in opt/fastcached/bin/fastcached \
                opt/fastcached/bin/fastcache-cc \
                opt/fastcached/bin/fastcached-uninstall \
                opt/fastcached/etc/fastcached.yaml.default \
                etc/paths.d/fastcached; do
    grep -qx "$expected" <<<"$payload" || fail "missing from payload: $expected"
done

# The live config must NOT ship: a .pkg has no conffile mechanism and overwrites
# its payload on every install, so shipping it would discard operator edits.
! grep -qx "opt/fastcached/etc/fastcached.yaml" <<<"$payload" \
    || fail "the live fastcached.yaml is in the payload; an upgrade would overwrite it"

# Third-party build artefacts must not leak into a package rooted at /.
! grep -qE '^(include|lib)/' <<<"$payload" \
    || fail "payload contains dependency headers/libraries at the filesystem root"

# Every installer pane must declare the mime-type of its resource. Installer.app
# renders a pane according to that attribute, not according to the file's
# extension, so an .html pane without it is displayed with its markup showing.
# CMake's stock Distribution template emits no mime-type at all, which is how
# this shipped: cmake/Packaging.cmake generates an override, and this is the
# assertion that the override actually reached productbuild. The table mirrors
# the one there — kept as rows rather than an associative array, because
# /bin/bash on macOS is still 3.2.
echo "== checking the installer panes"
distribution="${workdir}/expanded/Distribution"
[[ -r "$distribution" ]] || fail "the expanded package has no Distribution file"

pane_mime_types=(".txt=text/plain" ".html=text/html" ".rtf=text/rtf" ".rtfd=text/rtfd")
for pane in welcome readme license; do
    element="$(grep -o "<${pane}[^>]*>" "$distribution" | head -1)"
    [[ -n "$element" ]] || fail "the Distribution file declares no <${pane}> pane"

    paneFile="$(sed -n 's/.*file="\([^"]*\)".*/\1/p' <<<"$element")"
    paneMime="$(sed -n 's/.*mime-type="\([^"]*\)".*/\1/p' <<<"$element")"
    [[ -n "$paneFile" ]] || fail "<${pane}> names no resource file: ${element}"

    expected=""
    for row in "${pane_mime_types[@]}"; do
        [[ "${row%%=*}" == ".${paneFile##*.}" ]] && expected="${row#*=}"
    done
    [[ -n "$expected" ]] \
        || fail "<${pane}> names ${paneFile}, whose extension productbuild does not render"
    [[ "$paneMime" == "$expected" ]] || fail \
        "<${pane}> gives ${paneFile} mime-type='${paneMime:-<none>}', expected '${expected}'; Installer.app would render that pane as plain text"
done

# --- 2. the shipped binaries must be redistributable -----------------------
echo "== checking dynamic-library dependencies"
for tool in fastcached fastcache-cc; do
    binary="$(find "${workdir}/expanded" -path "*/Payload/opt/fastcached/bin/${tool}" -type f | head -1)"
    [[ -n "$binary" ]] || fail "could not locate ${tool} in the expanded payload"
    strays="$(otool -L "$binary" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/' || true)"
    [[ -z "$strays" ]] || fail "${tool} links libraries outside /usr/lib:"$'\n'"$strays"
done

# --- 3. install ------------------------------------------------------------
# Drive the installer's choices explicitly rather than accepting the defaults,
# so the service variant under test is the one that was asked for. The
# identifiers are CPack's, derived from the component names.
choices="${workdir}/choices.xml"
if [[ "$scope" == "daemon" ]]; then
    agent_selected=0; daemon_selected=1
else
    agent_selected=1; daemon_selected=0
fi
cat > "$choices" <<XML
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
  <dict>
    <key>choiceIdentifier</key><string>LaunchAgentChoice</string>
    <key>choiceAttribute</key><string>selected</string>
    <key>attributeSetting</key><integer>${agent_selected}</integer>
  </dict>
  <dict>
    <key>choiceIdentifier</key><string>LaunchDaemonChoice</string>
    <key>choiceAttribute</key><string>selected</string>
    <key>attributeSetting</key><integer>${daemon_selected}</integer>
  </dict>
</array>
</plist>
XML

echo "== installing (scope: ${scope})"
# Captured rather than redirected: a redirect is opened by this shell, not by
# sudo, so `sudo cmd > file` would silently write as the unprivileged user.
install_log="$(sudo installer -pkg "$pkg" -applyChoiceChangesXML "$choices" -target / 2>&1)" \
    || fail "installer failed: $install_log"

[[ -x "${PREFIX}/bin/fastcached" ]]           || fail "no ${PREFIX}/bin/fastcached"
[[ -x "${PREFIX}/bin/fastcache-cc" ]]         || fail "no ${PREFIX}/bin/fastcache-cc"
[[ -x "${PREFIX}/bin/fastcached-uninstall" ]] || fail "no uninstaller"
[[ -f "${PREFIX}/etc/fastcached.yaml" ]]      || fail "postinstall did not seed fastcached.yaml"

grep -qx "${PREFIX}/bin" "$PATHS_D" || fail "$PATHS_D does not name ${PREFIX}/bin"

# The receipts must carry the bundle id as their prefix, because that is the
# only thing the uninstaller has to find them by. Asserted positively here: the
# step-6 check that no receipt survives passes vacuously if they were never
# registered under this prefix in the first place, which is exactly what an
# empty CPACK_PRODUCTBUILD_IDENTIFIER produced.
pkgutil --pkgs | grep -q "^${LABEL}\." \
    || fail "no package receipt starts with ${LABEL}; the uninstaller would never find them"

# The symlinks are what make the tools reachable in a shell that is already
# open, and in fish, which never reads /etc/paths.d.
[[ -L /usr/local/bin/fastcached ]]   || fail "no /usr/local/bin/fastcached symlink"
[[ -L /usr/local/bin/fastcache-cc ]] || fail "no /usr/local/bin/fastcache-cc symlink"

# --- 4. the selected launchd job must be registered and serving -------------
echo "== checking the launchd registration"

wait_for_port() {
    for _ in $(seq 1 150); do
        (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

serves() {
    reply="$(printf 'set k 0 0 5\r\nhello\r\nget k\r\nquit\r\n' | nc -w 5 127.0.0.1 "$port" || true)"
    [[ "$reply" == *"STORED"* && "$reply" == *"hello"* ]] || fail "service did not serve: $reply"
}

# Echo the launchd domain holding this user's agent, if any. The candidate list
# and its order mirror ScopeTraits::domains in ServiceControl.cpp -- assert
# against every domain the installer could have chosen, not just the preferred
# one, or an agent in the fallback domain is invisible to this script.
registered_agent_domain() {
    for domain in "gui/$(id -u)" "user/$(id -u)"; do
        if launchctl print "${domain}/${LABEL}" >/dev/null 2>&1; then
            echo "$domain"
            return 0
        fi
    done
    return 1
}

if [[ "$scope" == "daemon" ]]; then
    # No GUI session is involved, so every step here is assertable on a headless
    # runner — which is the whole reason CI runs this variant.
    dscl . -read /Users/_fastcached >/dev/null 2>&1 \
        || fail "postinstall did not create the _fastcached service account"

    # ProgramArguments must carry --config and nothing the config file also
    # governs. A --storage baked in here outranks YAML for the life of the
    # registration, so the operator's storage_path edit would be a silent no-op.
    plist="/Library/LaunchDaemons/${LABEL}.plist"
    [[ -f "$plist" ]] || fail "no $plist"
    grep -q -- "--config=${PREFIX}/etc/fastcached.yaml" "$plist" \
        || fail "the daemon plist does not pass --config; the config file would never be read"
    ! grep -q -- "--storage" "$plist" \
        || fail "the daemon plist bakes in --storage; storage_path in the config file would be ignored"

    # The daemon drops to _fastcached before opening that file, and the reason
    # to keep `requirepass:` there rather than in the plist is that it need not
    # be world-readable. Both halves are asserted: group-readable by the service
    # account, and not readable by everyone else.
    # That the daemon can in fact read it is proven functionally a few lines
    # down, by the job reaching `running` and serving; this pins the mode, which
    # a functional check cannot distinguish from 0644.
    config_mode="$(stat -f '%Sp %Su' "${PREFIX}/etc/fastcached.yaml")"
    [[ "$config_mode" == "-rw-r----- root" ]] \
        || fail "fastcached.yaml is '${config_mode}', expected '-rw-r----- root'"

    # The group is compared numerically. %Sg would need a gid->name lookup,
    # which is the very thing that has been unreliable right after account
    # creation -- and the daemon compares numerically too, via getpwnam's
    # pw_gid, so this asserts what actually governs access.
    config_gid="$(stat -f '%g' "${PREFIX}/etc/fastcached.yaml")"
    account_gid="$(dscl . -read /Users/_fastcached PrimaryGroupID | awk '{print $2}')"
    [[ -n "$account_gid" && "$config_gid" == "$account_gid" ]] \
        || fail "fastcached.yaml gid is '${config_gid}', expected _fastcached's '${account_gid:-<unset>}'"

    # Polled: launchd reports a transient `xpcproxy` state while the stub execs
    # the real binary, so a single sample races the spawn.
    state=""
    for _ in $(seq 1 150); do
        state="$(sudo launchctl print "system/${LABEL}" 2>/dev/null | awk -F'= ' '/^\tstate = /{print $2; exit}')"
        [[ "$state" == "running" || "$state" == "not running" ]] && break
        sleep 0.1
    done
    [[ "$state" == "running" ]] || fail "system daemon state is '${state:-<absent>}', expected running"

    # The daemon must not be running as root: the point of the service account
    # is that a network-facing process does not have the whole machine.
    owner="$(ps -o user= -p "$(sudo launchctl print "system/${LABEL}" | awk -F'= ' '/^\tpid = /{print $2; exit}')" | tr -d ' ')"
    [[ "$owner" == "_fastcached" ]] || fail "daemon runs as '${owner}', expected _fastcached"

    wait_for_port || fail "nothing listening on 127.0.0.1:${port}"
    serves
    echo "   system daemon running as _fastcached and serving on ${port}"
else
    # The agent's postinstall deliberately does nothing without a console user,
    # which is the normal case on a runner. Assert whichever outcome applies
    # rather than demanding one that cannot happen here.
    #
    # Both domains are probed, because which one an agent lands in is decided at
    # install time: gui/<uid> needs an Aqua session, which a runner has not got,
    # so a registration here falls back to user/<uid>. Probing only gui/<uid>
    # made a real agent look absent — and made the uninstall assertion below
    # pass without ever testing anything.
    if agent_domain="$(registered_agent_domain)"; then
        wait_for_port || fail "nothing listening on 127.0.0.1:${port}"
        serves
        echo "   agent registered in ${agent_domain} and serving on ${port}"
    else
        echo "   no per-user agent (no console session); payload assertions only"
    fi
fi

# --- 5. an operator edit must survive a reinstall --------------------------
echo "== reinstalling over an edited config"
readonly MARKER="# fastcached-e2e-marker"
echo "$MARKER" | sudo tee -a "${PREFIX}/etc/fastcached.yaml" >/dev/null

reinstall_log="$(sudo installer -pkg "$pkg" -applyChoiceChangesXML "$choices" -target / 2>&1)" \
    || fail "reinstall failed: $reinstall_log"

# Read as root: the daemon install tightens this file to 0640 root:_fastcached
# so the `requirepass:` it is meant to hold is not readable by every account.
sudo grep -qF "$MARKER" "${PREFIX}/etc/fastcached.yaml" \
    || fail "reinstall overwrote the operator's fastcached.yaml"

# --- 6. uninstall must leave nothing ---------------------------------------
echo "== uninstalling"
uninstall_log="$(sudo "${PREFIX}/bin/fastcached-uninstall" 2>&1)" \
    || fail "uninstaller failed: $uninstall_log"

[[ ! -e "$PREFIX" ]]  || fail "$PREFIX still present"
[[ ! -e "$PATHS_D" ]] || fail "$PATHS_D still present"
[[ ! -e /usr/local/bin/fastcached ]]   || fail "/usr/local/bin/fastcached symlink left behind"
[[ ! -e /usr/local/bin/fastcache-cc ]] || fail "/usr/local/bin/fastcache-cc symlink left behind"
! registered_agent_domain >/dev/null || fail "user agent still registered in $(registered_agent_domain)"
! sudo launchctl print "system/${LABEL}" >/dev/null 2>&1 || fail "system daemon still registered"
[[ ! -e "/Library/LaunchDaemons/${LABEL}.plist" ]] || fail "system plist left behind"
[[ -z "$(pkgutil --pkgs | grep "^${LABEL}" || true)" ]] || fail "package receipts left behind"

# A stale service account is what makes a reinstall pick a *different* uid next
# time, silently orphaning the state directory it used to own.
! dscl . -read /Users/_fastcached >/dev/null 2>&1 || fail "_fastcached account left behind"

echo "macOS package E2E: all assertions held"
