#!/usr/bin/env bash
# `apt-get update` that a third-party source cannot fail.
#
# The GitHub `ubuntu-24.04` image ships `packages.microsoft.com` PREINSTALLED.
# Nothing in this repository wants anything from it, but `apt-get update` exits
# non-zero when ANY configured source is unreachable -- so a 403 from that mirror
# failed a step that only wanted `sccache` and `g++-14` from the Ubuntu archive
# (#550).
#
# The cost is not the 25 seconds a re-run takes. A red `clang-tidy` or a red
# `Linux-clang-release` reads as "this change broke something", and it is
# indistinguishable from a real failure until somebody opens the log. Established
# as a flake by an A/B inside ONE run: two jobs with byte-identical install steps,
# same image, same moment -- one passed, the other took the 403.
#
# ## What this does NOT do
#
# It does not make `apt-get update` tolerant. A failure of the UBUNTU ARCHIVE is
# still fatal, because that one means the install below cannot work and a job that
# continued would fail later and less clearly. Only sources this repository does
# not use are removed, and they are removed BEFORE the update rather than having
# their errors ignored afterwards -- ignoring errors is how a real archive failure
# becomes a silent one.
#
# ## Why a script rather than a step
#
# Eleven `apt-get update` call sites in `build.yml`, and this tree has no
# `.github/actions/`, no `workflow_call` and no local `uses: ./`, so a composite
# action would be the first of its kind. A script is a home the next Linux job can
# reach, and `check-apt-update.sh` refuses a workflow that calls `apt-get update`
# directly -- which is what stops the next job added from missing it.
set -euo pipefail

# Sources this repository never installs from. Removed, not ignored: an entry here
# is a claim that nothing we ask for comes from it.
ThirdParty=(
    /etc/apt/sources.list.d/microsoft-prod.list
    /etc/apt/sources.list.d/microsoft.list
)

removed=0
for source in "${ThirdParty[@]}"; do
    if [ -e "$source" ]; then
        sudo rm -f "$source"
        echo "ci-apt-update: removed $source (unused here, and its outages are ours to eat otherwise)"
        removed=$((removed + 1))
    fi
done
echo "ci-apt-update: ${removed} third-party source(s) removed of ${#ThirdParty[@]} known"

# The Ubuntu archive is still allowed to fail the job, loudly.
sudo apt-get update
