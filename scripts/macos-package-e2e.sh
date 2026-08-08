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
#   3. Service runs       — the selected launchd job is registered and serving.
#   4. Config survives    — an edit made after install is still there after a
#                           reinstall of the same package.
#   5. Uninstall is total — no files, no job, no PATH entry, no receipts.
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

fail() { echo "macOS package E2E FAILED: $*" >&2; exit 1; }

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

if [[ "$scope" == "daemon" ]]; then
    # No GUI session is involved, so every step here is assertable on a headless
    # runner — which is the whole reason CI runs this variant.
    dscl . -read /Users/_fastcached >/dev/null 2>&1 \
        || fail "postinstall did not create the _fastcached service account"

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
    if launchctl print "gui/$(id -u)/${LABEL}" >/dev/null 2>&1; then
        wait_for_port || fail "nothing listening on 127.0.0.1:${port}"
        serves
        echo "   agent registered and serving on ${port}"
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

grep -qF "$MARKER" "${PREFIX}/etc/fastcached.yaml" \
    || fail "reinstall overwrote the operator's fastcached.yaml"

# --- 6. uninstall must leave nothing ---------------------------------------
echo "== uninstalling"
uninstall_log="$(sudo "${PREFIX}/bin/fastcached-uninstall" 2>&1)" \
    || fail "uninstaller failed: $uninstall_log"

[[ ! -e "$PREFIX" ]]  || fail "$PREFIX still present"
[[ ! -e "$PATHS_D" ]] || fail "$PATHS_D still present"
[[ ! -e /usr/local/bin/fastcached ]]   || fail "/usr/local/bin/fastcached symlink left behind"
[[ ! -e /usr/local/bin/fastcache-cc ]] || fail "/usr/local/bin/fastcache-cc symlink left behind"
! launchctl print "gui/$(id -u)/${LABEL}" >/dev/null 2>&1 || fail "user agent still registered"
! sudo launchctl print "system/${LABEL}" >/dev/null 2>&1 || fail "system daemon still registered"
[[ ! -e "/Library/LaunchDaemons/${LABEL}.plist" ]] || fail "system plist left behind"
[[ -z "$(pkgutil --pkgs | grep "^${LABEL}" || true)" ]] || fail "package receipts left behind"

# A stale service account is what makes a reinstall pick a *different* uid next
# time, silently orphaning the state directory it used to own.
! dscl . -read /Users/_fastcached >/dev/null 2>&1 || fail "_fastcached account left behind"

echo "macOS package E2E: all assertions held"
