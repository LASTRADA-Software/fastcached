// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/ProtocolAutodetect.hpp>

#include <cstddef>
#include <expected>
#include <span>

namespace FastCache
{

namespace
{

    constexpr std::byte BinaryMagic { 0x80 };

} // namespace

ProtocolFlavor ClassifyFirstByte(std::byte first) noexcept
{
    if (first == BinaryMagic)
        return ProtocolFlavor::MemcachedBinary;
    if (first == CompileCacheWire::Magic)
        return ProtocolFlavor::CompileCache;
    auto const c = static_cast<char>(first);
    switch (c)
    {
        case '*':
        case '+':
        case '-':
        case ':':
        case '$':
            return ProtocolFlavor::RedisResp;
        default:
            return ProtocolFlavor::MemcachedText;
    }
}

Task<std::expected<AutodetectResult, NetError>> DetectProtocol(ISocket* socket)
{
    std::byte peekBuffer[1] {};
    auto const result = co_await socket->Read(std::span<std::byte> { peekBuffer, 1 });
    if (!result.has_value())
        co_return std::unexpected(result.error());

    if (*result == 0)
        co_return std::unexpected(NetError { .code = NetErrorCode::Eof, .systemCode = 0, .context = {} });

    AutodetectResult outcome;
    outcome.flavor = ClassifyFirstByte(peekBuffer[0]);
    outcome.primer = { peekBuffer[0] };
    co_return outcome;
}

} // namespace FastCache
