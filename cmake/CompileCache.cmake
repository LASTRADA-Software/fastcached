# SPDX-License-Identifier: Apache-2.0
#
# Compiler-cache launcher selection.
#
# Three launchers are supported, in preference order:
#
#   1. fastcache-cc — the launcher from the fastcached project. Its entries are
#      portable across checkout paths, so a CI runner and a developer working
#      from different directories share cache hits. It must already be installed
#      and on PATH. It is configured purely through the environment and caches
#      nothing unless FASTCACHE_ADDR / FASTCACHE_SRCROOT / FASTCACHE_BUILDTREE
#      are all set; the address defaults to fastcached's own port,
#      127.0.0.1:6674, and the two roots are injected here via `cmake -E env`,
#      because CMake already knows them. Selecting it is conditional on a daemon
#      actually answering there — see the probe below.
#   2. sccache — the usual third-party launcher, used when fastcache-cc is
#      unavailable or unconfigured. Supports shared (Redis/S3/...) caches.
#   3. ccache — the classic local cache, used when neither of the above applies.
#
# Launchers are wired in as compiler launchers, so CPM-/FetchContent-fetched
# dependencies get cached too. If a launcher is already set (e.g. via the
# command line or a preset), it is left untouched.
#
# This file is self-contained on purpose — it uses nothing but stock CMake and
# names no target of the project it sits in — so it can be dropped into another
# repository verbatim. Two things follow from that and must be preserved: it
# cannot use CPM or FetchContent (a consuming project may have neither, and here
# it is included before CPM is even bootstrapped), and it must never fail a
# configure, since a build that has no cache should still be a build.
#
# When none of the three is installed, -DFASTCACHE_AUTO_INSTALL=ON fetches
# fastcache-cc from GitHub Releases rather than leaving the build uncached; see
# the block below. To disable everything: -DUSE_COMPILER_CACHE=OFF.

option(USE_COMPILER_CACHE
       "Use a compiler-cache launcher when one is available (fastcache-cc when a daemon answers, else sccache, else ccache) [default: ON]"
       ON)

# Respect a launcher provided externally (command line, preset, toolchain).
# Check both C and CXX: a toolchain may set only one of them, and we must not
# override either (nor silently set the other alongside it).
if(DEFINED CMAKE_CXX_COMPILER_LAUNCHER OR DEFINED CMAKE_C_COMPILER_LAUNCHER)
    message(STATUS "[cache] Compiler launcher already set externally "
                   "(C='${CMAKE_C_COMPILER_LAUNCHER}', CXX='${CMAKE_CXX_COMPILER_LAUNCHER}'); leaving it untouched.")
    # A build tree configured before this module existed carries the launcher of
    # the day in its cache, and would keep it forever without a word about why
    # the selection below never runs.
    if(DEFINED CACHE{CMAKE_CXX_COMPILER_LAUNCHER} OR DEFINED CACHE{CMAKE_C_COMPILER_LAUNCHER})
        message(STATUS "[cache] That value comes from the CMake cache (a -D, a preset, or an older configure); "
                       "reconfigure with --fresh to let this module choose instead.")
    endif()
    return()
endif()

find_program(FASTCACHE_CC fastcache-cc DOC "fastcache-cc tool path; needs a fastcached daemon to be used")
find_program(SCCACHE sccache DOC "sccache tool path")
find_program(CCACHE ccache DOC "ccache tool path")

# find_program never revisits a cache entry it already filled, which is right for
# a PATH lookup and wrong for the auto-installed launcher below: that one lives in
# a per-user cache directory, and a cache cleaner emptying it would otherwise leave
# this pointing at a binary that is no longer there for the life of the build tree.
# Forget a path that has stopped existing so the lookup — and the fetch — can run
# again.
if(FASTCACHE_CC AND NOT EXISTS "${FASTCACHE_CC}")
    unset(FASTCACHE_CC CACHE)
    find_program(FASTCACHE_CC fastcache-cc DOC "fastcache-cc tool path; needs a fastcached daemon to be used")
endif()

# Where the daemon is: FASTCACHE_ADDR from the environment, else fastcached's
# own port, which a stock daemon (and the service the installers register)
# listens on. An empty -DFASTCACHE_ADDR= opts out of fastcache-cc entirely.
set(_fc_addr_env "$ENV{FASTCACHE_ADDR}")
if(_fc_addr_env STREQUAL "")
    set(_fc_addr_wanted "127.0.0.1:6674")
else()
    set(_fc_addr_wanted "${_fc_addr_env}")
endif()

# Ordinary cache semantics would freeze the address at whatever the first
# configure saw, so exporting FASTCACHE_ADDR to reach a remote daemon would do
# nothing until the build tree was wiped. Track the environment across
# configures instead and let a *change* to it retarget the cache entry — while
# leaving a -D from this very run alone, which is the one instruction more
# deliberate than the environment. The two are told apart by whether the cache
# still holds what this module last put there, which is also why the retarget
# needs a previous configure to compare against: on a first configure there is
# no bookkeeping yet, both tests hold vacuously, and a -DFASTCACHE_ADDR= meant
# to opt out would be overwritten by an address merely left in the environment.
if(NOT DEFINED CACHE{FASTCACHE_ADDR})
    set(FASTCACHE_ADDR "${_fc_addr_wanted}" CACHE STRING
        "host:port of the fastcached compile-cache daemon, 127.0.0.1:6674 by default (empty disables the fastcache-cc launcher)")
elseif(DEFINED CACHE{_FASTCACHE_ADDR_APPLIED}
       AND NOT _fc_addr_env STREQUAL "${_FASTCACHE_ADDR_ENV_SEEN}"
       AND FASTCACHE_ADDR STREQUAL "${_FASTCACHE_ADDR_APPLIED}")
    message(STATUS "[cache] FASTCACHE_ADDR changed in the environment; retargeting to ${_fc_addr_wanted}")
    set(FASTCACHE_ADDR "${_fc_addr_wanted}" CACHE STRING
        "host:port of the fastcached compile-cache daemon, 127.0.0.1:6674 by default (empty disables the fastcache-cc launcher)"
        FORCE)
endif()
set(_FASTCACHE_ADDR_ENV_SEEN "${_fc_addr_env}" CACHE INTERNAL
    "FASTCACHE_ADDR as the environment last presented it, to notice a change on reconfigure")
set(_FASTCACHE_ADDR_APPLIED "${FASTCACHE_ADDR}" CACHE INTERNAL
    "the address this module last applied, to tell its own value from one set externally")

# How fastcache-cc is configured, in one place: the probe below must test the
# very environment the build will use, or it would vouch for a configuration
# nothing else runs.
set(_fc_fastcache_env
    "FASTCACHE_ADDR=${FASTCACHE_ADDR}"
    "FASTCACHE_SRCROOT=${CMAKE_SOURCE_DIR}"
    "FASTCACHE_BUILDTREE=${CMAKE_BINARY_DIR}")

# Optional auto-install of fastcache-cc from this project's GitHub Releases.
#
# Vendored into another repository, everything above buys that repository
# nothing unless fastcache-cc is already on its PATH — a manual, out-of-band
# step on every new checkout and every fresh machine. With
# FASTCACHE_AUTO_INSTALL=ON this fetches the launcher itself: a prebuilt binary
# from the latest stable release, picked by host OS and architecture.
#
# Prebuilt rather than built from source, because building it would need a
# compiler, a build tree and several minutes at configure time — inside the very
# module whose job is to make compiling faster.
#
# Opt-in, because reaching out to the network during `cmake` is a meaningful
# change from "use what is already installed", and because it is what makes this
# module write outside the build tree for the first time. The binary is staged
# in a per-user directory shared by every repository and every build tree, so it
# is fetched once per machine and version rather than once per build.
#
# Nothing here can fail a configure. Every error path — no binary published for
# this platform, no network, a corrupt download, a binary that will not run —
# reports one status line and leaves FASTCACHE_CC empty, so selection falls
# through to sccache, then ccache, then no caching, exactly as it does today.
#
# That last property is why the download is deliberately *not* checked with
# `file(DOWNLOAD ... EXPECTED_HASH)`: a mismatch there is fatal even when STATUS
# is captured (measured), which is the one thing this must never be. The hash is
# compared by hand below instead — the same guarantee, minus the abort.

option(FASTCACHE_AUTO_INSTALL
       "Download fastcache-cc from GitHub Releases when no compiler-cache launcher is installed [default: OFF]"
       OFF)

set(FASTCACHE_AUTO_INSTALL_REPO "LASTRADA-Software/fastcached" CACHE STRING
    "owner/name of the GitHub repository to fetch prebuilt fastcache-cc binaries from")
set(FASTCACHE_AUTO_INSTALL_API "https://api.github.com" CACHE STRING
    "base URL of the GitHub API used to resolve the latest release")
set(FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE "https://github.com" CACHE STRING
    "base URL release archives are downloaded from (point at a mirror to install without reaching GitHub)")
set(FASTCACHE_AUTO_INSTALL_VERSION "" CACHE STRING
    "exact fastcache-cc version to install (empty resolves the latest stable release)")
set(FASTCACHE_AUTO_INSTALL_TTL_HOURS "24" CACHE STRING
    "how long a resolved latest release is reused before the GitHub API is asked again")

# Where a fetched launcher is kept. Per user rather than per build tree, so the
# second build tree on a machine costs nothing, and per version, so a new release
# never overwrites a binary a configured tree still points at.
#
# The platform split mirrors the launcher's own choice of state directory
# (src/apps/fastcache-cc/Stats.cpp) — with the *cache* spellings rather than the
# state ones, since a re-downloadable binary is by definition cache and belongs
# where a cache cleaner may take it.
if(NOT DEFINED FASTCACHE_AUTO_INSTALL_DIR)
    if(DEFINED ENV{FASTCACHE_CACHE_DIR} AND NOT "$ENV{FASTCACHE_CACHE_DIR}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{FASTCACHE_CACHE_DIR}")
    elseif(CMAKE_HOST_WIN32 AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{LOCALAPPDATA}/fastcache-cc")
    elseif(CMAKE_HOST_APPLE AND NOT "$ENV{HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{HOME}/Library/Caches/fastcache-cc")
    elseif(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{XDG_CACHE_HOME}/fastcache-cc")
    elseif(NOT "$ENV{HOME}" STREQUAL "")
        set(_fc_auto_dir_default "$ENV{HOME}/.cache/fastcache-cc")
    else()
        # No home to speak of (a bare container, a service account). Fall back to
        # the build tree: it still works, it just cannot be shared.
        set(_fc_auto_dir_default "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-cc")
    endif()
    get_filename_component(_fc_auto_dir_default "${_fc_auto_dir_default}" ABSOLUTE)
    set(FASTCACHE_AUTO_INSTALL_DIR "${_fc_auto_dir_default}" CACHE PATH
        "directory prebuilt fastcache-cc binaries are staged in, shared across repositories and build trees")
endif()

# Published-asset table, one row per (host system, host architecture) this
# project releases a binary for. A host with no row is not an error — it simply
# falls back, which is what every platform does today. Publishing a new platform
# is a new row here and nothing below changes.
#
#   _fc_asset_<id>_system    CMAKE_HOST_SYSTEM_NAME this row serves
#   _fc_asset_<id>_arch      host processor spellings mapping to this row
#   _fc_asset_<id>_platform  asset-name infix, after "fastcached-<version>-"
#   _fc_asset_<id>_ext       archive extension
#   _fc_asset_<id>_member    path to the launcher inside the archive's top-level
#                            directory, which differs per platform because the
#                            packages install to different prefixes
#   _fc_asset_<id>_exe       staged executable's filename
set(_fc_asset_rows linux_x86_64 darwin_arm64 windows_amd64)

set(_fc_asset_linux_x86_64_system "Linux")
set(_fc_asset_linux_x86_64_arch x86_64 amd64 AMD64)
set(_fc_asset_linux_x86_64_platform "Linux-x86_64")
set(_fc_asset_linux_x86_64_ext "tar.gz")
set(_fc_asset_linux_x86_64_member "usr/bin/fastcache-cc")
set(_fc_asset_linux_x86_64_exe "fastcache-cc")

set(_fc_asset_darwin_arm64_system "Darwin")
set(_fc_asset_darwin_arm64_arch arm64 aarch64)
set(_fc_asset_darwin_arm64_platform "Darwin-arm64")
set(_fc_asset_darwin_arm64_ext "tar.gz")
set(_fc_asset_darwin_arm64_member "opt/fastcached/bin/fastcache-cc")
set(_fc_asset_darwin_arm64_exe "fastcache-cc")

# The Windows archive is a plain ZIP rather than the MSI beside it: an installer
# is not something this can open, and the launcher is one self-contained file
# with no VC++ redistributable behind it. Its interior differs from the two
# above because the Windows package is the only one not rooted at /, so its
# binaries sit at the conventional bin/ rather than usr/bin or opt/fastcached.
set(_fc_asset_windows_amd64_system "Windows")
set(_fc_asset_windows_amd64_arch AMD64 x86_64 x64)
set(_fc_asset_windows_amd64_platform "Windows-AMD64")
set(_fc_asset_windows_amd64_ext "zip")
set(_fc_asset_windows_amd64_member "bin/fastcache-cc.exe")
set(_fc_asset_windows_amd64_exe "fastcache-cc.exe")

# Pick the row serving this host.
# @param outVar Receives the row id, or empty when no binary is published for it.
function(_fc_auto_install_select_row outVar)
    set(${outVar} "" PARENT_SCOPE)
    foreach(_id IN LISTS _fc_asset_rows)
        if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "${_fc_asset_${_id}_system}")
            continue()
        endif()
        # Architecture spellings vary by OS and by how CMake was told about the
        # host, so a row lists every name that means it rather than one.
        foreach(_arch IN LISTS _fc_asset_${_id}_arch)
            if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "${_arch}")
                set(${outVar} "${_id}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
endfunction()

# Fetch (or reuse) the GitHub release metadata describing the latest stable
# release. GitHub's "latest" already excludes drafts and prereleases, so it is
# the stable release by definition.
#
# The response is cached on disk with a time-to-live for two reasons that both
# bite in CI: unauthenticated API access is limited to 60 requests per hour per
# IP, which a fleet of runners behind one egress address exhausts quickly, and a
# machine that has resolved once should keep working when the network is gone.
# A token in the environment raises that limit and is used when present.
#
# @param outVar Receives the release JSON.
# @param reasonVar Receives a short diagnostic when outVar is empty.
function(_fc_auto_install_release_json outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # One cache file per repository: the staging directory is shared, and two
    # projects pointing at different forks must not read each other's answer.
    string(REPLACE "/" "_" _repoSlug "${FASTCACHE_AUTO_INSTALL_REPO}")
    set(_cacheFile "${FASTCACHE_AUTO_INSTALL_DIR}/${_repoSlug}-latest.json")

    if(EXISTS "${_cacheFile}")
        file(TIMESTAMP "${_cacheFile}" _stamp "%s")
        string(TIMESTAMP _now "%s" UTC)
        if(_stamp AND _now)
            math(EXPR _ageHours "(${_now} - ${_stamp}) / 3600")
            if(_ageHours LESS ${FASTCACHE_AUTO_INSTALL_TTL_HOURS})
                file(READ "${_cacheFile}" _cached)
                set(${outVar} "${_cached}" PARENT_SCOPE)
                set(${reasonVar} "" PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()

    set(_headers HTTPHEADER "Accept: application/vnd.github+json")
    # Either spelling, because gh and actions/checkout each export their own.
    foreach(_tokenVar GITHUB_TOKEN GH_TOKEN)
        if(NOT "$ENV{${_tokenVar}}" STREQUAL "")
            list(APPEND _headers HTTPHEADER "Authorization: Bearer $ENV{${_tokenVar}}")
            break()
        endif()
    endforeach()

    set(_tmp "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-download/latest.json")
    file(DOWNLOAD
        "${FASTCACHE_AUTO_INSTALL_API}/repos/${FASTCACHE_AUTO_INSTALL_REPO}/releases/latest"
        "${_tmp}"
        ${_headers}
        STATUS _status
        TIMEOUT 30
        INACTIVITY_TIMEOUT 15)
    list(GET _status 0 _code)

    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        # A stale answer beats no answer: the release we last saw is still a real
        # release, and a machine that is merely offline should keep building.
        if(EXISTS "${_cacheFile}")
            file(READ "${_cacheFile}" _cached)
            message(STATUS "[cache] GitHub API unreachable (${_message}); reusing the last known release")
            set(${outVar} "${_cached}" PARENT_SCOPE)
            set(${reasonVar} "" PARENT_SCOPE)
        else()
            set(${reasonVar} "cannot reach the GitHub API (${_message})" PARENT_SCOPE)
        endif()
        return()
    endif()

    file(READ "${_tmp}" _json)
    file(WRITE "${_cacheFile}" "${_json}")
    set(${outVar} "${_json}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Download and stage a prebuilt fastcache-cc for this host.
# @param outVar Set to the staged executable's path, or empty on any failure.
# @param reasonVar Set to a short diagnostic when outVar is empty.
function(_fc_auto_install_fastcache_cc outVar reasonVar)
    set(${outVar} "" PARENT_SCOPE)

    # An empty address is the documented way to opt out of fastcache-cc, so
    # fetching it would quietly override an instruction not to use it. Reported
    # rather than skipped: asking for the fetch and getting nothing, with no word
    # about why, is the silent fall-through the rest of this module avoids.
    if(NOT FASTCACHE_ADDR)
        set(${reasonVar} "FASTCACHE_ADDR is empty, which opts out of fastcache-cc" PARENT_SCOPE)
        return()
    endif()

    _fc_auto_install_select_row(_row)
    if(NOT _row)
        set(${reasonVar}
            "no prebuilt binary is published for ${CMAKE_HOST_SYSTEM_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR}"
            PARENT_SCOPE)
        return()
    endif()

    # A pinned version is an answer in itself and costs no API call, which is
    # also what makes an auto-installing build reproducible.
    set(_json "")
    if(FASTCACHE_AUTO_INSTALL_VERSION)
        set(_version "${FASTCACHE_AUTO_INSTALL_VERSION}")
    else()
        _fc_auto_install_release_json(_json _why)
        if(NOT _json)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
        string(JSON _tag ERROR_VARIABLE _jsonErr GET "${_json}" tag_name)
        if(_jsonErr OR NOT _tag)
            set(${reasonVar} "the GitHub API answered without a release tag" PARENT_SCOPE)
            return()
        endif()
        set(_version "${_tag}")
    endif()

    # Asset names carry a bare numeric triple, so anything else cannot name a
    # file that exists — the same shape cmake/Version.cmake insists on.
    if(NOT _version MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)$")
        set(${reasonVar} "'${_version}' is not a numeric X.Y.Z version" PARENT_SCOPE)
        return()
    endif()
    set(_version "${CMAKE_MATCH_1}")

    # Keyed by platform as well as version. A home directory is not always local
    # to one machine — a shared or synchronised $HOME is normal — and two hosts of
    # different architectures reaching the same staging directory must not be
    # handed each other's binary.
    set(_stagedDir "${FASTCACHE_AUTO_INSTALL_DIR}/${_version}/${_fc_asset_${_row}_platform}")
    set(_staged "${_stagedDir}/${_fc_asset_${_row}_exe}")

    if(NOT EXISTS "${_staged}")
        _fc_auto_install_download("${_row}" "${_version}" "${_json}" "${_stagedDir}" _why)
        if(_why)
            set(${reasonVar} "${_why}" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Asked of a binary just downloaded and of one staged by an earlier run
    # alike. A download can arrive intact and still be unusable here — too old a
    # libc, a truncated file no digest was published to catch — and a binary
    # staged last month can have been made unusable since, by the very sharing
    # the layout above accounts for. Finding out now costs one process; finding
    # out later costs every translation unit.
    execute_process(COMMAND "${_staged}" --version
                    TIMEOUT 10
                    RESULT_VARIABLE _rc
                    OUTPUT_QUIET
                    ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        file(REMOVE "${_staged}")
        set(${reasonVar} "the downloaded launcher does not run here (${_rc})" PARENT_SCOPE)
        return()
    endif()

    set(${outVar} "${_staged}" PARENT_SCOPE)
    set(${reasonVar} "" PARENT_SCOPE)
endfunction()

# Fetch one published archive and stage the launcher out of it.
# @param row Asset-table row id describing this host.
# @param version Release version, a bare numeric triple.
# @param json Release metadata, or empty when the version was pinned.
# @param stagedDir Directory the launcher is placed in.
# @param reasonVar Set to a short diagnostic on failure, empty on success.
function(_fc_auto_install_download row version json stagedDir reasonVar)
    set(${reasonVar} "" PARENT_SCOPE)
    set(_row "${row}")
    set(_version "${version}")
    set(_json "${json}")

    set(_stem "fastcached-${_version}-${_fc_asset_${_row}_platform}")
    set(_assetName "${_stem}.${_fc_asset_${_row}_ext}")
    set(_url "")
    set(_digest "")

    # Prefer the URL and digest the API reported. A pinned version has no
    # metadata to consult, so its URL is composed from the documented layout —
    # which is also the path an internal mirror takes, since pinning a version
    # and pointing FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE elsewhere installs
    # without reaching GitHub at all.
    if(_json)
        string(JSON _assetCount ERROR_VARIABLE _jsonErr LENGTH "${_json}" assets)
        if(_jsonErr OR NOT _assetCount GREATER 0)
            set(${reasonVar} "release ${_version} publishes no assets" PARENT_SCOPE)
            return()
        endif()
        math(EXPR _lastAsset "${_assetCount} - 1")
        foreach(_i RANGE 0 ${_lastAsset})
            string(JSON _name ERROR_VARIABLE _e GET "${_json}" assets ${_i} name)
            if(_e OR NOT _name STREQUAL "${_assetName}")
                continue()
            endif()
            string(JSON _url ERROR_VARIABLE _e GET "${_json}" assets ${_i} browser_download_url)
            if(_e)
                set(_url "")
            endif()
            # Present on releases GitHub has digested; absent on older ones.
            string(JSON _digest ERROR_VARIABLE _e GET "${_json}" assets ${_i} digest)
            if(_e)
                set(_digest "")
            endif()
            break()
        endforeach()
        if(NOT _url)
            set(${reasonVar} "release ${_version} publishes no ${_assetName}" PARENT_SCOPE)
            return()
        endif()
    else()
        set(_url "${FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE}/${FASTCACHE_AUTO_INSTALL_REPO}/releases/download/v${_version}/${_assetName}")
    endif()

    message(STATUS "[cache] Fetching fastcache-cc ${_version} for ${_fc_asset_${_row}_platform}")

    set(_work "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-download")
    set(_archive "${_work}/${_assetName}")
    file(DOWNLOAD "${_url}" "${_archive}"
         STATUS _status
         TIMEOUT 120
         INACTIVITY_TIMEOUT 30)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _status 1 _message)
        file(REMOVE "${_archive}")
        set(${reasonVar} "downloading ${_assetName} failed (${_message})" PARENT_SCOPE)
        return()
    endif()

    if(_digest MATCHES "^sha256:([0-9a-fA-F]+)$")
        file(SHA256 "${_archive}" _actual)
        string(TOLOWER "${CMAKE_MATCH_1}" _expected)
        if(NOT _actual STREQUAL "${_expected}")
            file(REMOVE "${_archive}")
            set(${reasonVar} "${_assetName} failed its SHA256 check" PARENT_SCOPE)
            return()
        endif()
    endif()

    # Unpack beside the archive, inside the build tree, so two build trees
    # unpacking the same release at the same time cannot collide.
    set(_unpack "${_work}/unpack")
    file(REMOVE_RECURSE "${_unpack}")
    file(MAKE_DIRECTORY "${_unpack}")
    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_unpack}")

    set(_member "${_unpack}/${_stem}/${_fc_asset_${_row}_member}")
    if(NOT EXISTS "${_member}")
        set(${reasonVar} "${_assetName} does not contain ${_fc_asset_${_row}_member}" PARENT_SCOPE)
        return()
    endif()

    # Move into place rather than write in place: two configures racing on the
    # same shared staging directory must never expose a half-written binary to
    # the one that arrives second. The temporary name is derived from the build
    # tree, so the two racers cannot pick the same one either.
    string(SHA256 _treeHash "${CMAKE_BINARY_DIR}")
    string(SUBSTRING "${_treeHash}" 0 12 _treeHash)
    set(_pending "${stagedDir}/.staging-${_treeHash}")
    file(MAKE_DIRECTORY "${stagedDir}")
    file(REMOVE "${_pending}")
    file(COPY_FILE "${_member}" "${_pending}" RESULT _copyFailed)
    if(_copyFailed)
        set(${reasonVar} "cannot write to ${stagedDir} (${_copyFailed})" PARENT_SCOPE)
        return()
    endif()
    file(CHMOD "${_pending}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    file(RENAME "${_pending}" "${stagedDir}/${_fc_asset_${_row}_exe}" RESULT _renameFailed)
    if(_renameFailed AND NOT EXISTS "${stagedDir}/${_fc_asset_${_row}_exe}")
        # Losing the race is success; failing to write at all is not.
        set(${reasonVar} "cannot stage the launcher (${_renameFailed})" PARENT_SCOPE)
        return()
    endif()
    file(REMOVE "${_pending}")
endfunction()

# Only when there is nothing else to use. A launcher the user installed is a
# decision already made, and preempting it would be the behaviour change this is
# careful not to be.
if(USE_COMPILER_CACHE
   AND FASTCACHE_AUTO_INSTALL
   AND NOT FASTCACHE_CC
   AND NOT SCCACHE
   AND NOT CCACHE)
    _fc_auto_install_fastcache_cc(_fc_auto_installed _fc_auto_why_not)
    if(_fc_auto_installed)
        message(STATUS "[cache] Auto-installed fastcache-cc (${_fc_auto_installed})")
        set(FASTCACHE_CC "${_fc_auto_installed}" CACHE FILEPATH
            "fastcache-cc tool path; needs a fastcached daemon to be used" FORCE)
    else()
        message(STATUS "[cache] Not auto-installing fastcache-cc: ${_fc_auto_why_not}")
    endif()
endif()

# Ask fastcache-cc itself whether the cache works, by compiling one tiny
# translation unit through it with FASTCACHE_VERBOSE=1 and requiring a reported
# cache outcome. A launcher that cannot reach its daemon still compiles fine —
# it just runs the real compiler — so nothing but an end-to-end exchange tells
# "the cache works" apart from "every TU will silently pay a failed connect,
# with precompiled headers disabled for nothing and ccache passed over".
#
# The match is positive (HIT/MISS only): should the launcher's diagnostics ever
# be reworded, this reports unusable and the build falls back to the next
# launcher, which is the harmless direction to be wrong in.
#
# @param outVar Set to TRUE when the cache served the probe, FALSE otherwise.
# @param reasonVar Set to a short diagnostic when outVar is FALSE.
function(_fc_probe_fastcache_cc outVar reasonVar)
    set(${outVar} FALSE PARENT_SCOPE)

    set(_dir "${CMAKE_BINARY_DIR}/CMakeFiles/fastcache-probe")
    set(_src "${_dir}/probe.cpp")
    # Both the file and its content are fixed, so the probe itself is a cache
    # hit from the second configure onwards — which exercises FETCH rather than
    # just STORE, and costs less than the first run.
    file(WRITE "${_src}" "int fastcacheProbe() { return 0; }\n")

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(_args /nologo /c "${_src}" "/Fo${_dir}/probe.obj")
    else()
        set(_args -c "${_src}" -o "${_dir}/probe.o")
    endif()

    # A probe that answers takes ~0.1s locally and little more over a LAN, so ten
    # seconds is generous for a working daemon and a bounded wait for a broken
    # one. The cap has to live here: FASTCACHE_TIMEOUT_MS bounds the launcher's
    # send/recv but not its connect(), so an address that drops packets rather
    # than refusing them — a firewall, a downed VPN, a host that is simply gone —
    # stalls on the TCP connect timeout instead (measured: 2m30s), and every
    # configure would pay it.
    set(_timeoutSeconds 10)

    # NO_STATS keeps the probe out of `fastcache-cc --show-stats`, where it would
    # read as a build that never hits. TIMEOUT_MS bounds a daemon that accepts
    # the connection and then stalls; builds keep the launcher's own default.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
                ${_fc_fastcache_env}
                "FASTCACHE_VERBOSE=1"
                "FASTCACHE_NO_STATS=1"
                "FASTCACHE_TIMEOUT_MS=2000"
                "${FASTCACHE_CC}" "${CMAKE_CXX_COMPILER}" ${_args}
        WORKING_DIRECTORY "${_dir}"
        TIMEOUT ${_timeoutSeconds}
        RESULT_VARIABLE _rc
        OUTPUT_QUIET
        ERROR_VARIABLE _err)

    if(_rc MATCHES "[Tt]imeout")
        set(${reasonVar} "no answer within ${_timeoutSeconds}s" PARENT_SCOPE)
    elseif(NOT _rc EQUAL 0)
        set(${reasonVar} "probe compile failed (${_rc})" PARENT_SCOPE)
    elseif(_err MATCHES "fastcache-cc: (HIT|MISS) key=")
        set(${outVar} TRUE PARENT_SCOPE)
        set(${reasonVar} "" PARENT_SCOPE)
    elseif(_err MATCHES "fastcache-cc: cache unavailable \\(([^)]*)\\)")
        set(${reasonVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${reasonVar} "no cache outcome reported" PARENT_SCOPE)
    endif()
endfunction()

# Candidate table, most-preferred first. Each row <id> is described by:
#   _fc_cache_<id>_label     human-readable name for the status message
#   _fc_cache_<id>_program   the found program (empty when not installed)
#   _fc_cache_<id>_requires  extra condition; the row is skipped when falsy
#   _fc_cache_<id>_env       NAME=VALUE pairs to inject around the invocation
#   _fc_cache_<id>_check     function deciding usability at configure time
#                            (empty when being installed is enough); called as
#                            <fn>(<outVar> <reasonVar>) and only for a row that
#                            already passed program and requires
#   _fc_cache_<id>_detail    extra words for the status message (empty for none)
# Supporting a fourth launcher is adding an id here plus its six variables.
set(_fc_cache_candidates fastcache_cc sccache ccache)

# Render "<label>[ <detail>]" for a row, so a launcher and where it points are
# named the same way whether it won or was passed over. Diagnosing a daemon that
# did not answer starts with knowing which address was tried.
# @param id Row id from _fc_cache_candidates.
# @param outVar Receives the rendered text.
function(_fc_cache_describe id outVar)
    set(_text "${_fc_cache_${id}_label}")
    if(_fc_cache_${id}_detail)
        string(APPEND _text " ${_fc_cache_${id}_detail}")
    endif()
    set(${outVar} "${_text}" PARENT_SCOPE)
endfunction()

set(_fc_cache_fastcache_cc_label "fastcache-cc")
set(_fc_cache_fastcache_cc_program "${FASTCACHE_CC}")
set(_fc_cache_fastcache_cc_requires "${FASTCACHE_ADDR}")
set(_fc_cache_fastcache_cc_env ${_fc_fastcache_env})
set(_fc_cache_fastcache_cc_check _fc_probe_fastcache_cc)
set(_fc_cache_fastcache_cc_detail "at ${FASTCACHE_ADDR}")

set(_fc_cache_sccache_label "sccache")
set(_fc_cache_sccache_program "${SCCACHE}")
set(_fc_cache_sccache_requires ON)
set(_fc_cache_sccache_env "")
set(_fc_cache_sccache_check "")
set(_fc_cache_sccache_detail "")

set(_fc_cache_ccache_label "ccache")
set(_fc_cache_ccache_program "${CCACHE}")
set(_fc_cache_ccache_requires ON)
set(_fc_cache_ccache_env "")
set(_fc_cache_ccache_check "")
set(_fc_cache_ccache_detail "")

set(_fc_cache_chosen "")
set(_fc_cache_rejected "")
if(USE_COMPILER_CACHE)
    foreach(_id IN LISTS _fc_cache_candidates)
        if(NOT _fc_cache_${_id}_program OR NOT _fc_cache_${_id}_requires)
            continue()
        endif()
        if(_fc_cache_${_id}_check)
            cmake_language(CALL ${_fc_cache_${_id}_check} _fc_cache_usable _fc_cache_why_not)
            if(NOT _fc_cache_usable)
                # Remember why, so a fall-through to a slower launcher explains
                # itself rather than looking like the faster one was never there.
                _fc_cache_describe("${_id}" _fc_cache_desc)
                list(APPEND _fc_cache_rejected "${_fc_cache_desc}: ${_fc_cache_why_not}")
                continue()
            endif()
        endif()
        set(_fc_cache_chosen "${_id}")
        break()
    endforeach()
endif()

# Say why a preferred launcher was passed over, whatever the outcome: falling
# through in silence looks exactly like it never being installed.
foreach(_rejection IN LISTS _fc_cache_rejected)
    message(STATUS "[cache] Not using ${_rejection}")
endforeach()

if(_fc_cache_chosen)
    set(_fc_cache_program "${_fc_cache_${_fc_cache_chosen}_program}")

    # `cmake -E env NAME=VALUE ... <program>` is the only way to attach
    # environment to a compiler launcher; rows without env invoke the program
    # directly so they pay no extra process.
    if(_fc_cache_${_fc_cache_chosen}_env)
        set(_fc_cache_launcher
            "${CMAKE_COMMAND}" -E env
            ${_fc_cache_${_fc_cache_chosen}_env}
            "${_fc_cache_program}")
    else()
        set(_fc_cache_launcher "${_fc_cache_program}")
    endif()

    _fc_cache_describe("${_fc_cache_chosen}" _fc_cache_desc)
    message(STATUS "[cache] Enabling ${_fc_cache_desc} (${_fc_cache_program}) for C/C++ compilation")
    set(CMAKE_C_COMPILER_LAUNCHER ${_fc_cache_launcher})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${_fc_cache_launcher})

    # None of the launchers reproduces anything but the object file on a cache
    # hit, so a precompiled header (a second, separately produced artefact)
    # cannot be served from cache.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

    # CMake's C++20 module support puts scanning flags on every compile line
    # (-fmodules-ts -fmodule-mapper=<per-object modmap> on GCC). A preprocess-only
    # run with those flags fails, so a launcher that derives its key by
    # preprocessing falls back on *every* translation unit. This project has no
    # module units, so the scan is pure overhead; turn it off while a launcher is
    # in use.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    # For the same reason none of them supports /Zi (shared PDB). Force MSVC to
    # embed debug info in .obj files (/Z7) via the modern CMake knob (CMP0141),
    # and also fix up any legacy /Zi already present in FLAGS_DEBUG /
    # FLAGS_RELWITHDEBINFO.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        set(CMAKE_POLICY_DEFAULT_CMP0141 NEW)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
        foreach(_var
                CMAKE_CXX_FLAGS_DEBUG
                CMAKE_C_FLAGS_DEBUG
                CMAKE_CXX_FLAGS_RELWITHDEBINFO
                CMAKE_C_FLAGS_RELWITHDEBINFO)
            string(REGEX REPLACE "([-/])Zi" "\\1Z7" ${_var} "${${_var}}")
        endforeach()
    endif()
else()
    # Define the launchers as empty rather than leaving them unset. Fetched
    # dependencies bring their own cache modules that auto-enable ccache when the
    # launcher is merely *undefined* (libunicode's cmake/EnableCcache.cmake does
    # exactly that), which would quietly re-enable caching for their targets.
    # An empty definition is inert for us and keeps USE_COMPILER_CACHE=OFF honest.
    set(CMAKE_C_COMPILER_LAUNCHER "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "")

    if(NOT USE_COMPILER_CACHE)
        message(STATUS "[cache] Compiler caching disabled by USE_COMPILER_CACHE=OFF")
    elseif(_fc_cache_rejected)
        message(STATUS "[cache] No other compiler-cache launcher found (sccache, ccache); caching disabled "
                       "(start a daemon with `fastcached` to cache through fastcache-cc)")
    else()
        message(STATUS "[cache] No compiler-cache launcher found (fastcache-cc, sccache, ccache); caching disabled")
    endif()
endif()
