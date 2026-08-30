# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

# cmake/portable holds the modules written to be copied into other projects
# verbatim — they depend on nothing else here. cmake/ holds the ones that only
# make sense while building this. Both are on the module path; the split says
# which is which, and cmake/portable/README.md is the note for anyone vendoring
# one.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/portable")

include(Version)
GetVersionInformation(FASTCACHED_VERSION FASTCACHED_VERSION_STRING)

# The oldest macOS the shipped binaries must run on. This has to precede
# project(): CMake bakes the deployment target into the compile lines while
# configuring the toolchain, so a value set afterwards is silently ignored and
# every binary is stamped with the *builder's* OS version instead. A package
# built that way installs fine and then refuses to launch on anything older —
# `minos 26.0` on a machine whose users are on 13.x.
#
# 13.3 and not 13.0: std::format of a floating-point value calls std::to_chars,
# whose floating-point overloads only appear in the libc++ dylib shipped with
# macOS 13.3. Targeting anything older fails to compile outright with
# "'to_chars' is unavailable: introduced in macOS 13.3" rather than degrading
# gracefully, so this is a floor imposed by the standard library, not a choice.
#
# Only a default: an explicit -DCMAKE_OSX_DEPLOYMENT_TARGET still wins, and so
# does the MACOSX_DEPLOYMENT_TARGET environment variable. The environment check
# has to be explicit: CMake initializes CMAKE_OSX_DEPLOYMENT_TARGET from that
# variable inside project(), so seeding the cache beforehand would pre-empt it
# and silently drop a target the caller had every reason to think was honoured.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum supported macOS version")
endif()

project(fastcached
    VERSION "${FASTCACHED_VERSION}"
    DESCRIPTION "Fast Cache Daemon — platform-independent memcached/Redis server"
    LANGUAGES CXX
)

include(GNUInstallDirs)

# Lay the install tree out for a distro package: the payload is rooted at /
# instead of /usr, because the systemd units and the /etc config sit at fixed
# absolute locations that cannot be rebased onto a /usr prefix (systemd does
# not read config from /usr/etc). See cmake/Packaging.cmake.
#
# The same reasoning applies to the macOS .pkg: `pkgbuild --install-location` is
# hardcoded to / by CPack, and the launchd plists and /etc/paths.d entry sit at
# fixed absolute locations outside any prefix. Note that this is why the prefix
# must stay `/` there too — pointing CPACK_PACKAGING_INSTALL_PREFIX at
# /opt/fastcached would work for the binaries, but an absolute install
# DESTINATION for /Library/LaunchDaemons then escapes the staging tree and
# writes to the build host's real filesystem.
#
# This is OFF by default, including on Linux, so that the ordinary
# `cmake --install --prefix /usr/local` keeps putting the daemon at
# /usr/local/bin/fastcached. Defaulting it ON rebased every developer install
# onto <prefix>/usr/bin, which is on nobody's PATH — a successful install that
# left `fastcached` reporting "command not found".
#
# Turn it ON only to build packages:
#   cmake -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON ... && cpack -G "DEB;RPM"
# The CI packaging job does exactly that; see .github/workflows/build.yml.
option(FASTCACHED_PACKAGE_ROOT_PREFIX "Lay out the install tree for .deb/.rpm/.pkg packaging" OFF)

# The macOS package prefix. Single source of truth: it is spelled into the
# install destinations below, into /etc/paths.d/fastcached, into the launchd
# plists, and into the uninstaller. macOS has no FHS to follow and /usr/local is
# shared with Homebrew, so a private /opt subtree keeps the payload trivially
# enumerable — which is what makes an uninstaller possible at all, given that a
# .pkg carries no removal mechanism of its own.
set(FASTCACHED_MACOS_PREFIX "opt/fastcached"
    CACHE STRING "macOS package install prefix, relative to /")

# macOS package identity. Declared here rather than in packaging/ because both
# packaging/CMakeLists.txt (a subdirectory, which inherits this scope) and
# cmake/Packaging.cmake (include()d at this scope) read them. A plain set() in
# the subdirectory does not propagate back up, which left
# CPACK_PRODUCTBUILD_IDENTIFIER empty: receipts then registered under CPack's
# fallback identifier and the uninstaller's `pkgutil --pkgs | grep` matched
# nothing, so macOS believed fastcached was still installed forever.
#
# The bundle id must match LaunchdLabelPrefix + the lowercased default service
# name in src/FastCache/Platform/ServiceControl.cpp: the daemon derives its own
# launchd label from its --service-name, and the installer scripts look the job
# up by that label. ServiceControl_test.cpp pins the derivation.
#
# Never change the bundle id once released: macOS keys package receipts and
# upgrade detection on it, the same discipline as CPACK_WIX_UPGRADE_GUID.
set(FASTCACHED_MACOS_BUNDLE_ID       "software.lastrada.fastcached")
set(FASTCACHED_MACOS_SERVICE_ACCOUNT "_fastcached")

# The compile worker's account, and deliberately NOT the daemon's above: a worker
# runs a compiler on input that arrived over the network while fastcached owns the
# cache storage, so one shared account would let a compromised compile rewrite
# every cached object -- the reason packaging/linux/fastcache-compile-node.sysusers
# declares its own too.
#
# Spelled exactly as that sysusers file and as MakeNodeServiceSpec's
# `.serviceAccount` spell it, rather than following the macOS `_`-prefix
# convention `_fastcached` does. The prefix's job is to keep the account out of
# the login window, which `IsHidden 1` already does; what it would cost is a third
# spelling of one name, across a binary, a package and a unit that never see each
# other. `ctest -R service-accounts` fails when they drift apart, on every
# platform -- because the failure mode is macOS-only and silent (issue #87).
set(FASTCACHED_MACOS_NODE_ACCOUNT    "fastcache-node")

# The worker's launchd label, on the same terms as FASTCACHED_MACOS_BUNDLE_ID
# above: LaunchdLabelPrefix + the lowercased NodeConfig::serviceName default.
# The uninstaller needs it because it removes the binary that would otherwise
# answer for itself, and a job left bootstrapped against a deleted executable is
# one nothing can reach afterwards. NodeConfig_test.cpp pins the derivation.
#
# NOT a package identifier: the installer registers no worker job (it has no
# scheduler or toolchain to register one with), so no receipt is ever filed under
# this name. It names only a job an operator installed by hand.
set(FASTCACHED_MACOS_NODE_LABEL      "software.lastrada.fastcachecompilenode")

# The tools symlinked into /usr/local/bin. /etc/paths.d only takes effect for
# LOGIN shells, so these are what make the tools reachable from an already-open
# terminal, from `ssh host cmd`, and from fish -- which never reads /etc/profile.
#
# One list, substituted into both the postinstall that creates the links and the
# uninstaller that removes them. They were two hand-written lists, and the file's
# own comment records what that cost: "The uninstaller has the mirror of this
# guard; this is the half that went missing." A tool added to one and not the
# other is either a link nothing removes or a link nothing creates.
set(FASTCACHED_MACOS_LINKED_TOOLS "fastcached fastcache-cc fastcache-compile-node")

# The machine-wide configuration directory. Declared here, not in packaging/,
# for the same scoping reason as the macOS variables above: packaging/ installs
# the file into it and src/FastCache/ compiles the very same path into the
# daemon's default lookup, and two sibling subdirectories cannot see each
# other's set(). Spelled without a leading slash to match the other install
# destinations, which are relative to CMAKE_INSTALL_PREFIX.
set(FASTCACHED_SYSCONF_DIR "etc/fastcached"
    CACHE STRING "System configuration directory, relative to /")

# Install destinations for the relocatable artifacts (binaries, docs). With the
# prefix moved to /, the ordinary FHS paths have to spell `usr/` themselves, or
# the binaries would land in /bin and the docs in /share/doc. Outside packaging
# these stay the plain GNUInstallDirs values.
if(FASTCACHED_PACKAGE_ROOT_PREFIX)
    if(APPLE)
        set(FASTCACHED_INSTALL_BINDIR "${FASTCACHED_MACOS_PREFIX}/bin")
        set(FASTCACHED_INSTALL_DOCDIR "${FASTCACHED_MACOS_PREFIX}/share/doc/fastcached")
    else()
        set(FASTCACHED_INSTALL_BINDIR "usr/${CMAKE_INSTALL_BINDIR}")
        set(FASTCACHED_INSTALL_DOCDIR "usr/${CMAKE_INSTALL_DOCDIR}")
    endif()
else()
    set(FASTCACHED_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(FASTCACHED_INSTALL_DOCDIR "${CMAKE_INSTALL_DOCDIR}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# ----------------------------------------------------------------------------
# Project-wide options

option(FASTCACHED_BUILD_TESTS "Build unit tests" ON)

# USE_COMPILER_CACHE is declared by cmake/portable/CompileCache.cmake, next to the logic
# that reads it: a module that consumed an option it did not declare would read
# an undefined variable as "off" and silently disable caching if it were ever
# included on its own.

# TLS is opt-in and off by default so the standard build stays dependency-free
# (mirroring TRACY_ENABLE). When on, the build links OpenSSL and our code keys
# off the FC_TLS_ENABLED compile definition; when off, no OpenSSL header is
# included and the TLS sources are not compiled. On Windows the OpenSSL package
# is pulled in via the vcpkg manifest's "tls" feature (see vcpkg.json).
option(FASTCACHED_ENABLE_TLS "Enable TLS support (requires OpenSSL)" OFF)

# On-disk value compression is ON by default: the standard build ships zstd +
# lz4 and the runtime default codec is zstd (admins opt out with
# `--compression none`). When explicitly built OFF, no compression library is
# fetched or linked, the codec table degrades to identity-only, and selecting
# zstd/lz4 from config becomes a clean startup error. Our code keys off the
# FC_COMPRESSION_ENABLED compile definition (set in src/FastCache/CMakeLists.txt).
option(FASTCACHED_ENABLE_COMPRESSION "Enable on-disk value compression (zstd + lz4)" ON)

# Tracy profiler. TRACY_ENABLE is Tracy's own gate; reusing the name means the
# fetched client library actually emits instrumentation rather than its no-op
# stub. Our code keys off the separate FC_TRACY_ENABLED compile definition
# (set below) so Core/Profiling.hpp stays decoupled from Tracy's macro surface
# and pulls in no Tracy header when the profiler is off.
option(TRACY_ENABLE "Enable Tracy profiler instrumentation (fetches & links the Tracy client)" OFF)
option(TRACY_ON_DEMAND "Tracy: only collect data while a profiler is connected" ON)

# ----------------------------------------------------------------------------
# Compiler caching. Loaded before any compiler invocation so that CPM-fetched
# dependencies also get cached. CompileCache.cmake prefers our own fastcache-cc
# launcher (when installed and a daemon answers, at 127.0.0.1:6674 unless
# FASTCACHE_ADDR says otherwise), falls back to sccache and then ccache, and
# silently no-ops when none is usable or USE_COMPILER_CACHE=OFF. It also declares
# the USE_COMPILER_CACHE option.

include(CompileCache)

# Default install/runtime/library output directories.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()

# ----------------------------------------------------------------------------
# Dependencies via CPM (tries find_package first).
#
# Fetched BEFORE the pedantic/sanitizer/clang-tidy includes so the global
# add_compile_options() / CMAKE_CXX_CLANG_TIDY they install only apply to our
# own targets (defined later in this directory), not to third-party code where
# -Werror -Wconversion is not our problem to fix.

set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Prefer system-installed packages over CPM downloads")
include(cmake/CPM.cmake)

# yaml-cpp 0.8.0 declares cmake_minimum_required(VERSION 3.4) which CMake 4.x
# refuses outright. Inject the policy-version compatibility shim so the fetch
# configures on modern toolchains. This is scoped to yaml-cpp only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
CPMAddPackage(
    NAME yaml-cpp
    GITHUB_REPOSITORY jbeder/yaml-cpp
    GIT_TAG 0.8.0
    OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_BUILD_SHARED_LIBS OFF"
        "YAML_CPP_INSTALL OFF"
)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

if(FASTCACHED_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.6.0
    )

    # Locate the directory shipping Catch.cmake (providing catch_discover_tests)
    # and put it on CMAKE_MODULE_PATH so every test subdirectory can simply
    # include(Catch). Catch2 reaches us by one of two routes:
    #   * CPM source build  -> helpers live in ${Catch2_SOURCE_DIR}/extras
    #   * system find_package -> helpers live next to Catch2Config.cmake,
    #                            i.e. in ${Catch2_DIR}
    # CPM_USE_LOCAL_PACKAGES makes the latter the default whenever a system
    # Catch2 is installed, so both routes must be handled.
    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    elseif(Catch2_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    endif()
endif()

# Tracy is third-party, so (like yaml-cpp/Catch2) it is fetched BEFORE the
# pedantic/clang-tidy includes below — the global -Werror and clang-tidy hooks
# must not apply to its sources. We forward TRACY_ENABLE/TRACY_ON_DEMAND so the
# client emits instrumentation; TRACY_ON_DEMAND keeps a long-running daemon from
# buffering events (and growing memory) until a profiler actually connects.
if(TRACY_ENABLE)
    CPMAddPackage(
        NAME tracy
        GITHUB_REPOSITORY wolfpld/tracy
        GIT_TAG v0.11.1
        OPTIONS
            "TRACY_ENABLE ON"
            "TRACY_ON_DEMAND ${TRACY_ON_DEMAND}"
            "TRACY_STATIC ON"
    )
endif()

# OpenSSL for the optional TLS support. Located before the pedantic/clang-tidy
# includes so its imported targets carry no -Werror expectations of ours. The
# REQUIRED here means a misconfigured TLS build fails fast at configure time
# rather than at link time.
if(FASTCACHED_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

# zstd + lz4 for on-disk value compression. Fetched (like yaml-cpp/Tracy)
# BEFORE the pedantic/clang-tidy includes so the global -Werror -Wconversion
# and clang-tidy hooks never apply to their third-party sources. CPM tries a
# system package first (CPM_USE_LOCAL_PACKAGES), otherwise fetches the source.
# Both build static, programs/tests off, to keep the dependency footprint small.
#
# EXCLUDE_FROM_ALL is load-bearing, not tidiness: a fetched zstd brings its own
# install() rules, and CMake honours those for every `cmake --install` of this
# project. With the package payload rooted at / that put zstd.h, libzstd.a and a
# zstdConfig.cmake at /include, /lib and /lib/cmake — third-party developer
# files, installed system-wide, from a package that ships two executables.
# add_subdirectory(EXCLUDE_FROM_ALL) drops those rules; the static libraries are
# still built on demand because our targets link them.
if(FASTCACHED_ENABLE_COMPRESSION)
    CPMAddPackage(
        NAME zstd
        GITHUB_REPOSITORY facebook/zstd
        VERSION 1.5.6
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "ZSTD_BUILD_PROGRAMS OFF"
            "ZSTD_BUILD_TESTS OFF"
            "ZSTD_BUILD_SHARED OFF"
            "ZSTD_BUILD_STATIC ON"
    )
    CPMAddPackage(
        NAME lz4
        GITHUB_REPOSITORY lz4/lz4
        VERSION 1.10.0
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL YES
        OPTIONS
            "LZ4_BUILD_CLI OFF"
            "LZ4_BUILD_LEGACY_LZ4C OFF"
            "BUILD_SHARED_LIBS OFF"
            "BUILD_STATIC_LIBS ON"
    )
endif()

# ----------------------------------------------------------------------------
# Toolchain helpers (warning levels, sanitizers, coverage, clang-tidy).
#
# Coverage belongs here, next to Sanitizers, and not one line earlier: both add
# their instrumentation with directory-scoped add_compile_options(), which
# reaches targets defined AFTER it. Every CPM dependency has been added by this
# point, so yaml-cpp, Catch2 and Tracy are left uninstrumented rather than
# instrumented and then filtered back out of the report.

include(PedanticCompiler)
include(Sanitizers)
include(Coverage)
include(ClangTidy)
include(Utf8CodePage)

# ----------------------------------------------------------------------------
# Test wiring is enabled early so all subdirectories (CowTree, FastCache,
# src/tests) can register their CTest test executables via
# catch_discover_tests().

if(FASTCACHED_BUILD_TESTS)
    enable_testing()
endif()

# ----------------------------------------------------------------------------
# Sibling libraries (no dependency on FastCache).

add_subdirectory(src/CowTree)

# ----------------------------------------------------------------------------
# Library target: all FastCache code lives here (so both the executable and
# the test binary link it).

add_library(FastCache STATIC)
target_include_directories(FastCache PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(FastCache PUBLIC cxx_std_23)
target_link_libraries(FastCache PUBLIC yaml-cpp::yaml-cpp)
target_link_libraries(FastCache PUBLIC CowTree)

# Tracy is linked PUBLIC so FC_TRACY_ENABLED and the Tracy include dirs
# propagate transitively to the fastcached executable (thread naming in
# main.cpp) and to the FastCacheTest binary (so Profiling_test sees the same
# mode). When TRACY_ENABLE is OFF nothing is linked or defined and every
# Core/Profiling.hpp macro compiles to (void)0 — a strictly zero-cost build.
if(TRACY_ENABLE)
    target_link_libraries(FastCache PUBLIC Tracy::TracyClient)
    target_compile_definitions(FastCache PUBLIC FC_TRACY_ENABLED=1)
endif()

# Substitute the version triple/string into Core/Version.hpp at configure
# time. The generated header lives under the build dir so it never gets
# checked in.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FastCache/Core/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/FastCache/Core/Version.hpp"
    @ONLY
)

if(WIN32)
    # iphlpapi: GetAdaptersAddresses, behind Platform/LocalAddresses. PUBLIC for the
    # same reason ws2_32 is -- every executable that links FastCache asks the cache
    # surface who a caller is, and a private link would leave that unresolved in each
    # of them.
    target_link_libraries(FastCache PUBLIC ws2_32 iphlpapi)
    target_compile_definitions(FastCache PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

add_subdirectory(src/FastCache)

# ----------------------------------------------------------------------------
# Executables. Every binary lives under src/apps/<name>/ and declares its own
# target and install rule there; src/apps/CMakeLists.txt holds the app table
# that gates each one. The shipped binaries are fastcached and fastcache-cc;
# compile-cache-testclient is test infrastructure and is off by default.

add_subdirectory(src/apps)

# ----------------------------------------------------------------------------
# Installation of the project docs, under the standard GNUInstallDirs prefix
# (share/doc/fastcached/). Each executable installs itself into bin/ from its
# own subdirectory. The FastCache/CowTree static libraries and headers are
# intentionally not installed: they PUBLIC-link an un-installed CPM yaml-cpp,
# so exporting a consumable SDK is out of scope.

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${FASTCACHED_INSTALL_DOCDIR}"
    COMPONENT Runtime
)

# ----------------------------------------------------------------------------
# Service-integration assets (systemd units, sysusers/tmpfiles declarations,
# the example config, the WiX service fragment) and the CPack configuration
# that wraps everything installed above into .deb / .rpm / .msi.
#
# Order matters twice over: packaging/ exports the variables Packaging.cmake
# consumes, and CPack only sees install() rules declared before include(CPack)
# — so this stays at the end, after every install() in the project.

add_subdirectory(packaging)
include(Packaging)

# ----------------------------------------------------------------------------
# Tests.

if(FASTCACHED_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()

# ----------------------------------------------------------------------------
# The UTF-8 process code page every Windows executable declares. Here rather than
# in each add_executable() site, and here rather than earlier, for the reason the
# coverage block below states: it walks the build system as it stands, so a target
# defined after this call is a binary that keeps the host's legacy code page.
# cmake/Utf8CodePage.cmake carries why the code page is what gets declared.

fastcached_declare_utf8_code_page()

# ----------------------------------------------------------------------------
# The `coverage` / `coverage-clean` targets, when ENABLE_COVERAGE is on. Last in
# the file on purpose: the target hands llvm-cov every executable defined under
# src/, and it discovers them by walking the build system as it stands, so a
# subdirectory added after this call is a binary left out of the report.

if(ENABLE_COVERAGE)
    fastcached_add_coverage_targets()
endif()
