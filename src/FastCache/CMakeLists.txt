# SPDX-License-Identifier: Apache-2.0
# FastCache library — source list. Headers are added so IDEs surface them in
# the target view; only the .cpp files actually drive compilation.

target_sources(FastCache
    PRIVATE
        Async/EpollReactor.cpp
        Async/IocpReactor.cpp
        Async/KqueueReactor.cpp
        Async/TestReactor.cpp
        Auth/AuthPolicy.cpp
        Cache/CacheEngine.cpp
        Cache/CowTreeStorage.cpp
        Cache/InMemoryLruStorage.cpp
        Cache/LayeredStorage.cpp
        Cache/NotifyingStorage.cpp
        Cache/ShardedStorage.cpp
        Cache/TracingStorage.cpp
        Cache/WriteErrorReportingStorage.cpp
        CompileCache/CohortManifest.cpp
        CompileCache/CompileValue.cpp
        CompileCache/PathCanon.cpp
        Config/ByteSize.cpp
        Config/CliParser.cpp
        Config/ConfigMerge.cpp
        Config/ConfigReloader.cpp
        Config/EnvExpand.cpp
        Config/YamlReader.cpp
        Core/Crc32c.cpp
        Core/Logger.cpp
        Core/BufferPool.cpp
        Metrics/PrometheusFormatter.cpp
        Net/BlockingSocket.cpp
        Net/HealthProbe.cpp
        Net/EpollSocket.cpp
        Net/IocpSocket.cpp
        Net/KqueueSocket.cpp
        Net/SocketAddress.cpp
        Platform/CpuAffinity.cpp
        Platform/DaemonControls.cpp
        Platform/Environment.cpp
        Platform/HostMemory.cpp
        Platform/PosixDaemonHost.cpp
        Platform/ServiceControl.cpp
        Platform/Terminal.cpp
        Platform/WindowsServiceHost.cpp
        Net/InMemoryTransport.cpp
        Net/Framing/LineReader.cpp
        Protocol/CompileCacheHandler.cpp
        Protocol/MemcachedBinary.cpp
        Protocol/MemcachedMeta.cpp
        Protocol/MemcachedText.cpp
        Protocol/ProtocolAutodetect.cpp
        Protocol/KeyspaceNotifier.cpp
        Protocol/PubSubRegistry.cpp
        Protocol/RedisMutationObserver.cpp
        Protocol/RedisResp.cpp
        Protocol/RedisTransaction.cpp
        Protocol/StreamWaiterRegistry.cpp
        Server/AdminHttpServer.cpp
        Server/Connection.cpp
        Server/ReactorServerLoop.cpp
        Server/Server.cpp
)

# The macOS package prefix, handed to the code that has to agree with it.
# ServiceControl's launchd install picks the system job's storage and log
# directories under this prefix, and the installer scripts lay the payload out
# under the same one; when the two were spelled independently, a build with a
# non-default FASTCACHED_MACOS_PREFIX produced a plist whose StandardOutPath
# pointed into a directory the package never created, so launchd could not
# spawn the job while the installer still reported success.
#
# PUBLIC so ServiceControl_test.cpp asserts against the same value.
if(APPLE)
    target_compile_definitions(FastCache PUBLIC FC_MACOS_PREFIX="/${FASTCACHED_MACOS_PREFIX}")
endif()

# Optional TLS support: compiled and linked only when FASTCACHED_ENABLE_TLS is
# on, so the default build pulls in no OpenSSL header and links no OpenSSL
# symbol. FC_TLS_ENABLED is PUBLIC so dependents (the test binary) can guard
# TLS-only tests behind the same macro.
if(FASTCACHED_ENABLE_TLS)
    target_sources(FastCache
        PRIVATE
            Net/TlsContext.cpp
            Net/TlsSocket.cpp
    )
    target_link_libraries(FastCache PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(FastCache PUBLIC FC_TLS_ENABLED=1)
endif()

# Optional on-disk value compression (zstd + lz4). ON by default. The exact
# target names differ between a CPM source build and a system find_package, so
# resolve whichever target each library actually exports before linking.
# FC_COMPRESSION_ENABLED is PUBLIC so the test binary compiles the same codec
# table (and the codec-selection tests) as the library.
if(FASTCACHED_ENABLE_COMPRESSION)
    if(TARGET libzstd_static)
        set(_fc_zstd_target libzstd_static)
    elseif(TARGET zstd::libzstd_static)
        set(_fc_zstd_target zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        set(_fc_zstd_target zstd::libzstd_shared)
    else()
        message(FATAL_ERROR "FASTCACHED_ENABLE_COMPRESSION is ON but no zstd target was found")
    endif()

    if(TARGET lz4_static)
        set(_fc_lz4_target lz4_static)
    elseif(TARGET LZ4::lz4_static)
        set(_fc_lz4_target LZ4::lz4_static)
    elseif(TARGET lz4::lz4)
        set(_fc_lz4_target lz4::lz4)
    else()
        message(FATAL_ERROR "FASTCACHED_ENABLE_COMPRESSION is ON but no lz4 target was found")
    endif()

    target_sources(FastCache PRIVATE Core/Compression.cpp)
    target_link_libraries(FastCache PUBLIC ${_fc_zstd_target} ${_fc_lz4_target})
    target_compile_definitions(FastCache PUBLIC FC_COMPRESSION_ENABLED=1)
else()
    # Identity-only build: the codec utility still compiles (no zstd/lz4 header).
    target_sources(FastCache PRIVATE Core/Compression.cpp)
endif()

# Header-only files surfaced for IDE indexing.
target_sources(FastCache
    PRIVATE
        FILE_SET fastcache_public_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_SOURCE_DIR}/src"
        FILES
            Core/Errors/NetError.hpp
            Core/Errors/ProtocolError.hpp
            Core/Errors/StorageError.hpp
            Core/Errors/ConfigError.hpp
            Core/Bytes.hpp
            Core/Compression.hpp
            Core/Endian.hpp
            Core/StringHash.hpp
            Core/Clock.hpp
            Core/Logger.hpp
            Core/BufferPool.hpp
            Core/Profiling.hpp
)
