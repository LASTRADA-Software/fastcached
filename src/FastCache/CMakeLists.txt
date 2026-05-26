# SPDX-License-Identifier: Apache-2.0
# FastCache library — source list. Headers are added so IDEs surface them in
# the target view; only the .cpp files actually drive compilation.

target_sources(FastCache
    PRIVATE
        Core/Logger.cpp
        Core/BufferPool.cpp
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
