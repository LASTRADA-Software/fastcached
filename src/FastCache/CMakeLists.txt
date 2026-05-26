# SPDX-License-Identifier: Apache-2.0
# FastCache library — source list. Headers are added so IDEs surface them in
# the target view; only the .cpp files actually drive compilation.

target_sources(FastCache
    PRIVATE
        Async/TestReactor.cpp
        Cache/CacheEngine.cpp
        Cache/DiskStorage.cpp
        Cache/InMemoryLruStorage.cpp
        Config/CliParser.cpp
        Config/ConfigReloader.cpp
        Config/YamlReader.cpp
        Core/Crc32c.cpp
        Core/Logger.cpp
        Core/BufferPool.cpp
        Net/BlockingSocket.cpp
        Platform/DaemonControls.cpp
        Platform/PosixDaemonHost.cpp
        Platform/WindowsServiceHost.cpp
        Net/InMemoryTransport.cpp
        Net/Framing/LineReader.cpp
        Protocol/MemcachedBinary.cpp
        Protocol/MemcachedText.cpp
        Protocol/ProtocolAutodetect.cpp
        Protocol/RedisResp.cpp
        Server/BlockingServerLoop.cpp
        Server/Connection.cpp
        Server/Server.cpp
)

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
            Core/Clock.hpp
            Core/Logger.hpp
            Core/BufferPool.hpp
)
