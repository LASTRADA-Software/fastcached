# --- shared: create a hidden service account --------------------------------
# Substituted into every postinstall that needs one (see packaging/CMakeLists.txt),
# so there is one implementation rather than a copy per account. macOS has no
# sysusers.d, so what packaging/linux/*.sysusers declares in a single line has to
# be built by hand here -- and every subtlety below was a failure first, which is
# exactly why a second account must not be a second copy of it.
#
# Expects: nothing. Defines: fastcached_create_account.
#
# fastcached_create_account <account> <real name>
#   Creates <account> if it is absent, waits until getpwnam(3) can resolve it, and
#   sets FASTCACHED_ACCOUNT_UID / FASTCACHED_ACCOUNT_GID from the record. Both are
#   left EMPTY when the account could not be created or resolved; every caller
#   tests them rather than assuming, because a postinstall must exit 0 whatever
#   happens and an unset id would otherwise chown the wrong thing.
fastcached_create_account() {
    _fc_account="$1"
    _fc_realname="$2"

    FASTCACHED_ACCOUNT_UID=""
    FASTCACHED_ACCOUNT_GID=""

    # The 200..400 band is the convention for hidden system accounts; picking the
    # next free id keeps this from colliding with another package's.
    #
    # Both namespaces are scanned, because the one number becomes the uid *and*
    # the gid below. dscl enforces no uniqueness of its own, and macOS packs
    # system users and groups into this band densely (_softwareupdate is uid 200
    # and gid 200), so a uid-only scan can hand the new group a gid another group
    # already holds -- which would give that group's members read access to
    # whatever the account is later given.
    if ! /usr/bin/dscl . -read "/Users/${_fc_account}" >/dev/null 2>&1; then
        # The first id in the band that no user and no group already holds, rather
        # than max+1. max+1 produced exactly the collision it was meant to avoid:
        # the scan excluded 400, so a highest-in-band of 399 (com.apple.access_ssh
        # on a stock system) chose 400 -- which com.apple.access_remote_ae already
        # had. Creating the group then failed, and a LaunchDaemon whose group does
        # not exist never spawns.
        #
        # $NF, not $2: a record name may contain spaces, and the id is always last.
        _fc_next_id=$({ /usr/bin/dscl . -list /Users UniqueID; /usr/bin/dscl . -list /Groups PrimaryGroupID; } \
            | /usr/bin/awk '{ used[$NF] = 1 }
                            END { for (id = 200; id <= 400; id++) if (!(id in used)) { print id; exit } }')
        if [ -z "${_fc_next_id:-}" ]; then
            echo "fastcached: no free id in 200..400 for the '${_fc_account}' account" >&2
            return 0
        fi

        # Both interfaces, because neither is reliable alone here: dseditgroup is
        # the supported one and produces a record getgrnam(3) can find, but it has
        # been observed to leave no record at all on a fresh runner; dscl always
        # writes one, though a group assembled attribute-by-attribute is not always
        # resolvable by name. Whichever lands first wins, and nothing below depends
        # on the outcome -- the ids used for chown come from the *user* record.
        /usr/sbin/dseditgroup -q -o create -r "$_fc_realname" -i "$_fc_next_id" "$_fc_account" || true
        if ! /usr/bin/dscl . -read "/Groups/${_fc_account}" PrimaryGroupID >/dev/null 2>&1; then
            /usr/bin/dscl . -create "/Groups/${_fc_account}" PrimaryGroupID "$_fc_next_id" || true
        fi
        /usr/bin/dscl . -create "/Users/${_fc_account}" UniqueID "$_fc_next_id" || true
        /usr/bin/dscl . -create "/Users/${_fc_account}" PrimaryGroupID "$_fc_next_id" || true
        /usr/bin/dscl . -create "/Users/${_fc_account}" UserShell /usr/bin/false || true
        /usr/bin/dscl . -create "/Users/${_fc_account}" NFSHomeDirectory /var/empty || true
        /usr/bin/dscl . -create "/Users/${_fc_account}" RealName "$_fc_realname" || true
        # Without IsHidden the account appears in the login window's user list.
        # It is what hides the account, not the name: this project's accounts are
        # spelled the same on macOS as in the Linux units so that one name reaches
        # the supervisor, the packaging and the binary, rather than three.
        /usr/bin/dscl . -create "/Users/${_fc_account}" IsHidden 1 || true
    fi

    # dscl returns before the record is resolvable through getpwnam(3), which is
    # what launchd -- and fastcached's own pre-flight check -- actually use. Wait
    # instead of racing: an unresolvable UserName leaves launchd registering the job
    # and then never spawning it, with no error anywhere.
    #
    # One flush, then poll. Flushing the whole DirectoryService cache on every
    # iteration is not free, and a loop that ran its full fifty rounds -- which is
    # what happens the moment the condition is something that never becomes true --
    # left opendirectoryd wedged badly enough that the --install-service after it
    # never returned and the installer killed the script ten minutes later.
    #
    # The group is deliberately *not* waited on. No caller needs it by name: the
    # chowns use numeric ids, so group-name resolution is launchd's problem alone,
    # and blocking the install on it buys nothing.
    /usr/bin/dscacheutil -flushcache >/dev/null 2>&1 || true
    _fc_attempt=0
    while [ "$_fc_attempt" -lt 50 ] && ! /usr/bin/id -u "$_fc_account" >/dev/null 2>&1; do
        /bin/sleep 0.1
        _fc_attempt=$((_fc_attempt + 1))
    done
    # Reported, NOT returned on. Timing out here says the DirectoryService cache
    # is still behind, which is exactly the thing the id read below steps around
    # by going to the directory node -- so giving up now would skip a chown that
    # would have worked, and a postinstall does not run again. The caller is left
    # with a service whose state directory and config it does not own, which is
    # the state --install-service refuses; the ids are the better bet.
    /usr/bin/id -u "$_fc_account" >/dev/null 2>&1 \
        || echo "fastcached: '${_fc_account}' is not resolvable via getpwnam after account creation" >&2

    # Numeric ids, read straight from the records just written. Name lookups go
    # through the DirectoryService cache, which lags dscl -- and a chown that names
    # the group does not degrade when that lookup misses, it fails. Reading the
    # directory node avoids the cache entirely, and works on a reinstall too, where
    # the account already exists and the id scan above never ran.
    #
    # BOTH ids come from the *user* record. The gid there is the account's
    # PrimaryGroupID, which is exactly the pw_gid getpwnam(3) hands the service and
    # therefore exactly what its own readability check compares against. Reading it
    # from /Groups instead made the whole thing hostage to a group record that
    # turned out not to exist: the id came back empty, every chown was skipped, and
    # the config was left in a state the install then refused.
    FASTCACHED_ACCOUNT_UID=$(/usr/bin/dscl . -read "/Users/${_fc_account}" UniqueID 2>/dev/null | /usr/bin/awk '{print $2}')
    FASTCACHED_ACCOUNT_GID=$(/usr/bin/dscl . -read "/Users/${_fc_account}" PrimaryGroupID 2>/dev/null | /usr/bin/awk '{print $2}')

    # Recorded because two runs were lost to guessing at this from the outside.
    echo "fastcached: account ${_fc_account} uid=${FASTCACHED_ACCOUNT_UID:-<none>} gid=${FASTCACHED_ACCOUNT_GID:-<none>}" \
        "group-record=$(/usr/bin/dscl . -read "/Groups/${_fc_account}" PrimaryGroupID 2>/dev/null | /usr/bin/awk '{print $2}' || true)" >&2
}
