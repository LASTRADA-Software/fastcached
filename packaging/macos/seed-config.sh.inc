# --- shared: seed the live config -------------------------------------------
# Substituted into every postinstall that needs it (see packaging/CMakeLists.txt),
# so there is one implementation rather than a copy per component.
#
# A .pkg has no equivalent of a dpkg conffile or rpm %config(noreplace): the
# payload is written unconditionally on every install. So the live config is
# deliberately NOT payload -- it is seeded here from the shipped .default, once,
# and an upgrade then leaves the operator's edits alone.
#
# Every consumer seeds, rather than relying on the Runtime component to have
# done it, because macOS does not run the component postinstalls in the order
# the components are declared. It ran LaunchDaemon's first, whose registration
# passes --config; fastcached then exited with "no such file" and no service was
# registered at all.
#
# Expects: PREFIX. Defines: CONFIG, DEFAULT_CONFIG.
CONFIG="${PREFIX}/etc/fastcached.yaml"
DEFAULT_CONFIG="${PREFIX}/etc/fastcached.yaml.default"

if [ ! -e "$CONFIG" ] && [ -e "$DEFAULT_CONFIG" ]; then
    if cp "$DEFAULT_CONFIG" "$CONFIG"; then
        # storage_path must be live rather than commented out. The daemon is
        # given only --config, precisely so this file governs the cache
        # location; left unset it would come up memory-only and lose the whole
        # cache on every restart. The shipped .default is the annotated
        # reference shared with the Linux packages, where every key is a comment
        # and the example path is the FHS one.
        cat >>"$CONFIG" <<SEED_CONFIG_EOF || true

# --- macOS package defaults -------------------------------------------------
# Set by the installer. Edit freely: this file survives upgrades, and the
# launchd job re-reads it on every start.
storage_path: ${PREFIX}/var/cache
SEED_CONFIG_EOF
    fi
fi
