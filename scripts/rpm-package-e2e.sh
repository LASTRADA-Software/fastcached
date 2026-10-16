#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Install the built .rpm on an RPM distribution, under a real systemd, and drive
# both services it ships.
#
# The packaging job builds the .rpm on a Debian-family runner and, until this
# script, only ever INSPECTED it: its file list, its %config markings, and a
# retired-conffile upgrade unpacked into a throwaway root. Nothing installed it
# where rpm resolves its dependencies, nothing ran its scriptlets, and no service
# it ships was ever started from it. The .deb got all of that on the runner's own
# systemd; the .rpm got none of it, while being the package every Fedora and
# RHEL-family machine runs.
#
# And neither package started the compile worker with a disk tier, which is how
# #1674 shipped: the unit's ProtectSystem=strict left the documented
# `cache_dir: /var/cache/fastcache-node` read-only, and the node refused to start
# ("--cache-dir cannot create /var/cache/fastcache-node: Read-only file system").
# Reproduced in exactly this container against the 0.5.0 .rpm, which is what the
# worker leg below is for.
#
# WHAT IT ASSERTS, in order:
#
#   install   `dnf install` of the file resolves its dependencies on Fedora and
#             runs %post: the service accounts exist, and a FRESH install has
#             enabled and started fastcached.
#   daemon    fastcached runs as its own account, logs readiness to the journal
#             (so it is not daemonizing), round-trips a set/get, keeps its config
#             at 0640 root:fastcached and readable by that account, applies a
#             SIGHUP reload, and stops with Result=success.
#   worker    the socket activates fastcache-compile-node with a memory AND a disk
#             tier at /var/cache/fastcache-node, the service adopts the socket,
#             runs as fastcache-node, opens its store there, and stops cleanly.
#   upgrade   a reinstall keeps an operator's edit to a %config(noreplace) file.
#   removal   `dnf remove` takes the units away and leaves no enablement symlink.
#
# WHY A CONTAINER WITH systemd AS PID 1, AND WHY --privileged: the units are the
# subject, hardening included (ProtectSystem=strict, PrivateTmp, RestrictNamespaces,
# a seccomp filter), and systemd can only apply that sandbox where it may create
# namespaces and mounts. An unprivileged container would fail those services for a
# reason that is the container's, not the package's.
#
# The base is Fedora from quay.io rather than Docker Hub, whose anonymous pull
# limit is shared by every job on a runner's address.
#
# Usage:
#   bash scripts/rpm-package-e2e.sh <path/to/fastcached-*.rpm> [base-image]
#
# Needs docker. Exit 0 on success, 1 on any failure -- including docker being
# absent: this runs where it was asked for, and a run that could not happen is
# not one that passed.

set -euo pipefail

rpm_path="${1:-}"
base_image="${2:-quay.io/fedora/fedora:44}"

[[ -n "$rpm_path" && -f "$rpm_path" ]] || { echo "usage: $0 <fastcached-*.rpm> [base-image]" >&2; exit 1; }
command -v docker >/dev/null || { echo "rpm-package-e2e: docker is required and was not found" >&2; exit 1; }

readonly NODE_ACCOUNT="fastcache-node"
readonly NODE_CACHE_DIR="/var/cache/fastcache-node"
readonly NODE_PORT=6676
readonly DAEMON_PORT=6674

rpm_name="$(basename "$rpm_path")"
workdir="$(mktemp -d)"
container="fastcached-rpm-e2e-$$"
# One image per base, reused across runs: it holds systemd and a compiler, and
# nothing of the package under test, which is copied into each container fresh.
image_tag="fastcached-rpm-e2e:$(printf '%s' "$base_image" | tr -c 'A-Za-z0-9_.-' '-')"

cleanup() {
    docker rm -f "$container" >/dev/null 2>&1 || true
    rm -rf "$workdir"
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "rpm package E2E" "$workdir"

# Run a command inside the container.
in_container() { docker exec "$container" "$@"; }

# A unit's journal, written to a file on this side so a wait can dump it.
# @param 1 unit
# @param 2 file to write
journal_to() { in_container journalctl -u "$1" --no-pager -o cat > "$2" 2>&1 || true; }

dump_container_state() {
    echo "--- systemctl --failed ---" >&2
    in_container systemctl --failed --no-pager >&2 2>&1 || true
    local unit
    for unit in fastcached fastcache-compile-node.socket fastcache-compile-node; do
        echo "--- journalctl -u ${unit} (last 40) ---" >&2
        in_container journalctl -u "$unit" --no-pager -n 40 >&2 2>&1 || true
    done
    return 0
}

# --- 0. a Fedora with systemd as PID 1 ----------------------------------------
echo "== building ${image_tag} from ${base_image}"
cat > "${workdir}/Containerfile" <<EOF
FROM ${base_image}
RUN dnf -y install systemd util-linux nmap-ncat iproute procps-ng gcc-c++ && dnf clean all
STOPSIGNAL SIGRTMIN+3
CMD ["/usr/sbin/init"]
EOF
docker build -q -t "$image_tag" -f "${workdir}/Containerfile" "$workdir" >/dev/null

echo "== booting ${container}"
docker run -d --name "$container" --privileged --cgroupns=private \
    --tmpfs /run --tmpfs /tmp "$image_tag" >/dev/null
e2e_on_fail dump_container_state

system_booted() {
    local state
    state="$(in_container systemctl is-system-running 2>/dev/null || true)"
    [[ "$state" == running || "$state" == degraded ]]
}
wait_until system_booted "systemd in the container to finish booting" - - 60

# --- 1. install ---------------------------------------------------------------
echo "== dnf install ${rpm_name}"
docker cp "$rpm_path" "${container}:/root/${rpm_name}"
in_container dnf -y install "/root/${rpm_name}" > "${workdir}/dnf-install.log" 2>&1 \
    || { cat "${workdir}/dnf-install.log" >&2; fail "dnf could not install ${rpm_name}"; }
in_container rpm -q fastcached

in_container id fastcached >/dev/null || fail "%post did not create the fastcached account"
in_container id "$NODE_ACCOUNT" >/dev/null || fail "%post did not create the ${NODE_ACCOUNT} account"

# --- 2. the daemon a fresh install starts -------------------------------------
echo "== fastcached"
[[ "$(in_container systemctl is-enabled fastcached 2>&1 || true)" == enabled ]] \
    || fail "a fresh install did not enable fastcached.service"

daemon_ready() {
    journal_to fastcached "${workdir}/daemon.log"
    grep -q 'ready, accepting connections' "${workdir}/daemon.log"
}
wait_until daemon_ready "fastcached to log readiness to the journal" - "${workdir}/daemon.log" 60

[[ "$(in_container systemctl show -p User --value fastcached)" == fastcached ]] \
    || fail "fastcached does not run as the fastcached account"

# Bounded from THIS side, around the whole exchange: run_bounded rather than
# timeout(1), which the fixtures may not name (macOS has none, and the scan reads text).
run_bounded 10 docker exec "$container" bash -c \
    "printf 'set k 0 0 5\r\nhello\r\nget k\r\nquit\r\n' | nc 127.0.0.1 ${DAEMON_PORT}" \
    > "${workdir}/round-trip.txt" 2>&1 || true
grep -q STORED "${workdir}/round-trip.txt" && grep -q hello "${workdir}/round-trip.txt" \
    || { cat "${workdir}/round-trip.txt" >&2; fail "a set/get round trip against the packaged daemon failed"; }

# %post restricts the machine-wide config to the service account (#861); this is the
# INSTALLED result, which a scan of the scriptlet source cannot see.
config_mode="$(in_container stat -c '%a %U:%G' /etc/fastcached/fastcached.yaml)"
[[ "$config_mode" == "640 root:fastcached" ]] \
    || fail "/etc/fastcached/fastcached.yaml is '${config_mode}', expected '640 root:fastcached'"
in_container runuser -u fastcached -- test -r /etc/fastcached/fastcached.yaml \
    || fail "the fastcached account cannot read its own config"

# A reload that applies, asserted by its own log line rather than by the unit still
# being up afterwards -- a SIGHUP nothing handled leaves it up as well.
in_container grep -q '^#log_level: info' /etc/fastcached/fastcached.yaml \
    || fail "the shipped fastcached.yaml no longer contains '#log_level: info'; update this script"
in_container sed -i 's/^#log_level: info/log_level: debug/' /etc/fastcached/fastcached.yaml
in_container systemctl reload fastcached
daemon_reloaded() {
    journal_to fastcached "${workdir}/daemon.log"
    if grep -q 'config reload failed' "${workdir}/daemon.log"; then
        fail "fastcached refused the reload: $(grep 'config reload failed' "${workdir}/daemon.log")"
    fi
    grep -q 'config reloaded' "${workdir}/daemon.log"
}
wait_until daemon_reloaded "fastcached to apply a SIGHUP reload" - "${workdir}/daemon.log" 30

in_container systemctl stop fastcached
[[ "$(in_container systemctl show -p Result --value fastcached)" == success ]] \
    || fail "fastcached did not stop cleanly"

# --- 3. the compile worker, with a disk tier ----------------------------------
echo "== fastcache-compile-node with cache_dir ${NODE_CACHE_DIR}"
# The worker's whole configuration. A named toolchain rather than discovery: a node
# with nothing to serve treats that as a configuration error and exits non-zero at
# its stop, which would fail the clean-stop assertion for a reason that is not the
# package's.
in_container bash -c "printf 'toolchain:\n  - /usr/bin/g++\ncache_memory: 64m\ncache_disk: 256m\ncache_dir: ${NODE_CACHE_DIR}\n' \
    > /etc/fastcached/fastcache-compile-node.yaml"

in_container systemctl enable --now fastcache-compile-node.socket
in_container systemctl is-active --quiet fastcache-compile-node.socket || fail "the worker's socket unit did not start"
if in_container systemctl is-active --quiet fastcache-compile-node; then
    fail "the worker started without a connection, so the socket was bypassed"
fi

run_bounded 10 docker exec "$container" bash -c "exec 3<>/dev/tcp/127.0.0.1/${NODE_PORT}" || true

# Waited for by its OUTPUT: Type=simple reads active at the fork, while the worker
# still has to fingerprint its toolchain. A refusal ends the wait at once rather than
# at the bound -- the defect this leg exists for restarts the unit with exit 1, and
# waiting out 240 s for it says nothing a refusal line has not already said.
node_ready() {
    journal_to fastcache-compile-node "${workdir}/node.log"
    if grep -q 'refusing to start' "${workdir}/node.log"; then
        fail "the worker refused to start: $(grep 'refusing to start' "${workdir}/node.log" | tail -1)"
    fi
    grep -q 'compile node ready' "${workdir}/node.log"
}
wait_until node_ready "the activated worker to log readiness" - "${workdir}/node.log" 240

grep -q "disk 256M [a-z0-9]* at ${NODE_CACHE_DIR}" "${workdir}/node.log" \
    || fail "the worker did not report a 256M disk tier at ${NODE_CACHE_DIR}: $(grep 'local cache tier' "${workdir}/node.log" || echo 'no cache tier line')"
store_owner="$(in_container stat -c '%U %a' "${NODE_CACHE_DIR}/objects.cow" 2>&1 || true)"
[[ "$store_owner" == "${NODE_ACCOUNT} 600" ]] \
    || fail "the worker's store is '${store_owner}', expected '${NODE_ACCOUNT} 600' at ${NODE_CACHE_DIR}/objects.cow"

grep -q 'serving a socket-activated listener' "${workdir}/node.log" \
    || fail "the worker did not adopt the activated socket"
[[ "$(in_container systemctl show -p User --value fastcache-compile-node)" == "$NODE_ACCOUNT" ]] \
    || fail "the worker does not run as ${NODE_ACCOUNT}"

in_container systemctl stop fastcache-compile-node
[[ "$(in_container systemctl show -p Result --value fastcache-compile-node)" == success ]] \
    || fail "the worker did not stop cleanly"
in_container systemctl stop fastcache-compile-node.socket

# --- 4. an upgrade keeps the operator's edits ---------------------------------
echo "== dnf reinstall keeps a local edit"
in_container bash -c "echo '# local edit that must survive' >> /etc/fastcached/fastcached.yaml"
in_container dnf -y reinstall "/root/${rpm_name}" > "${workdir}/dnf-reinstall.log" 2>&1 \
    || { cat "${workdir}/dnf-reinstall.log" >&2; fail "dnf could not reinstall ${rpm_name}"; }
in_container grep -q 'local edit that must survive' /etc/fastcached/fastcached.yaml \
    || fail "a reinstall discarded the local edit to fastcached.yaml"

# --- 5. removal deregisters ---------------------------------------------------
echo "== dnf remove"
in_container systemctl stop fastcached >/dev/null 2>&1 || true
in_container dnf -y remove fastcached > "${workdir}/dnf-remove.log" 2>&1 \
    || { cat "${workdir}/dnf-remove.log" >&2; fail "dnf could not remove fastcached"; }
for leftover in /usr/lib/systemd/system/fastcached.service \
                /usr/lib/systemd/system/fastcache-compile-node.service \
                /usr/lib/systemd/system/fastcache-compile-node.socket \
                /etc/systemd/system/multi-user.target.wants/fastcached.service \
                /etc/systemd/system/sockets.target.wants/fastcache-compile-node.socket; do
    if in_container test -e "$leftover"; then
        fail "removal left ${leftover} behind"
    fi
done

echo "rpm package E2E: ${rpm_name} installed, served, upgraded and removed on ${base_image}"
