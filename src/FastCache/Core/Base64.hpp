// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache
{

/// Encode bytes as standard base64 (RFC 4648 §4), always padded.
///
/// The exact inverse of `Base64Decode` below, and written beside it for that
/// reason: an encoder and a decoder that disagree about the alphabet or about
/// padding is the failure a shared table exists to prevent, and it is invisible
/// until something round-trips through a peer built from the other half.
///
/// **Always padded, and always the standard alphabet.** The decoder refuses an
/// unpadded input and refuses `-`/`_`, so an encoder that produced either would
/// emit something this project cannot read back. There is deliberately no
/// URL-safe variant: two spellings of one value is what makes a MAC over that
/// value checkable in one form and not the other.
/// @param bytes What to encode.
/// @return The encoded text; empty for empty input.
[[nodiscard]] std::string Base64Encode(std::span<std::byte const> bytes);

/// Decode standard base64 (RFC 4648 §4), padding required.
///
/// Written here rather than pulled in, because the one place this project needs
/// base64 is the `Basic` credential on the admin surface -- and a dependency for
/// forty lines of table lookup is not a trade this codebase makes.
///
/// **Refuses rather than repairs.** A byte outside the alphabet, a length that is
/// not a multiple of four, padding in the middle of the input, or a padded final
/// group whose spare bits are not zero all return nullopt instead of being
/// skipped. Skipping them is the traditional shape of this
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
