// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace FastCache
{

/// Constant-time byte-sequence equality. Runs in time that does not depend on
/// the position of the first differing byte, so it cannot be exploited as a
/// timing oracle to recover a secret one byte at a time. A length mismatch
/// returns false immediately — the length of an attacker-supplied guess is not
/// itself a secret.
/// @param a First operand.
/// @param b Second operand.
/// @return True iff both views have equal length and identical bytes.
[[nodiscard]] bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept;

/// Immutable client-authentication policy.
///
/// Models redis-style `requirepass` (a single shared secret) plus an optional
/// username for the ACL / SASL form. The instance is constructed once and
/// shared, read-only, across every connection for the server's lifetime —
/// there is no per-connection state here (that lives in the protocol handler's
/// coroutine frame). Verification is constant-time (see ConstantTimeEquals) so
/// the secret cannot leak through response timing.
class AuthPolicy
{
  public:
    /// Construct a policy.
    /// @param username Expected username for the two-argument verify form
    ///                 (redis `AUTH <user> <pass>` / SASL PLAIN authcid).
    /// @param secret   Shared secret; an empty secret means "auth disabled".
    AuthPolicy(std::string username, std::string secret) noexcept;

    /// @return True when a non-empty secret is configured (auth is required).
    [[nodiscard]] bool Enabled() const noexcept;

    /// Verify a password against the configured secret (redis `requirepass`
    /// form, where the username is implied to be the default user).
    /// @param password Candidate password.
    /// @return True on a constant-time match.
    [[nodiscard]] bool Verify(std::string_view password) const noexcept;

    /// Verify a username/password pair (redis ACL / SASL PLAIN form).
    /// @param username Candidate username.
    /// @param password Candidate password.
    /// @return True when both username and password match (constant-time).
    [[nodiscard]] bool Verify(std::string_view username, std::string_view password) const noexcept;

    /// @return The configured username.
    [[nodiscard]] std::string_view Username() const noexcept;

  private:
    std::string _username;
    std::string _secret;
};

} // namespace FastCache
