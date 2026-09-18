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
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/CodecEnvelope.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/CompileJob.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/DirectManifest.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/Dispatch.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/EndpointDial.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ParallelFor.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ProcessRunner.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/Stats.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainDiscovery.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainFingerprint.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainHost.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ToolchainProbe.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/WorkerProtocol.cpp")

add_executable(fastcache-compile-node
    main.cpp
    NodeLogging.cpp
    AdminEndpoint.cpp
    CacheProxy.cpp
    CacheTier.cpp
    ClusterAdminCli.cpp
    ConsensusTier.cpp
    CordonCli.cpp
    DiscoveryTier.cpp
    EndpointDialer.cpp
    EnrollClient.cpp
    EnrollmentResponder.cpp
    EnrollmentWindow.cpp
    NodeProofClient.cpp
    NodeProofResponder.cpp
    SchedulerLink.cpp
    SchedulerTier.cpp
    ScratchClaim.cpp
    LocalCache.cpp
    NodeAnnounce.cpp
    NodePresenceTier.cpp
    NodeRoster.cpp
    NodeConditions.cpp
    NodeConfig.cpp
    NodeReload.cpp
    NodeFrameSurface.cpp
    NodeIdentity.cpp
    NodeKey.cpp
    NodeSurfaces.cpp
    NodeToolchains.cpp
    RemoteUpstream.cpp
    FrameEndpoint.cpp
    NodeIoLoop.cpp
    WorkerLease.cpp
    WorkerTier.cpp
    CompileCapacity.cpp
    CompileResponder.cpp
    NodeStatusResponder.cpp
    LiveStatsResponder.cpp
    LiveStatsSources.cpp
    FleetReadGate.cpp
    FleetTextResponder.cpp
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

# The node's own unit tests. AdminEndpoint and SchedulerEndpoint are
# the only sources here that the launcher's test binary does not already cover --
# and each of the last two lives outside main.cpp precisely so that it can be.
if(FASTCACHED_BUILD_TESTS)
    add_executable(fastcache-compile-node-tests
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/test_main.cpp"
        AdminEndpoint.cpp
        AdminEndpoint_test.cpp
        CacheProxy.cpp
        CacheProxy_test.cpp
        CacheTier.cpp
        CacheTier_test.cpp
        ClusterAdminCli.cpp
        ClusterAdminCli_test.cpp
        ConsensusTier.cpp
        ConsensusTier_test.cpp
        CordonCli.cpp
        CordonCli_test.cpp
        DiscoveryTier.cpp
        DiscoveryTier_test.cpp
        EndpointDialer.cpp
        EnrollClient.cpp
        EnrollClient_test.cpp
        FleetForgetPropagation_test.cpp
        EnrollmentResponder.cpp
        EnrollmentResponder_test.cpp
        EnrollmentWindow.cpp
        EnrollmentWindow_test.cpp
        NodeProofClient.cpp
    NodeProofResponder.cpp
        NodeProofResponder_test.cpp
        EnrollTrap_test.cpp
        ExchangeLog_test.cpp
        NodeAnnounce_test.cpp
        NodePresenceTier_test.cpp
        SchedulerLink.cpp
        SchedulerLink_test.cpp
        SchedulerTier.cpp
        SchedulerTier_test.cpp
        ScratchClaim.cpp
        ScratchClaim_test.cpp
        LocalCache.cpp
        LocalCache_test.cpp
        RemoteUpstream.cpp
        RemoteUpstream_test.cpp
        NodeAnnounce.cpp
        NodePresenceTier.cpp
        NodeRoster.cpp
        NodeRoster_test.cpp
        NodeConditions.cpp
        NodeConditions_test.cpp
        NodeConfig.cpp
        NodeCredential_test.cpp
        NodeReload.cpp
        NodeReload_test.cpp
        NodeLogging.cpp
        NodeLogging_test.cpp
        NodeFrameSurface.cpp
        NodeFrameSurface_test.cpp
        AuthRefusalContract_test.cpp
        FleetMachinePresence_test.cpp
        FleetRoster_test.cpp
        FleetLeaseRouting_test.cpp
        NodeConfig_test.cpp
        NodeIdentity.cpp
        NodeIdentity_test.cpp
        NodeKey.cpp
        NodeKey_test.cpp
        RaftKeyRevocation_test.cpp
        NodeSurfaces.cpp
        NodeSurfaces_test.cpp
        NodeMembership_test.cpp
        WorkerLease_test.cpp
        NodeToolchains.cpp
        NodeToolchains_test.cpp
        ToolchainHashProgress_test.cpp
        ToolchainWalkConcurrency_test.cpp
        FrameEndpoint.cpp
        NodeIoLoop.cpp
        FrameEndpoint_test.cpp
        WorkerLease.cpp
        WorkerTier.cpp
        WorkerTier_test.cpp
        CompileCapacity.cpp
        CompileCapacity_test.cpp
        CompileResponder.cpp
        NodeStatusResponder.cpp
        LiveStatsResponder.cpp
        LiveStatsSources.cpp
        FleetReadGate.cpp
        FleetTextResponder.cpp
        CompileResponder_test.cpp
        NodeStatusResponder_test.cpp
        LiveStatsResponder_test.cpp
        FleetReadGate_test.cpp
        MembershipGate_test.cpp
        FleetTextResponder_test.cpp
        DocumentedCommandLines_test.cpp
        ${_fc_node_shared}
        # The CLI's reading of a `NodeMetrics` body and of a `/metrics` one, compiled in for the
        # reason the launcher's sources are: one case holds the two doors a node's figures leave by
        # against each other, through the code each client reads them with (#1406).
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cli/CliAnswer.cpp"
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cli/CliValue.cpp"
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cli/StatsSource.cpp"
        # The CLI's one reading of a node's conditions, compiled in for the same reason: the case
        # that proves every surface walks the node's real table renders through the code the CLI
        # draws its `node` line and panel with (#1364).
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cli/NodeStatusText.cpp"
    )
    target_include_directories(fastcache-compile-node-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc"
        "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cli")
    target_link_libraries(fastcache-compile-node-tests PRIVATE FastCache Catch2::Catch2)
    # The launchd label the macOS uninstaller boots this worker's job out by,
    # handed to the test so it compares the packaging's value against the one the
    # binary derives -- rather than against a literal of its own, which would
    # agree with the code and drift from the package in silence. The uninstaller
    # deletes the executable that would otherwise answer for itself, so a label
    # that drifted leaves a job registered with nothing left able to remove it.
    target_compile_definitions(fastcache-compile-node-tests PRIVATE
        FASTCACHED_MACOS_NODE_LABEL="${FASTCACHED_MACOS_NODE_LABEL}"
        # The repository root, so DocumentedCommandLines_test.cpp can read the
        # documentation pages it checks. The docs are not build inputs, so there is
        # no generated copy to point at and no reason to make one.
        #
        # PROJECT_SOURCE_DIR rather than CMAKE_SOURCE_DIR: the two differ the moment
        # this tree is consumed with add_subdirectory(), and the check would then
        # iterate a `docs/` belonging to somebody else -- or, far more likely, throw
        # a filesystem_error for a directory that is not there.
        FASTCACHED_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(fastcache-compile-node-tests PRIVATE ws2_32)
    endif()
    include(Catch)
    # `SKIP_RETURN_CODE 4` tells ctest a Catch2 skip is a skip and not a failure
    # (#499) -- and it is KNOWN BROKEN in the other direction: Catch2's exit code IS
    # its failed-assertion count, so a case failing exactly four assertions is scored
    # *skipped* (#1128). It stays because all three alternative channels were measured
    # and are worse; the argument is at the top of the top-level CMakeLists and the
    # fix is #1152. `catch-skip-return-code` enforces the property, and its green does
    # not mean the property is sound.
    catch_discover_tests(fastcache-compile-node-tests PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION "${FASTCACHED_ERROR_DIALOG_ENVIRONMENT}")
endif()
