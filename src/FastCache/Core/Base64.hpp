// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

/// Decode standard base64 (RFC 4648 §4), padding required.
///
/// Written here rather than pulled in, because the one place this project needs
/// base64 is the `Basic` credential on the admin surface -- and a dependency for
/// forty lines of table lookup is not a trade this codebase makes.
///
/// **Refuses rather than repairs.** A byte outside the alphabet, a length that is
/// not a multiple of four, or padding in the middle of the input all return
/// nullopt instead of being skipped. Skipping them is the traditional shape of this
/// function and it is wrong here for a specific reason: this decodes a credential,
/// and a decoder that quietly ignores what it does not understand turns two
/// different inputs into one secret.
///
/// The URL-safe alphabet (`-` and `_`) is deliberately **not** accepted: HTTP Basic
/// is specified over the standard one, and accepting both would mean two spellings
/// of the same credential.
/// @param text The encoded text.
/// @return The decoded bytes, or nullopt when @p text is not valid base64.
[[nodiscard]] std::optional<std::string> Base64Decode(std::string_view text);

} // namespace FastCache
