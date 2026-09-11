// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <optional>
#include <string_view>

namespace FastCache
{

/// @file LeaderRedirect.hpp
/// The one predicate that reads a `NotLeader` refusal.
///
/// `NotLeader` is an INSTRUCTION, not an answer about the fleet: a client follows it to
/// the endpoint it names. But it carries two opposite facts under one code -- *somebody
/// else leads, at this address* and *there is no leader right now* -- and the only thing
/// separating them is whether the message parses as an address. A client that tested the
/// message for EMPTY instead gets neither, because an empty message is replaced by the
/// error table's default sentence: both arrive as a non-empty string.
///
/// Splitting is not parsing. `SplitHostPort` takes the LAST colon, so *no leader: try
/// again* splits into a host and a port of ` try again` -- and a launcher DIALS what the
/// admin CLI only printed, so a wrong answer here is a hop the real leader never hears.
///
/// **It lives here because it has three callers and had two authors.** `ClusterAdminCli`
/// wrote this rule once and `fastcache-cc` wrote it again; that was
/// [#237](https://github.com/LASTRADA-Software/fastcached/issues/237), closed by making
/// the first ask the second. `fastcache-cli` gaining the cluster verbs would have been
/// the THIRD author, and the third one is the one nobody compares against the other two.
/// A rule that works only while every copy agrees is a rule with a scheduled expiry.
///
/// Header-only and resting on two header-only headers, because `fastcache-cc` does not
/// link the `FastCache` library and must still be able to ask.

/// The endpoint a refusal redirects to, or nothing.
///
/// The message is returned as it arrived rather than as the parse: the endpoint travels
/// on to a dial, which splits it again, and handing back a re-joined form would be this
/// layer normalising text the scheduler chose.
///
/// @param code What the server refused with.
/// @param message What it said about it.
/// @return The endpoint to retry against, or `std::nullopt` when this is not a redirect
///         -- a `NotLeader` raised while no leader is known included, and any message
///         that does not parse as `host:port`.
[[nodiscard]] inline std::optional<std::string_view> LeaderRedirectTarget(CompileCacheWire::ErrorCode code,
                                                                          std::string_view message)
{
    if (code != CompileCacheWire::ErrorCode::NotLeader)
        return std::nullopt;
    // `ParseDialEndpoint`, and not a test spelled again here: it is also what the dial
    // asks of this very string a moment later, and two spellings of *is this an address*
    // would eventually disagree.
    if (!ParseDialEndpoint(message).has_value())
        return std::nullopt;
    return message;
}

} // namespace FastCache
