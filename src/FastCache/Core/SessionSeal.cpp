// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Hkdf.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <utility>

namespace FastCache
{

namespace
{
    /// The tag for the frame at @p position.
    ///
    /// The position is a FIELD of the MAC's message and never travels: both ends count, so a
    /// frame dropped, replayed or reordered inside a session is tagged for one position and
    /// checked at another. The header and payload are two fields, so a receiver that read them
    /// apart MACs what it read without first copying them together -- and so no choice of
    /// header can shift bytes across the boundary into the payload.
    /// @param key The session's key.
    /// @param position The frame's place in its session, from zero.
    /// @param header The frame's header.
    /// @param payload The frame's payload.
    /// @return The tag.
    [[nodiscard]] SessionTag TagAt(SessionKey const& key,
                                   std::uint64_t position,
                                   std::span<std::byte const> header,
                                   std::span<std::byte const> payload)
    {
        auto const at = WireFields::ToBigEndian<std::uint64_t>(position);
        return HmacSha256(key.Bytes(), WireFields::Encode({ std::span<std::byte const> { at }, header, payload }));
    }
} // namespace

SessionKey::SessionKey(SecureByteBuffer bytes) noexcept:
    _bytes { std::move(bytes) }
{
}

std::expected<SessionKey, CryptoError> DeriveSessionKey(std::span<std::byte const> sharedSecret,
                                                        std::span<std::byte const> salt,
                                                        std::span<std::byte const> info)
{
    return HkdfSha256(salt, sharedSecret, info, SessionKeyBytes).transform([](SecureByteBuffer bytes) {
        return SessionKey { std::move(bytes) };
    });
}

FrameSealer::FrameSealer(SessionKey key) noexcept:
    _key { std::move(key) }
{
}

SessionTag FrameSealer::Seal(std::span<std::byte const> header, std::span<std::byte const> payload)
{
    return TagAt(_key, _next++, header, payload);
}

FrameOpener::FrameOpener(SessionKey key) noexcept:
    _key { std::move(key) }
{
}

bool FrameOpener::Open(std::span<std::byte const> header, std::span<std::byte const> payload, SessionTag const& tag)
{
    if (!ConstantTimeEquals(TagAt(_key, _next, header, payload), tag))
        return false;
    ++_next;
    return true;
}

} // namespace FastCache
