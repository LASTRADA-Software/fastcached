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
        Cache/ShardedStorage.cpp
        Cache/TracingStorage.cpp
        Config/ByteSize.cpp
        Config/CliParser.cpp
        Config/ConfigReloader.cpp
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
        Platform/HostMemory.cpp
        Platform/PosixDaemonHost.cpp
        Platform/ServiceControl.cpp
        Platform/Terminal.cpp
        Platform/WindowsServiceHost.cpp
        Net/InMemoryTransport.cpp
        Net/Framing/LineReader.cpp
        Protocol/MemcachedBinary.cpp
        Protocol/MemcachedMeta.cpp
        Protocol/MemcachedText.cpp
        Protocol/ProtocolAutodetect.cpp
        Protocol/KeyspaceNotifier.cpp
        Protocol/PubSubRegistry.cpp
        Protocol/RedisResp.cpp
        Protocol/RedisTransaction.cpp
        Server/AdminHttpServer.cpp
        Server/Connection.cpp
        Server/ReactorServerLoop.cpp
        Server/Server.cpp
)

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
            Core/Endian.hpp
            Core/StringHash.hpp
            Core/Clock.hpp
            Core/Logger.hpp
            Core/BufferPool.hpp
            Core/Profiling.hpp
)
