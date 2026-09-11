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
/// These three started as private copies in `CacheProxy_test`, `ClusterAdminCli_test`
/// and `CacheTier_test`, and #1330 found five more spread across two further binaries.
/// The rulebook's argument about a shared test FAKE is the argument here, and it is not
/// about repetition: a copy that has stopped matching `DecodeReplyHeader`'s contract
/// goes **GREEN** rather than red, so a change to the reply header has to be found in
/// every copy and no copy will fail if it is missed. Duplication that reports its own
/// drift is cheap; this kind hides it.
///
/// No count is kept here. The set is whatever includes this header, a grep answers it
/// exactly, and a number maintained beside a list nothing derives it from is a second
/// source of truth that drifts while still reading as current.
///
/// ## What is deliberately NOT folded in
///
/// Each of these shares a NAME with something below and answers a different question,
/// so folding it would change what its suite asserts -- which is not a consolidation.
/// Recorded because "why is this one still private" is the question a reader arrives
/// with, and an unanswered one gets closed by folding it.
///
/// - **`CompileResponder_test`** keeps its own pair: its `StatusOf` `REQUIRE`s the
///   header and returns a bare `Status`, and its `ErrorOf` runs `DecodeErrorPayload`
///   where the one below reads the first payload byte.
/// - **`CompileCacheHandler_test`** takes an already-decoded `ReplyFrame` rather than
///   bytes and answers a `DecodedError { present, code, message }` built from
///   `DecodeErrorPayload`. Same name, different parameter and different return.
/// - **`fastcache-cc/WorkerProtocol_test`** is TOTAL where the one below is partial:
///   it returns a bare `ErrorCode`, using `MalformedFrame` as its cannot-read value
///   rather than `nullopt`, and all of its call sites compare with `==`. Folding it
///   would rewrite every one of them. **Reachability is not the reason** -- the
///   tempting argument is that the launcher does not link `FastCache`, and it does not
///   hold: that file already includes three `src/tests/` headers, and this one depends
///   only on `CompileCacheWire.hpp`, which is header-only and dependency-free by rule.
///   It is the SHAPE that excludes it, and stating the wrong reason would send the
///   next reader to fix a linkage problem that is not there.

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
