# SPDX-License-Identifier: Apache-2.0
#
# `udp-opener` must be SEEN to refuse a second UDP opener through each primitive it names, seen to
# stay quiet over a comment, a test file, a vendored file and a name that merely CONTAINS a
# primitive, and seen to refuse a stale permitted row and an empty tree -- by directory walk and by
# a staged git index. It prints how many cases ran, so a run that stopped early does not read as one
# that judged everything.
#
# Every tree is SYNTHESISED under FASTCACHED_SCRATCH_DIR; the tree under test is never touched.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> [-DGIT_EXECUTABLE=<git>] \
#         -P scripts/check-udp-opener-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-udp-opener.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(ran 0)
set(failures "")

set(discoveryTier "src/apps/fastcache-compile-node/DiscoveryTier.cpp")
set(routeProbe "src/FastCache/Platform/RouteProbe.cpp")
set(otherFile "src/FastCache/Cluster/Other.cpp")

# The baseline tree: the two permitted openers (discovery's, and the route probe's connect-only one), a file that opens nothing, and a vendored root.
function(fastcached_udp_tree name outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/elsewhere\n")
    file(WRITE "${tree}/${discoveryTier}"
        "#include \"DiscoveryTier.hpp\"\n    auto opened = core::net::openSharedPortUdpSocket(host, beacon, reply);\n")
    file(WRITE "${tree}/${routeProbe}" "    auto fd = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);\n")
    file(WRITE "${tree}/${otherFile}" "#include <FastCache/Cluster/Other.hpp>\n")
    file(WRITE "${tree}/vendor/elsewhere/socket.hpp" "int s = ::socket(AF_INET, SOCK_DGRAM, 0);\n")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

function(fastcached_stage_git tree)
    if(NOT GIT_EXECUTABLE)
        message(FATAL_ERROR "udp-opener-selftest: INCONCLUSIVE -- no git to stage ${tree} with")
    endif()
    foreach(step IN ITEMS "init;-q" "add;-A")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" ${step} RESULT_VARIABLE status
                        OUTPUT_QUIET ERROR_VARIABLE errors)
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "udp-opener-selftest: INCONCLUSIVE -- `git ${step}` in ${tree} failed: ${errors}")
        endif()
    endforeach()
endfunction()

function(fastcached_udp_judge name tree expect mustSay)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${check}"
        OUTPUT_VARIABLE output ERROR_VARIABLE errors RESULT_VARIABLE result)
    if(NOT result MATCHES "^[0-9]+$")
        message(FATAL_ERROR "udp-opener-selftest: INCONCLUSIVE -- the check could not be RUN (${result})")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}${errors}")
    string(REPLACE ";" "," flat "${flat}")
    set(objected FALSE)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(objected TRUE)
    endif()
    string(FIND "${flat}" "${mustSay}" said)
    if(expect STREQUAL "refuse" AND NOT objected)
        list(APPEND failures "${name}: accepted, and must refuse. Output: ${flat}")
    elseif(expect STREQUAL "accept" AND objected)
        list(APPEND failures "${name}: refused, and must accept. Output: ${flat}")
    elseif(said EQUAL -1)
        list(APPEND failures "${name}: ${expect}ed without saying `${mustSay}`. Output: ${flat}")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

set(accepted "the UDP openers are ${discoveryTier} and ${routeProbe}, and nothing else opens one")
set(refused "${otherFile}:2: opens a UDP socket outside the permitted files")

fastcached_udp_tree(clean tree)
fastcached_udp_judge(clean "${tree}" accept "${accepted}")

fastcached_udp_tree(cleanGit tree)
fastcached_stage_git("${tree}")
fastcached_udp_judge(cleanGit "${tree}" accept "via git ls-files")

# A second opener, once per primitive the check names. Each is what the firewall rule would expose.
foreach(spelling IN ITEMS
        "sharedPort|    auto pair = core::net::openSharedPortUdpSocket(host, 7000, 0)"
        "plainUdp|    auto socket = core::net::openUdpSocket(host, 7000)"
        "receiveBuffer|    auto socket = core::net::detail::openUdpSocketWithReceiveBuffer(host, 7000, size)"
        "rawDatagram|    int s = ::socket(AF_INET, SOCK_DGRAM, 0)"
        "rawProtocol|    int s = ::socket(AF_INET6, 0, IPPROTO_UDP)")
    string(FIND "${spelling}" "|" bar)
    string(SUBSTRING "${spelling}" 0 ${bar} caseName)
    math(EXPR afterBar "${bar} + 1")
    string(SUBSTRING "${spelling}" ${afterBar} -1 statement)
    fastcached_udp_tree(${caseName} tree)
    file(APPEND "${tree}/${otherFile}" "${statement};\n")
    fastcached_udp_judge(${caseName} "${tree}" refuse "${refused}")
endforeach()

# The refusal says WHY a second opener matters, not only where it is.
fastcached_udp_tree(refusalSaysWhy tree)
file(APPEND "${tree}/${otherFile}" "    auto socket = core::net::openUdpSocket(host, 7000);\n")
fastcached_udp_judge(refusalSaysWhy "${tree}" refuse "would be EXPOSED by the compile node's any-port firewall rule")

fastcached_udp_tree(violationGit tree)
file(APPEND "${tree}/${otherFile}" "    auto socket = core::net::openUdpSocket(host, 7000);\n")
fastcached_stage_git("${tree}")
fastcached_udp_judge(violationGit "${tree}" refuse "via git ls-files")

fastcached_udp_tree(commentedOut tree)
file(APPEND "${tree}/${otherFile}" "// core::net::openUdpSocket is what this file must never call\n")
fastcached_udp_judge(commentedOut "${tree}" accept "${accepted}")

fastcached_udp_tree(nameContainsPrimitive tree)
file(APPEND "${tree}/${otherFile}" "    auto const reopenUdpSocketCount = 0;\n")
fastcached_udp_judge(nameContainsPrimitive "${tree}" accept "${accepted}")

fastcached_udp_tree(testFile tree)
file(WRITE "${tree}/src/FastCache/Cluster/Other_test.cpp" "auto socket = core::net::openUdpSocket(host, 0);\n")
file(WRITE "${tree}/src/tests/Datagram.hpp" "int s = ::socket(AF_INET, SOCK_DGRAM, 0);\n")
fastcached_udp_judge(testFile "${tree}" accept "${accepted}")

fastcached_udp_tree(vendoredDeclined tree)
fastcached_udp_judge(vendoredDeclined "${tree}" accept "declined under third-party roots")

fastcached_udp_tree(staleNoOpener tree)
file(WRITE "${tree}/${discoveryTier}" "#include \"DiscoveryTier.hpp\"\n")
fastcached_udp_judge(staleNoOpener "${tree}" refuse "STALE permitted row -- ${discoveryTier}: was read and opens no UDP socket")

fastcached_udp_tree(staleMissing tree)
file(REMOVE "${tree}/${discoveryTier}")
fastcached_udp_judge(staleMissing "${tree}" refuse "STALE permitted row -- ${discoveryTier}: does not exist")

set(tree "${FASTCACHED_SCRATCH_DIR}/empty")
file(REMOVE_RECURSE "${tree}")
file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "vendor/elsewhere\n")
file(WRITE "${tree}/vendor/elsewhere/socket.hpp" "int s = ::socket(AF_INET, SOCK_DGRAM, 0);\n")
fastcached_udp_judge(empty "${tree}" refuse "matched no first-party C++ source")

list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    string(REPLACE ";" "\n  " printable "${failures}")
    message(FATAL_ERROR "udp-opener-selftest: ${failureCount} of ${ran} case(s) did not judge as they must:\n  ${printable}\n")
endif()
message(STATUS "udp-opener-selftest: ${ran} case(s) ran, every verdict as it must be")
