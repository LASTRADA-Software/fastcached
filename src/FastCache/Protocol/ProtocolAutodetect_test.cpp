// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/ProtocolAutodetect.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace FastCache;

TEST_CASE("First byte 0xFC selects the CompileCache flavor")
{
    CHECK(ClassifyFirstByte(std::byte { 0xFC }) == ProtocolFlavor::CompileCache);
}

TEST_CASE("Compile-cache magic does not disturb the existing flavors")
{
    CHECK(ClassifyFirstByte(std::byte { 0x80 }) == ProtocolFlavor::MemcachedBinary);
    CHECK(ClassifyFirstByte(static_cast<std::byte>('*')) == ProtocolFlavor::RedisResp);
    CHECK(ClassifyFirstByte(static_cast<std::byte>('$')) == ProtocolFlavor::RedisResp);
    CHECK(ClassifyFirstByte(static_cast<std::byte>('g')) == ProtocolFlavor::MemcachedText); // "get ..."
}

TEST_CASE("CompileCache flavor has a stable log name")
{
    CHECK(ToStringView(ProtocolFlavor::CompileCache) == "compile-cache");
}
