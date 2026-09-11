// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace FastCache::Testing
{

/// @file WireReply.hpp
/// Reading a framed `0xFC` reply, for a test that sent one.
///
/// Three functions that had four private copies between them in one directory --
/// `CacheProxy_test`, `ClusterAdminCli_test` and `CacheTier_test` spelled `StatusOf`
/// identically, and two of the three spelled `ErrorOf` and `PayloadOf` identically
/// too. The rulebook's argument about a shared test FAKE is the argument here: a copy
/// that has stopped matching `DecodeReplyHeader`'s contract goes GREEN rather than
/// red, so the next change to the reply header has to be found in four files and
/// three of them will not fail if it is missed.
///
/// **`CompileResponder_test` keeps its own pair deliberately**, and it is a different
/// shape rather than a fourth copy: its `StatusOf` `REQUIRE`s the header and returns a
/// bare `Status`, and its `ErrorOf` runs `DecodeErrorPayload` where the one below
/// reads the first payload byte. Folding those into these would change what two
/// suites assert, which is not a consolidation.

/// The status of a framed reply.
/// @param reply The reply bytes.
/// @return Its status, or nullopt when the header does not decode.
[[nodiscard]] inline std::optional<CompileCacheWire::Status> StatusOf(std::span<std::byte const> reply)
{
    auto const header = CompileCacheWire::DecodeReplyHeader(reply);
    return header.has_value() ? std::optional { header->status } : std::nullopt;
}

/// The error code of a refusal.
/// @param reply The reply bytes.
/// @return The code, or nullopt when the reply is not a refusal carrying one.
[[nodiscard]] inline std::optional<CompileCacheWire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = CompileCacheWire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != CompileCacheWire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<CompileCacheWire::ErrorCode>(reply[CompileCacheWire::ReplyHeaderSize]);
}

/// Everything after the reply header.
/// @param reply The reply bytes.
/// @return The payload; empty when there is none.
[[nodiscard]] inline std::span<std::byte const> PayloadOf(std::span<std::byte const> reply)
{
    return reply.subspan(CompileCacheWire::ReplyHeaderSize);
}

} // namespace FastCache::Testing
