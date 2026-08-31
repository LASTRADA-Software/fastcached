// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <span>

namespace FastCache
{

/// What checking an `AUTH` payload established.
///
/// **Four states, not a `bool`**, and the distinction that costs the most is
/// `NoPolicy` versus `Accepted`. A surface with no credential configured answers
/// `AUTH` with `Ok` -- so that turning a token on at a client is not a breaking
/// change against a server that requires none -- but it must NOT mark the
/// connection authenticated, because nothing was verified. Collapsing the two would
/// mean a later reconfiguration that enables a credential blesses every connection
/// already open on the strength of a check that never ran.
///
/// `Malformed` is separate from `Rejected` for the same reason a refusal is named
/// anywhere here: a frame this build could not parse and a secret that did not match
/// are different operator problems, and one wire code for both makes a client
/// version skew look like a wrong password.
enum class CredentialOutcome : std::uint8_t
{
    Malformed, ///< The payload is not a well-formed `AUTH` request.
    NoPolicy,  ///< No credential is configured here; answer `Ok`, verify nothing.
    Rejected,  ///< A credential is configured and this one did not match.
    Accepted,  ///< Verified. The connection may be marked authenticated.
};

/// Check an `AUTH` frame's payload against a surface's policy.
///
/// **One decision, two callers.** The daemon's `0xFC` handler and the compile node's
/// frame server both terminate this verb, and the rules below are subtle enough that
/// writing them twice would be writing them differently
/// ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289)). What each
/// caller still owns is how to *answer* -- the two surfaces reply through different
/// machinery -- not what is true.
///
/// An **empty username** asks to be checked against the secret alone (the redis
/// `requirepass` form), so a client configured with only a token is not locked out
/// of a server that also names a user. That is the form every in-tree client uses:
/// `Cc::Credential` is constructed `{ .username = {}, .secret = token }`.
///
/// @param policy The surface's credential, or nullptr when it has none.
/// @param payload The `AUTH` request payload, already read and bounded by
///        `MaxAuthPayload` -- this function does no length checking, because the
///        pre-payload gate that admitted the frame already applied the ceiling.
/// @return What was established. Only `Accepted` may set a connection's flag.
[[nodiscard]] inline CredentialOutcome CheckCredential(AuthPolicy const* policy, std::span<std::byte const> payload)
{
    auto const fields = CompileCacheWire::DecodeAuthPayload(payload);
    if (!fields.has_value())
        return CredentialOutcome::Malformed;

    if (policy == nullptr || !policy->Enabled())
        return CredentialOutcome::NoPolicy;

    auto const username = CompileCacheWire::AsStringView(fields->username);
    auto const secret = CompileCacheWire::AsStringView(fields->secret);
    bool const ok = username.empty() ? policy->Verify(secret) : policy->Verify(username, secret);
    return ok ? CredentialOutcome::Accepted : CredentialOutcome::Rejected;
}

} // namespace FastCache
