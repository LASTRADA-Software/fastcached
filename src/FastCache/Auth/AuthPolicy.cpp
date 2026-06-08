// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Auth/AuthPolicy.hpp>

#include <cstddef>
#include <ranges>
#include <utility>

namespace FastCache
{

bool ConstantTimeEquals(std::string_view a, std::string_view b) noexcept
{
    // A length difference is reported immediately: the length of an
    // attacker-supplied guess is not secret, and comparing unequal-length
    // inputs byte-by-byte buys no security. For equal lengths, accumulate the
    // XOR of every byte pair without short-circuiting so the running time is
    // independent of where the first mismatch occurs.
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (auto const i: std::views::iota(std::size_t { 0 }, a.size()))
        diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    return diff == 0;
}

AuthPolicy::AuthPolicy(std::string username, std::string secret) noexcept:
    _username { std::move(username) },
    _secret { std::move(secret) }
{
}

bool AuthPolicy::Enabled() const noexcept
{
    return !_secret.empty();
}

bool AuthPolicy::Verify(std::string_view password) const noexcept
{
    return ConstantTimeEquals(password, _secret);
}

bool AuthPolicy::Verify(std::string_view username, std::string_view password) const noexcept
{
    // Evaluate both comparisons unconditionally (no `&&` short-circuit) so a
    // username mismatch and a password mismatch take the same time.
    bool const userOk = ConstantTimeEquals(username, _username);
    bool const passOk = ConstantTimeEquals(password, _secret);
    return userOk && passOk;
}

std::string_view AuthPolicy::Username() const noexcept
{
    return _username;
}

SharedAuthSource::SharedAuthSource(std::shared_ptr<AuthPolicy const> initial) noexcept:
    _policy { std::move(initial) }
{
}

std::shared_ptr<AuthPolicy const> SharedAuthSource::Current() const noexcept
{
    std::scoped_lock const lock { _mu };
    return _policy;
}

void SharedAuthSource::Store(std::shared_ptr<AuthPolicy const> next) noexcept
{
    std::scoped_lock const lock { _mu };
    _policy = std::move(next);
}

} // namespace FastCache
