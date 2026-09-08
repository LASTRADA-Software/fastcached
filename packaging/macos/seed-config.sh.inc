# --- shared: seed the live config(s) -----------------------------------------
# Substituted into every postinstall that needs it (see packaging/CMakeLists.txt),
# so there is one implementation rather than a copy per component.
#
# A .pkg has no equivalent of a dpkg conffile or rpm %config(noreplace): the
# payload is written unconditionally on every install. So a live config is
# deliberately NOT payload -- it is seeded here from the shipped .default, once,
# and an upgrade then leaves the operator's edits alone.
#
# Every consumer seeds, rather than relying on the Runtime component to have
# done it, because macOS does not run the component postinstalls in the order
# the components are declared. It ran LaunchDaemon's first, whose registration
# passes --config; fastcached then exited with "no such file" and no service was
# registered at all.
#
# **A TABLE, never one hardcoded name.** `fastcache-compile-node.yaml` arrived
# with #291 and reached no non-Linux package for want of a second entry here, so
# a worker on macOS or Windows had no annotated reference at all and every
# setting documented in that file -- including `allow_compile_arg`, which decides
# what a client may make this machine's compiler do -- was invisible there
# (#397). A second copy of this block would have been the same defect carrying a
# maintenance cost, which is why the fix is a loop.
#
# `/bin/sh` here, not bash: an Installer postinstall gets whatever /bin/sh is.
# No `local`, no arrays, no `[[`.
#
# Expects: PREFIX. Defines: CONFIG, DEFAULT_CONFIG (the DAEMON's pair, which
# postinstall-launchdaemon then chowns -- it is the one file with a credential in
# it, so it is the one file whose mode the daemon's component tightens).

# One row per seeded file: `<basename>`. The APPENDIX is per file and is applied
# below by name rather than carried in the row, because only one file has one and
# a column that is empty for every row but one is a column nobody maintains.
FASTCACHED_SEED_FILES="fastcached.yaml fastcache-compile-node.yaml"

for _seed in $FASTCACHED_SEED_FILES; do
    _live="${PREFIX}/etc/${_seed}"
    _template="${PREFIX}/etc/${_seed}.default"

    # Not present in the payload is ORDINARY, not a failure: a component-limited
    # install ships the templates it has. Already present means an operator's
    # file, which is the whole reason this is seed-once.
    #
    # `if`, never `[ ... ] && continue`. These scripts carry `set -u` and not
    # `set -e` today, so the terse form would work -- and it would stop working
    # silently the day somebody adds `set -e`, because the compound's non-zero
    # status would end the postinstall at the first file that is already there,
    # which on an UPGRADE is every file. A loop that stops on the ordinary case is
    # the failure this idiom has already caused elsewhere in this repository.
    if [ -e "$_live" ] || [ ! -e "$_template" ]; then
        continue
    fi
    if ! cp "$_template" "$_live"; then
        continue
    fi

    # `storage_path` must be live rather than commented out, and it is the one
    # setting the shipped reference CANNOT carry: it names the prefix, which is
    # chosen at install time. That asymmetry is why the references under
    # packaging/config/ hold no live absolute path at all -- the installer is the
    # only thing that knows one, so the installer is what writes it.
    #
    # The daemon is given only --config, precisely so this file governs the cache
    # location; left unset it would come up memory-only and lose the whole cache
    # on every restart. The worker has no such setting and gets no appendix.
    if [ "$_seed" = "fastcached.yaml" ]; then
        cat >>"$_live" <<SEED_CONFIG_EOF || true

# --- macOS package defaults -------------------------------------------------
# Set by the installer. Edit freely: this file survives upgrades, and the
# launchd job re-reads it on every start.
storage_path: ${PREFIX}/var/cache
SEED_CONFIG_EOF
    fi
done

# The daemon's pair, named for the consumers that tighten its mode. Set outside
# the loop so it holds whether or not this install seeded anything.
CONFIG="${PREFIX}/etc/fastcached.yaml"
DEFAULT_CONFIG="${PREFIX}/etc/fastcached.yaml.default"
