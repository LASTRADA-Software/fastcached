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
/// emit something this project cannot read back.
///
/// **One spelling per KIND of value, never two for one value.** Two spellings of one
/// value is what makes a MAC over that value checkable in one form and not the other,
/// so what is spelled in this alphabet is spelled in no other: the HTTP `Basic`
/// credential, which its RFC fixes here. `Base64UrlEncode` below is a different kind's
/// one spelling -- a public key an operator types into a flag -- and neither decoder
/// accepts the other's alphabet, so no value can arrive in both.
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

/// Encode bytes as URL-safe base64 (RFC 4648 §5), never padded.
///
/// The spelling of a node's PUBLIC KEY (#178), and the reason it is not the standard
/// one is where the key is typed: inside a `--raft-peer=<id>=<host>:<port>@<key>`
/// token, a unit file and a shell line. `/` and `+` mean something in two of those,
/// and `=` is the token's own separator -- so the key's alphabet is the one of the
/// two that holds none of the three, and an unpadded one, because padding would put
/// `=` back.
///
/// The exact inverse of `Base64UrlDecode` below: the two share one walk and differ
/// from the standard pair only in the alphabet and in carrying no padding.
/// @param bytes What to encode.
/// @return The encoded text: `ceil(4n/3)` characters, no `=`; empty for empty input.
[[nodiscard]] std::string Base64UrlEncode(std::span<std::byte const> bytes);

/// Decode URL-safe base64 (RFC 4648 §5), padding refused.
///
/// **Refuses rather than repairs, by `Base64Decode`'s argument**: a byte outside the
/// URL-safe alphabet -- including `+`, `/` and `=` -- a length no encoder produces
/// (one symbol past a whole group), and a final partial group whose spare bits are
/// not zero all return nullopt. The last is what keeps a key from having two
/// spellings: 32 bytes are 43 symbols carrying 258 bits, and a decoder that ignored
/// the two spare ones would read four different strings as one key.
/// @param text The encoded text.
/// @return The decoded bytes, or nullopt when @p text is not canonical unpadded base64url.
[[nodiscard]] std::optional<std::string> Base64UrlDecode(std::string_view text);

} // namespace FastCache
