// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Auth/AuthPolicy.hpp>
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Server/AdminCredential.hpp>

#include <algorithm>

namespace FastCache
{

namespace
{
    /// Case-insensitive comparison, for a scheme token.
    ///
    /// RFC 7235 makes the scheme case-insensitive, and clients differ: curl sends
    /// `Basic`, some tooling sends `basic`. A byte comparison would accept the
    /// credential from one and refuse it from the other, which gets diagnosed as
    /// "the token is wrong".
    [[nodiscard]] bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) noexcept {
            auto const lower = [](char c) noexcept {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(x) == lower(y);
        });
    }
} // namespace

std::array<AdminAuthScheme, 2> const& AdminAuthSchemes() noexcept
{
    static std::array<AdminAuthScheme, 2> const schemes {
        AdminAuthScheme { .name = "Basic",
                          .extract =
                              [](std::string_view parameter) -> std::optional<std::string> {
                                  auto decoded = Base64Decode(parameter);
                                  if (!decoded.has_value())
                                      return std::nullopt;
                                  // `user:secret`, and the username half is ignored:
                                  // the token file holds one secret, so demanding a
                                  // matching username would be a second secret
                                  // nobody was given. A value with no colon at all
                                  // is malformed for this scheme rather than a
                                  // secret with an empty username.
                                  auto const colon = decoded->find(':');
                                  if (colon == std::string::npos)
                                      return std::nullopt;
                                  return decoded->substr(colon + 1);
                              } },
        AdminAuthScheme { .name = "Bearer",
                          .extract =
                              [](std::string_view parameter) -> std::optional<std::string> {
                                  return std::string { parameter };
                              } },
    };
    return schemes;
}

bool AdminCredential::Accepts(std::string_view authorization) const
{
    if (!Required())
        return true;

    auto const space = authorization.find(' ');
    if (space == std::string_view::npos)
        return false;
    auto const scheme = authorization.substr(0, space);
    auto const parameter = authorization.substr(space + 1);

    for (auto const& row: AdminAuthSchemes())
    {
        if (!EqualsIgnoringCase(scheme, row.name))
            continue;
        auto const presented = row.extract(parameter);
        // A scheme that could not parse its parameter is a refusal, not a fall
        // through to the next row: `Basic` with unreadable base64 must not then be
        // tried as a bearer token, which would let a malformed header be compared
        // verbatim against the secret.
        return presented.has_value() && ConstantTimeEquals(*presented, _secret);
    }
    return false;
}

} // namespace FastCache
