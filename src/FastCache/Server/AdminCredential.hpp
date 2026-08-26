// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// How a client may present the admin credential.
///
/// Two schemes rather than one, and the pair is chosen rather than generous:
///
///   - **`Basic`** is what a *browser* can be made to prompt for. A page whose
///     whole point is being opened from a laptop is unreachable without it --
///     there is no browser prompt for `Bearer`, so a reader would have to paste a
///     token into a URL or a cookie, both of which are worse places for a secret.
///   - **`Bearer`** is what a script reaches for, and costs one row.
///
/// A row rather than an `if`, so a third scheme is a row somebody adds. The
/// username half of `Basic` is **ignored**: the token file holds one secret, and
/// demanding a matching username would be a second secret nobody was given.
struct AdminAuthScheme
{
    /// The scheme token, as it appears before the space.
    std::string_view name;
    /// Extract the presented secret from what follows it.
    ///
    /// Returns nullopt when the parameter is not well formed for this scheme --
    /// which is a refusal, never an empty secret that might compare equal to
    /// something.
    std::optional<std::string> (*extract)(std::string_view parameter);
};

/// Every scheme the admin surface accepts.
[[nodiscard]] std::array<AdminAuthScheme, 2> const& AdminAuthSchemes() noexcept;

/// The credential an admin route requires, or the absence of one.
///
/// A type rather than a bare string so "no credential configured" is a state
/// somebody constructs rather than the empty string -- which would otherwise
/// compare equal to a client that sent nothing, and open the surface by accident.
class AdminCredential
{
  public:
    /// No credential: every route is served to anybody who can reach the port.
    AdminCredential() = default;

    /// Require @p secret.
    /// @param secret The shared secret; must not be empty.
    explicit AdminCredential(std::string secret) noexcept: _secret { std::move(secret) } {}

    /// Whether a credential is configured at all.
    /// @return True when one must be presented.
    [[nodiscard]] bool Required() const noexcept
    {
        return !_secret.empty();
    }

    /// Whether this `Authorization` header value presents the right secret.
    ///
    /// Compared in constant time, so the credential cannot be recovered one byte
    /// at a time from how long the answer took.
    /// @param authorization The header value, empty when the client sent none.
    /// @return True when the caller may proceed.
    [[nodiscard]] bool Accepts(std::string_view authorization) const;

  private:
    std::string _secret;
};

} // namespace FastCache
