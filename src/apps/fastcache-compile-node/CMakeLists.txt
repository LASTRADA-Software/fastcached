# SPDX-License-Identifier: Apache-2.0
# fastcache-compile-node — the compile worker for distributed builds.
#
# Unlike fastcache-cc, this one LINKS the FastCache library, and deliberately: it
# is a daemon, and "the same daemon capabilities as fastcached" means the same
# listener, socket, TLS, logging and (in a later phase) service-management code
# rather than a second implementation of each. The launcher avoids the library
# because it is spawned thousands of times per build and its startup cost is on the
# critical path; a worker starts once and then lives, so the trade is the opposite.
#
# It reuses the launcher's own pieces by compiling those sources in, the same way
# fastcache-cc compiles in a few FastCache leaves. They are the client-side and
# worker-side halves of one protocol, and a second copy of either is how the two
# ends drift apart.
#
# The toolchain-fingerprint group (CmdLine, DirectManifest, Stats,
# ToolchainFingerprint, ToolchainProbe) is here for the same reason and is not
# optional: a worker derives its own fingerprint so an operator never has to paste
# a digest, and it MUST derive it through the identical code its clients use. A
# second implementation would register cleanly, heartbeat happily, and never be
# matched -- with nothing anywhere reporting why.
set(_fc_node_shared
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/CacheProtocol.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/CmdLine.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/CompileJob.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/DirectManifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/Dispatch.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ProcessRunner.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/Stats.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/TcpClient.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainFingerprint.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainProbe.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/WorkerProtocol.cpp")

add_executable(fastcache-compile-node
    main.cpp
    AdminEndpoint.cpp
    WorkerServer.cpp
    ${_fc_node_shared}
)
target_include_directories(fastcache-compile-node PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc")
target_link_libraries(fastcache-compile-node PRIVATE FastCache)
target_compile_definitions(fastcache-compile-node PRIVATE
    FASTCACHE_NODE_VERSION="${FASTCACHED_VERSION_STRING}")
if(WIN32)
    target_link_libraries(fastcache-compile-node PRIVATE ws2_32)
endif()

install(TARGETS fastcache-compile-node
    RUNTIME DESTINATION "${FASTCACHED_INSTALL_BINDIR}"
    COMPONENT Runtime
)

# The node's own unit tests. WorkerServer and AdminEndpoint are the only sources
# here that the launcher's test binary does not already cover -- and AdminEndpoint
# is here rather than in main.cpp precisely so that it can be.
if(FASTCACHED_BUILD_TESTS)
    add_executable(fastcache-compile-node-tests
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/test_main.cpp"
        AdminEndpoint.cpp
        AdminEndpoint_test.cpp
        WorkerServer.cpp
        WorkerServer_test.cpp
        ${_fc_node_shared}
    )
    target_include_directories(fastcache-compile-node-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc")
    target_link_libraries(fastcache-compile-node-tests PRIVATE FastCache Catch2::Catch2)
    if(WIN32)
        target_link_libraries(fastcache-compile-node-tests PRIVATE ws2_32)
    endif()
    include(Catch)
    catch_discover_tests(fastcache-compile-node-tests)
endif()
