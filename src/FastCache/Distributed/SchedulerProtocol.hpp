// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace FastCache::Distributed
{

/// Turn one `0xFC` request into one reply, for a fleet scheduler.
///
/// **Pure**: bytes in, bytes out, exactly as `Cc::WorkerProtocol` is on the other
/// side of the same wire. It opens no socket, so the whole verb surface is tested
/// by handing it a frame and reading the answer -- with no listener, no reactor and
/// no cluster.
///
/// A scheduler answers exactly three verbs: `Register`, `Heartbeat` and `Lease`.
/// Everything else -- `Store` and `Fetch` above all -- is refused with
/// `DispatchNotPermitted`, because a scheduler is not a cache. That refusal is a
/// *reply* and not a close: a client that sent a cache verb to the scheduler's port
/// learns which, rather than seeing a dropped connection it cannot tell from a dead
/// host. The same reasoning `WorkerProtocol` records for refusing the scheduler's
/// verbs, pointing the other way.
///
/// ## The verb set is a table, and it is the security-relevant one
///
/// `IsSchedulerVerb` is derived from `SchedulerOps` rather than from a `switch`, so
/// a verb reaching this class without a row is refused by construction. That is the
/// direction a mistake has to fail in here for the same reason it is in
/// `OpDescriptor::preAuth`: what an unauthenticated -- or, here, non-member --
/// peer can reach is a property a reviewer must be able to read off a table.
class SchedulerProtocol
{
  public:
    /// @param service Decides every request; must outlive this.
    explicit SchedulerProtocol(SchedulerService& service) noexcept:
        _service { service }
    {
    }

    /// Answer one complete request frame.
    ///
    /// Takes the frame whole rather than a header and a payload, because the caller
    /// has already had to read a declared length in order to know where the frame
    /// ended -- which is the property the `[magic][version][op][u32 length]` header
    /// exists to give, and the reason every refusal below can be a reply.
    /// @param frame Header plus payload, exactly as received.
    /// @param caller Who is asking, gathered by the transport.
    /// @return The encoded reply. Never empty: a refusal is still an answer.
    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> frame, CallerContext const& caller);

  private:
    /// Decode one verb's payload and hand it to the service.
    ///
    /// Named `Route` rather than `Dispatch` deliberately: `RedisResp.cpp` already
    /// defines a free `Dispatch()` in namespace `FastCache`, and that collision
    /// under unqualified lookup is why this whole module is spelled `Distributed`
    /// in the first place. Re-introducing the word here would put the trap back one
    /// scope down.
    /// @param op The verb, already known to be one of `SchedulerOps`.
    /// @param payload The request payload, its length already checked.
    /// @param caller Who is asking.
    /// @return What the service decided, or a malformed-frame refusal.
    [[nodiscard]] SchedulerReply Route(CompileCacheWire::Op op,
                                       std::span<std::byte const> payload,
                                       CallerContext const& caller);

    SchedulerService& _service;
};

} // namespace FastCache::Distributed
