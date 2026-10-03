# SPDX-License-Identifier: Apache-2.0
#
# Exactly ONE first-party file opens a UDP socket: discovery's, in
# `src/apps/fastcache-compile-node/DiscoveryTier.cpp`.
#
# ## Why this is a rule
#
# `--install-service` gives the compile node a Windows Firewall rule for discovery's reply socket.
# Unless `--discovery-reply-port` pins it, the kernel chooses that socket's port at every start, so
# no port-scoped rule can name it, and the rule admits EVERY local UDP port (`... discovery-reply
# udp/any`), scoped to the program and its service. What that rule exposes is therefore every UDP
# socket this program opens. The documentation, the rulebook and `NodeFirewallRules` all say that is
# "discovery alone" -- true only while discovery is the only UDP socket there is. A second one would
# be reachable from the network through a rule written for somebody else, and nothing about a green
# build would say so.
#
# ## What is scanned, and what counts as opening a UDP socket
#
# First-party C++, DERIVED rather than listed: `fastcached_first_party_cxx`, which is every tracked
# C++ source (`git ls-files`, or a directory walk where there is no index) minus the third-party
# roots. Tests are not scanned -- `*_test.cpp` and `src/tests/` open sockets of their own to test
# with, and no service registers them -- and neither is anything declined as third-party.
#
# An opener is a USE, in code with comments stripped (`fastcached_scan_code_lines`), of one of the
# primitives below: core-cpp's three UDP factories, or a raw `SOCK_DGRAM` / `IPPROTO_UDP` for a
# socket made without them. Each row carries its reason, and a needle nobody can argue with is a
# needle nobody reads.
#
# ## The permitted row is the positive control
#
# The permitted file must EXIST, be ENUMERATED, and be SEEN opening a UDP socket. A row that has
# stopped doing any of the three is refused as stale: that is what a renamed primitive or a moved
# opener looks like, after which this check would go on passing while guarding nothing.
#
# NOT covered, and the direction it fails in is PERMISSIVE: a UDP socket opened through a primitive
# this table does not name (a factory core-cpp adds later, or a platform call spelling neither
# `SOCK_DGRAM` nor `IPPROTO_UDP`), or through a macro. And **sockets a dependency opens inside
# this process are not scanned**: only first-party source is read, so a UDP socket core-cpp, a
# resolver, or a TLS or telemetry library opens on the program's behalf is exposed by the any-port
# rule exactly as a first-party one would be, and this check says nothing about it -- SILENT, and
# therefore OPEN. What is known about core-cpp itself, read from the pinned source rather than
# assumed: its datagram socket is created in `detail::makeDatagramSocket`, reached only from the
# open path behind `openUdpSocket` (and `detail::openUdpSocketWithReceiveBuffer`, and
# `openSharedPortUdpSocket`, which calls `openUdpSocket` twice) -- every one of which this scan
# catches at the first-party call site. A core-cpp that later opens a UDP socket of its own, for
# a resolver say, would not be seen here.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports failure
# through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> [-DGIT_EXECUTABLE=<git>] -P scripts/check-udp-opener.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The one file allowed to open a UDP socket, and why. One row, and adding a second is a decision
# about the firewall rule rather than about this check.
set(FastCachedUdpOpenerUnits
    "src/apps/fastcache-compile-node/DiscoveryTier.cpp|discovery's shared beacon socket and its private reply socket, the sockets the program-scoped udp/any firewall rule exists for"
)

# What counts as opening a UDP socket.
set(FastCachedUdpOpenerPrimitives
    "openSharedPortUdpSocket|core-cpp's shared-port beacon and reply pair"
    "openUdpSocket|core-cpp's plain UDP socket, and the prefix of its receive-buffer variant"
    "SOCK_DGRAM|a raw datagram socket made without core-cpp"
    "IPPROTO_UDP|a raw UDP socket named by its protocol"
)

set(permittedPaths "")
foreach(row IN LISTS FastCachedUdpOpenerUnits)
    fastcached_row_fields("${row}" permittedPath permittedReason)
    string(STRIP "${permittedReason}" permittedReason)
    if(permittedReason STREQUAL "")
        message(FATAL_ERROR
            "udp-opener: the permitted row for ${permittedPath} gives no reason. A file allowed to open a UDP "
            "socket is a decision about a firewall rule, and without its reason it reads like one somebody forgot.")
    endif()
    list(APPEND permittedPaths "${permittedPath}")
    set("seenOpener_${permittedPath}" FALSE)
endforeach()

set(primitiveNames "")
foreach(row IN LISTS FastCachedUdpOpenerPrimitives)
    fastcached_row_fields("${row}" primitiveName primitiveReason)
    string(STRIP "${primitiveReason}" primitiveReason)
    if(primitiveReason STREQUAL "")
        message(FATAL_ERROR "udp-opener: the primitive `${primitiveName}` gives no reason")
    endif()
    list(APPEND primitiveNames "${primitiveName}")
endforeach()
list(JOIN primitiveNames "|" primitiveAlternation)
set(openerPattern "(^|[^A-Za-z0-9_])(${primitiveAlternation})")

fastcached_first_party_cxx("${FASTCACHED_SOURCE_DIR}" sourceFiles declinedFiles scanSource)
list(LENGTH declinedFiles declinedCount)
if(NOT sourceFiles)
    message(FATAL_ERROR
        "udp-opener: the scan (${scanSource}) matched no first-party C++ source at all. That is not a clean "
        "tree but a scan that stopped working -- a moved source root, or a FASTCACHED_SOURCE_DIR pointing "
        "elsewhere.")
endif()

set(violations "")
set(enumeratedPermitted "")
set(scannedCount 0)
foreach(relative IN LISTS sourceFiles)
    if(relative MATCHES "_test\\.cpp$" OR relative MATCHES "^src/tests/")
        continue()
    endif()
    math(EXPR scannedCount "${scannedCount} + 1")
    if(relative IN_LIST permittedPaths)
        list(APPEND enumeratedPermitted "${relative}")
    endif()
    # The index can name a file deleted from the work tree and not yet staged.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    # A whole-file test first; the line walk is the costly part and almost no file needs it.
    if(NOT wholeFile MATCHES "${openerPattern}")
        continue()
    endif()
    fastcached_scan_code_lines("${wholeFile}" "${openerPattern}" hits)
    foreach(hit IN LISTS hits)
        if(NOT hit MATCHES "^use:([0-9]+)$")
            continue()
        endif()
        if(relative IN_LIST permittedPaths)
            set("seenOpener_${relative}" TRUE)
        else()
            list(APPEND violations "${relative}:${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

set(stale "")
foreach(permittedPath IN LISTS permittedPaths)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${permittedPath}")
        list(APPEND stale "${permittedPath}: does not exist")
    elseif(NOT permittedPath IN_LIST enumeratedPermitted)
        list(APPEND stale "${permittedPath}: exists but the enumeration (${scanSource}) did not list it, so nothing says the rest of the tree was listed either")
    elseif(NOT seenOpener_${permittedPath})
        list(APPEND stale "${permittedPath}: was read and opens no UDP socket -- the row excuses nothing, or a primitive has been renamed and this scan is guarding a name nobody calls")
    endif()
endforeach()

if(violations OR stale)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}: opens a UDP socket outside src/apps/fastcache-compile-node/DiscoveryTier.cpp")
    endforeach()
    foreach(entry IN LISTS stale)
        message("  STALE permitted row -- ${entry}")
    endforeach()
    message("")
    message("A second UDP socket in this program would be EXPOSED by the compile node's any-port firewall")
    message("rule. Without --discovery-reply-port, --install-service opens `<service> discovery-reply udp/any`:")
    message("inbound UDP on every local port, scoped to the program and its service, because the kernel")
    message("chooses the reply socket's port. That is safe only while discovery is the only UDP socket the")
    message("program opens, and the docs and the rulebook say \"discovery alone\".")
    message("")
    message("For a violation: open the socket in DiscoveryTier.cpp if it is discovery's. Otherwise the")
    message("any-port rule has to stop covering it -- pin the reply port (--discovery-reply-port) so the")
    message("rule becomes one port, or scope the rule some other way -- and only then add a permitted row")
    message("to scripts/check-udp-opener.cmake and correct every \"discovery alone\" sentence.")
    message("")
    message("A STALE row means DiscoveryTier.cpp stopped opening a UDP socket, moved, or was never")
    message("enumerated. Fix the row, or find why the scan did not see what it is anchored on.")
    message("")
    message("NOT covered: a UDP socket made through a primitive this table does not name, or a macro.")
    message("And sockets a dependency opens inside this process are not scanned: only first-party source")
    message("is read, so one core-cpp or any other library opens on the program's behalf passes SILENTLY")
    message("-- this rule is open in that direction -- while the any-port rule would expose it all the same.")
    message("core-cpp's own datagram socket (detail::makeDatagramSocket) is reached only through")
    message("openUdpSocket, which is caught at the first-party call site.")
    message("Scanned ${scannedCount} first-party C++ source(s), tests excluded, via ${scanSource}; declined ${declinedCount} under third-party roots.")
    list(LENGTH violations violationCount)
    list(LENGTH stale staleCount)
    message(FATAL_ERROR "udp-opener: ${violationCount} UDP opener(s) outside the permitted file, ${staleCount} stale permitted row(s)")
endif()

message(STATUS
    "udp-opener: ${scannedCount} first-party C++ source(s) via ${scanSource}, tests excluded, ${declinedCount} "
    "declined under third-party roots; the one UDP opener is src/apps/fastcache-compile-node/DiscoveryTier.cpp, "
    "and nothing else opens one")
