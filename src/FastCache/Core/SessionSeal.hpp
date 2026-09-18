// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/CryptoError.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace FastCache
{

// Sealing the frames of an authenticated session (#178).
//
// A session is what two ends agree on at the end of a handshake: a KEY only they hold,
// derived from an ephemeral Diffie-Hellman exchange both of them signed. Every frame after
// the handshake carries an HMAC-SHA256 tag under that key, over the frame's position in its
// session and its bytes -- so a frame changed, dropped, replayed, reordered or carried over
// from another session fails its tag, and there is no field an attacker could set to make it
// pass.
//
// **Here, in `Core/`, rather than in the Raft peer wire it was written for**, because it is
// not about Raft: it is what the `0xFC` wire needs next, and a second copy of "tag a frame
// under a session key at an implicit position" would be two constructions that can drift.
// What a wire derives its key FROM -- which transcript, which labels -- stays with the wire.
//
// **One direction per key.** A sealer and an opener over one key agree on ONE count, so a
// wire that sends both ways derives a key per direction; one that shares a key between two
// senders has two counts claiming the same positions, and each end refuses the other's frames.
// The Raft peer wire sends one way per connection, which is why it derives one.
//
// Pure: no socket, no clock, no randomness. The key arrives from a caller that drew it.

/// Bytes in a session key: one HKDF-SHA256 block.
inline constexpr std::size_t SessionKeyBytes = Sha256::DigestSize;

/// Bytes in the tag after each sealed frame.
inline constexpr std::size_t SessionTagBytes = Sha256::DigestSize;

/// The tag after each sealed frame. Not a secret: it travels.
using SessionTag = Sha256::Digest;

/// The key one session's frames are sealed under.
///
/// Its own type rather than a `SecureByteBuffer`, so the raw Diffie-Hellman output -- which is
/// a curve coordinate, not a key -- cannot be handed to a sealer by mistake: the only way to
/// make one is `DeriveSessionKey`. It zeroes its storage at every release.
class SessionKey
{
  public:
    /// @return The key's bytes, for the HMAC and nothing else.
    [[nodiscard]] std::span<std::byte const> Bytes() const noexcept
    {
        return _bytes;
    }

  private:
    friend std::expected<SessionKey, CryptoError> DeriveSessionKey(std::span<std::byte const> sharedSecret,
                                                                   std::span<std::byte const> salt,
                                                                   std::span<std::byte const> info);

    /// @param bytes Exactly `SessionKeyBytes` of HKDF output.
    explicit SessionKey(SecureByteBuffer bytes) noexcept;

    SecureByteBuffer _bytes;
};

/// Derive a session key from a Diffie-Hellman exchange (RFC 5869 HKDF-SHA256).
///
/// @param sharedSecret The raw X25519 output both ends computed.
/// @param salt Non-secret values fresh to this session -- both ends' nonces, for instance.
/// @param info What binds the key to its purpose and to the exchange that produced it: a
///        label naming the wire, and both ends' ephemeral public keys, so a key derived for
///        one wire or one exchange is never the key of another.
/// @return The key, or HKDF's refusal -- which a 32-byte output from a 32-byte secret never
///         meets, and which a caller still treats as a handshake that failed.
[[nodiscard]] std::expected<SessionKey, CryptoError> DeriveSessionKey(std::span<std::byte const> sharedSecret,
                                                                      std::span<std::byte const> salt,
                                                                      std::span<std::byte const> info);

/// Seals the frames one end of a session sends.
///
/// Holds the session's position, so it is one per session and never shared between two
/// senders: a second sealer over the same key would restart the count and produce tags the
/// opener refuses.
class FrameSealer
{
  public:
    /// @param key The session's key.
    explicit FrameSealer(SessionKey key) noexcept;

    /// The tag for the next frame, which moves the position on.
    /// @param header The frame's header bytes.
    /// @param payload The frame's payload bytes.
    /// @return The tag that goes after the frame.
    [[nodiscard]] SessionTag Seal(std::span<std::byte const> header, std::span<std::byte const> payload);

  private:
    SessionKey _key;
    std::uint64_t _next { 0 };
};

/// Checks the frames one end of a session receives.
class FrameOpener
{
  public:
    /// @param key The session's key.
    explicit FrameOpener(SessionKey key) noexcept;

    /// Whether @p tag is the next frame's, which moves the position on when it is.
    ///
    /// A refusal does not move it, and the caller ends the session anyway: a session that has
    /// seen one frame it cannot account for has no position to resume from. Compared in
    /// constant time.
    /// @param header The frame's header bytes, as read.
    /// @param payload The frame's payload bytes, as read.
    /// @param tag The tag that followed it.
    /// @return True when the frame is this session's next one.
    [[nodiscard]] bool Open(std::span<std::byte const> header, std::span<std::byte const> payload, SessionTag const& tag);

  private:
    SessionKey _key;
    std::uint64_t _next { 0 };
};

} // namespace FastCache
