// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Core/X25519.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache::Consensus::RaftWire
{

/// The `0xFA` Raft peer wire format: how one node's `RaftMessage` reaches another.
///
/// **Header-only and pure.** Every function here is a transform between bytes and
/// the structs in `RaftTypes.hpp` — no socket, no clock, no state — which is the
/// same deliberate exception to the inject-every-dependency rule that
/// `CompileCacheWire` documents, and for the same reason: there is nothing to
/// inject. Keeping serialization out of `IRaftTransport` is what lets a whole
/// cluster run in one process over direct calls, which is how the algorithm is
/// tested against partitions and reordering at all.
///
/// ## Frame layout
///
/// ```
/// frame   := [u8 magic=0xFA][u8 version][u8 type][u32 payloadLength] payload
/// payload := field*        where field := [u32 len][len bytes]
///
/// connection := Challenge(acceptor -> dialler)
///               Proof(dialler -> acceptor)
///               Verdict(acceptor -> dialler)
///               (frame [32-byte tag])*   dialler -> acceptor, once Accepted
/// ```
///
/// The first three are the HANDSHAKE and carry no trailer: their own fields hold the
/// signatures. Everything after is a SESSION frame, and its tag sits after the payload,
/// outside `payloadLength` -- so the header keeps exactly the meaning `WireFrame`
/// gives it, and the compile-cache wire that shares it is untouched. Which types are
/// legal in which part is `FramePhase`, a column of `MessageTable`. What the signatures
/// cover, and what key the tags are made under, is `RaftPeerSession`'s (#1308, #178).
///
/// The field grammar is `Core/WireFields.hpp`, shared verbatim with the
/// compile-cache wire so the two cannot drift. All multi-byte integers are
/// big-endian — a host-order length is a frame that only round-trips between
/// machines of the same architecture.
///
/// ## Why a declared length, when nothing here ever replies
///
/// The compile-cache wire needs its length so a rejection can be a *reply*
/// instead of a dropped connection. Raft has no replies to make: `IRaftTransport`
/// is best-effort and returns nothing, because the algorithm already assumes any
/// message may be lost and recovers on the next heartbeat.
///
/// The length earns itself for a different reason, and it is the one that matters
/// in a fleet nobody upgrades atomically. A peer connection is long-lived and
/// carries a stream of messages, so a frame a receiver cannot interpret is not
/// the end of the conversation — it must be **stepped over** so the next frame,
/// which the receiver very likely does understand, still arrives. Without a
/// declared length the only available response to an unrecognised type is to
/// close, and a node running a newer build would silently partition itself from
/// every older peer it talked to. That is why `FrameHeader::kindRaw` is kept
/// **raw**: an unknown type is a recoverable condition the reader skips, not a
/// decode failure, and validating it into an enum too early is what would throw
/// that away.
///
/// ## Why there is a handshake now
///
/// This section used to argue the opposite, and its reasoning was sound for what it
/// was asked: a VERSION negotiation re-run on every reconnect costs a round trip at
/// the moment a cluster is least healthy, while one byte per frame answers "can I
/// read this?" from the frame alone. That still holds, and the version byte still
/// travels in every frame.
///
/// What changed is that the wire authenticates (#1308), and freshness is the one
/// thing a frame cannot prove about itself. A tag over a frame alone accepts that
/// frame again tomorrow, on another connection, from anybody who recorded it. So each
/// connection opens with a challenge the ACCEPTOR chose, and every frame after it is
/// bound to that exchange. A reconnect costs one round trip, two signatures, two
/// verifications and a Diffie-Hellman exchange at each end (#178), which is the price the
/// old section was right to refuse for a version check and is cheap for the question it now
/// answers.
///
/// It also makes the version a property of the CONNECTION. A handshake frame at a
/// version this build does not speak ends the connection before anything is read, and
/// a session frame whose version byte differs from the handshake's ends it too --
/// stepping over it would mean guessing whether a trailer follows.

/// First byte of every Raft frame. Distinct from the compile-cache magic (0xFC),
/// the memcached binary magic (0x80) and every RESP first byte, so a frame that
/// arrives on the wrong port is rejected rather than half-parsed.
inline constexpr std::byte Magic { 0xFA };

/// Protocol version, carried in every frame header.
///
/// Deliberately not derived from the release version — the wire changes far more
/// rarely than the product, and tying them would force a flag day on every
/// release. The type itself is `WireFrame`'s, since the header it appears in is.
using WireVersion = WireFrame::Version;

/// The version this build speaks and emits.
///
/// 2 since #1308, which changed the GRAMMAR -- a handshake before the first message
/// and a tag after every frame -- rather than only a MAC input, which is why this
/// moved where `DiscoveryWire::CurrentVersion` deliberately did not for #402.
///
/// 3 since #1449, and it is the grammar again, one level down. A configuration
/// carries voters AND learners, as two nested id lists where version 2 had one flat
/// list, and it travels in two places: `InstallSnapshotRequest::configuration`, and
/// the payload of every `EntryKind::Configuration` entry an AppendEntries carries. The
/// frame's arity did not move, so the frame itself would still decode -- which is
/// exactly why the version must: a version 2 peer's flat list read by this build's
/// decoder is refused as malformed at best, and a version 3 configuration entry
/// replicated to a version 2 follower is one it cannot read and silently ignores,
/// counting a quorum of the configuration before it. The question the #402/#1308 pair
/// asks is always WHICH changed, and here a field's grammar did.
///
/// 4 since #178, and it is the grammar a third time: every handshake frame changed shape. A
/// connection is proved by each end's OWN key rather than by a key every member shares, so a
/// proof and a verdict carry a 64-byte Ed25519 signature where they carried a 32-byte MAC, and a
/// challenge and a proof each carry an X25519 ephemeral key, from which the two ends agree a
/// session key nobody else holds. A version 3 peer's handshake is refused by its arity before
/// any field is read -- and should it ever decode, it proves nothing this build can check.
inline constexpr WireVersion CurrentVersion = 4;

/// The oldest version this build still accepts.
///
/// Equal to `CurrentVersion`: a version 1 peer authenticates nothing, and accepting
/// one would be the per-connection fallback #1308 exists to refuse; a version 2 peer
/// spells a configuration this build cannot read (#1449); a version 3 peer proves only that
/// it holds the key every member shares, which is exactly what #178 stopped accepting. So
/// the consensus members of a fleet upgrade together.
inline constexpr WireVersion MinSupportedVersion = 4;

/// Size of the fixed frame header: magic, version, type, payload length.
inline constexpr std::size_t HeaderSize = WireFrame::HeaderSize;

/// Size of the tag after every session frame: an HMAC-SHA256 under the session's own key.
inline constexpr std::size_t TagSize = SessionTagBytes;

/// Size of the signature a proof and a verdict each carry.
inline constexpr std::size_t SignatureSize = Ed25519SignatureBytes;

/// Size of the ephemeral key a challenge and a proof each carry.
inline constexpr std::size_t EphemeralKeySize = X25519KeyBytes;

/// The longest node id a handshake carries.
///
/// A bound because a Proof arrives before its sender has proved anything, and what an
/// unauthenticated peer may make this node buffer has to be a number rather than
/// whatever it declares. Generous against what an id is -- a minted one is 32
/// characters -- so no id an operator would type reaches it; a node whose own id does
/// reach it refuses to send the Proof rather than having every peer refuse it.
inline constexpr std::size_t MaxHandshakeIdBytes = 1024;

/// How long a connection may take to finish its handshake, at either end.
///
/// ONE number for both ends, because they bound one exchange from opposite sides: an
/// acceptor closes a connection that has not proved its id within it, and a dialler
/// abandons an acceptor that has not challenged or answered within it.
///
/// The acceptor's half is the one that matters. Before #1308 a connection that sent
/// nothing held one of `PeerServerOptions::maxConnections` slots for as long as its
/// socket lived, so anything that could reach the port could hold all of them. Sized
/// for a round trip plus the handshake's signatures and Diffie-Hellman exchange on a loaded
/// or instrumented host, not for a network round trip on an idle one.
inline constexpr std::chrono::milliseconds HandshakeBound { 5000 };

/// Wire type codes. One byte, third in the frame header.
///
/// Explicit values because they are a published contract: a renumbering that
/// looked like a harmless reordering would make two builds disagree about what
/// every frame means. Numbered from `0x01` so a zeroed buffer is not a valid
/// type as well as not carrying a valid magic.
enum class MessageType : std::uint8_t
{
    /// Not a message, and never a `MessageTable` row.
    ///
    /// Exists so a default-constructed `MessageDescriptor` and a zeroed buffer
    /// name the same not-a-message rather than a value outside the enumeration —
    /// which is what `bugprone-invalid-enum-default-initialization` objects to,
    /// and it is right to: an enum whose zero means nothing still gets zeroed.
    /// Validity is decided by the table either way, so `FindMessage(0)` finds no
    /// row exactly as it did before this enumerator existed.
    Invalid = 0x00,
    RequestVote = 0x01,             ///< Candidate asking for a vote (§5.2).
    RequestVoteResponse = 0x02,     ///< A voter's answer.
    AppendEntries = 0x03,           ///< Leader replicating; a heartbeat when empty (§5.3).
    AppendEntriesResponse = 0x04,   ///< A follower's answer.
    PreVote = 0x05,                 ///< Asking whether an election could be won (thesis §9.6).
    PreVoteResponse = 0x06,         ///< A voter's answer to that question.
    InstallSnapshot = 0x07,         ///< State a compacted log can no longer replay.
    InstallSnapshotResponse = 0x08, ///< A follower's answer to that.

    // The handshake (#1308). Numbered from 0x10 so the session types keep room to grow
    // without a gap in either run meaning anything.
    Challenge = 0x10, ///< Acceptor -> dialler: the nonce and ephemeral key a proof must answer.
    Proof = 0x11,     ///< Dialler -> acceptor: who it is, whom it dialled, its own fresh values, signed.
    Verdict = 0x12,   ///< Acceptor -> dialler: the signed answer to that proof.
};

/// Where in a connection a message type is legal, and what an unauthenticated peer
/// may make this node buffer for it.
///
/// **A type rather than an enum, so a row that does not state its phase fails to
/// COMPILE** -- `CompileCacheWire::PreAuth`'s reason: a designated initializer
/// value-initializes an omitted member, and a class with no default constructor
/// cannot be. One column rather than two, because a handshake frame's ceiling is what
/// being a handshake frame MEANS here: the only way to state `Handshake` is with a
/// ceiling, so a pre-authentication type with no bound cannot be written.
class FramePhase
{
  public:
    /// Deleted on purpose: a row must state its phase.
    FramePhase() = delete;

    /// A handshake frame, read before the peer has proved anything.
    /// @param ceiling The largest payload it may declare; refused before buffering.
    ///        Must be non-zero.
    /// @return The phase.
    [[nodiscard]] static constexpr FramePhase Handshake(std::size_t ceiling) noexcept
    {
        return FramePhase { ceiling };
    }

    /// A session frame, read only on a connection whose dialler has proved its id.
    /// @return The phase. The connection's own frame cap governs its size.
    [[nodiscard]] static constexpr FramePhase Session() noexcept
    {
        return FramePhase { 0 };
    }

    /// @return True for a handshake frame.
    [[nodiscard]] constexpr bool IsHandshake() const noexcept
    {
        return _ceiling != 0;
    }

    /// @return The handshake ceiling in bytes; 0 for a session frame.
    [[nodiscard]] constexpr std::size_t Ceiling() const noexcept
    {
        return _ceiling;
    }

  private:
    constexpr explicit FramePhase(std::size_t ceiling) noexcept:
        _ceiling { ceiling }
    {
    }

    std::size_t _ceiling;
};

/// The largest payload any handshake frame may declare.
///
/// What a stranger can make this node buffer per connection, and the bound every
/// handshake row is checked against below.
inline constexpr std::size_t MaxHandshakePayload = 4096;

/// What one message type needs, as a row rather than as a branch.
///
/// The field count lives here so it has one home instead of being spelled again
/// at the encoder and the decoder — the arity is the one thing those two must
/// agree on, and it is exactly what drifts when each states it separately.
struct MessageDescriptor
{
    MessageType type { MessageType::Invalid }; ///< The wire code.
    std::string_view name;                     ///< For a log line naming what was refused.
    std::size_t fieldCount {};                 ///< How many top-level fields the payload holds.
    FramePhase phase;                          ///< Where in a connection it is legal; no default.
};

namespace Detail
{
    /// The payload a handshake frame needs when each field is at its largest.
    /// @param fieldBytes The largest size of each field, in wire order.
    /// @return The ceiling, length prefixes included.
    [[nodiscard]] constexpr std::size_t HandshakeCeiling(std::initializer_list<std::size_t> fieldBytes) noexcept
    {
        return core::ranges::FoldLeft(fieldBytes, std::size_t { 0 }, [](std::size_t total, std::size_t bytes) {
            return total + WireFields::FieldPrefixSize + bytes;
        });
    }
} // namespace Detail

/// Every message type this build knows. Adding one is adding a row.
inline constexpr std::array MessageTable {
    MessageDescriptor {
        .type = MessageType::RequestVote, .name = "RequestVote", .fieldCount = 4, .phase = FramePhase::Session() },
    MessageDescriptor { .type = MessageType::RequestVoteResponse,
                        .name = "RequestVoteResponse",
                        .fieldCount = 3,
                        .phase = FramePhase::Session() },
    MessageDescriptor {
        .type = MessageType::AppendEntries, .name = "AppendEntries", .fieldCount = 6, .phase = FramePhase::Session() },
    MessageDescriptor { .type = MessageType::AppendEntriesResponse,
                        .name = "AppendEntriesResponse",
                        .fieldCount = 4,
                        .phase = FramePhase::Session() },
    MessageDescriptor { .type = MessageType::PreVote, .name = "PreVote", .fieldCount = 4, .phase = FramePhase::Session() },
    MessageDescriptor {
        .type = MessageType::PreVoteResponse, .name = "PreVoteResponse", .fieldCount = 3, .phase = FramePhase::Session() },
    MessageDescriptor {
        .type = MessageType::InstallSnapshot, .name = "InstallSnapshot", .fieldCount = 6, .phase = FramePhase::Session() },
    MessageDescriptor { .type = MessageType::InstallSnapshotResponse,
                        .name = "InstallSnapshotResponse",
                        .fieldCount = 4,
                        .phase = FramePhase::Session() },
    // The acceptor's nonce and its ephemeral key.
    MessageDescriptor { .type = MessageType::Challenge,
                        .name = "Challenge",
                        .fieldCount = 2,
                        .phase = FramePhase::Handshake(Detail::HandshakeCeiling({ NonceBytes, EphemeralKeySize })) },
    // The dialler's id, the id it dialled, its nonce, its ephemeral key, and its signature.
    MessageDescriptor { .type = MessageType::Proof,
                        .name = "Proof",
                        .fieldCount = 5,
                        .phase = FramePhase::Handshake(Detail::HandshakeCeiling(
                            { MaxHandshakeIdBytes, MaxHandshakeIdBytes, NonceBytes, EphemeralKeySize, SignatureSize })) },
    // The verdict, the acceptor's id, and its signature.
    MessageDescriptor { .type = MessageType::Verdict,
                        .name = "Verdict",
                        .fieldCount = 3,
                        .phase =
                            FramePhase::Handshake(Detail::HandshakeCeiling({ 1, MaxHandshakeIdBytes, SignatureSize })) },
};

/// Whether every handshake row is bounded by `MaxHandshakePayload`.
///
/// The ceiling being PRESENT is `FramePhase`'s; this is that it is SMALL, which is what
/// makes it a bound on what a stranger can make this node hold rather than a formality.
/// @return True when every handshake row fits.
[[nodiscard]] consteval bool HandshakeFramesAreBounded() noexcept
{
    return std::ranges::all_of(MessageTable, [](MessageDescriptor const& row) {
        return !row.phase.IsHandshake() || row.phase.Ceiling() <= MaxHandshakePayload;
    });
}

static_assert(HandshakeFramesAreBounded(), "a handshake frame's ceiling must fit MaxHandshakePayload");

/// How many fields one log entry encodes as: term, kind, payload.
inline constexpr std::size_t LogEntryFieldCount = 3;

/// Look a raw type byte up in the table.
/// @param kindRaw The byte from a frame header.
/// @return The descriptor, or nullptr when this build does not know the type.
[[nodiscard]] constexpr MessageDescriptor const* FindMessage(std::uint8_t kindRaw) noexcept
{
    return core::findOrNull(
        MessageTable, kindRaw, [](MessageDescriptor const& row) { return static_cast<std::uint8_t>(row.type); });
}

/// How many top-level fields `type`'s payload holds, from the table.
///
/// `constexpr` so the encoder can be *checked* against the table rather than
/// merely documented as agreeing with it; see `Detail::Frame`.
/// @param type The message type.
/// @return Its field count, or zero for a type the table does not know.
[[nodiscard]] constexpr std::size_t FieldCountOf(MessageType type) noexcept
{
    auto const* const row = FindMessage(static_cast<std::uint8_t>(type));
    return row != nullptr ? row->fieldCount : 0;
}

/// Whether this build can decode frames of `version`.
/// @param version The version byte from a frame header.
/// @return True when within [MinSupportedVersion, CurrentVersion].
[[nodiscard]] constexpr bool IsSupported(WireVersion version) noexcept
{
    return WireFrame::IsSupported(version, MinSupportedVersion, CurrentVersion);
}

/// The decoded fixed part of a frame.
///
/// `WireFrame::Header` itself rather than a copy of its three fields: the layout
/// is shared with the compile-cache wire, so a second struct here would be a
/// second thing to keep in step with it. The type is kept **raw**, deliberately;
/// see *Why a declared length* above, and `WireFrame::Header` for the same
/// reasoning stated at the layer that enforces it.
using FrameHeader = WireFrame::Header;

namespace Detail
{

    /// Encode one small enum as a single-byte field.
    /// @tparam E The enumeration; its underlying type must be `std::uint8_t`.
    /// @param value The value to encode.
    /// @return The one byte, as an owning array.
    template <typename E>
    [[nodiscard]] std::array<std::byte, 1> EnumField(E value) noexcept
    {
        return WireFields::ToBigEndian<std::uint8_t>(static_cast<std::uint8_t>(value));
    }

    /// Decode a single-byte field into `E`.
    ///
    /// The width check is this function's; the *range* check is
    /// `Consensus::DecodeWireEnum`, whose bound is DERIVED from the enum's own
    /// `Last` -- so this decoder and `FileRaftStorage`'s cannot disagree about
    /// where the enum ends, and neither of them can be left behind when an
    /// enumerator is appended.
    /// @tparam E The enumeration, which must carry a trailing `Last`.
    /// @param field The field's bytes.
    /// @return The value, or nullopt when the field is the wrong width or the
    ///         byte names no enumerator.
    template <EnumWithLast E>
    [[nodiscard]] std::optional<E> DecodeEnum(std::span<std::byte const> field) noexcept
    {
        auto const raw = WireFields::FromBigEndian<std::uint8_t>(field);
        if (!raw.has_value())
            return std::nullopt;
        return DecodeWireEnum<E>(*raw);
    }

    /// Encode a `Term` as a u64 field.
    ///
    /// Takes the wrapper rather than its `.value` so it does the unwrapping its
    /// name implies, which is also what keeps a `Term` and a `LogIndex` from
    /// being transposed on the way to a field — the confusion `RaftTypes` gives
    /// them distinct types to prevent.
    /// @param term The term.
    /// @return The eight bytes, as an owning array.
    [[nodiscard]] inline std::array<std::byte, sizeof(std::uint64_t)> CounterField(Term term) noexcept
    {
        return WireFields::ToBigEndian<std::uint64_t>(term.value);
    }

    /// Encode a `LogIndex` as a u64 field.
    /// @param index The index.
    /// @return The eight bytes, as an owning array.
    [[nodiscard]] inline std::array<std::byte, sizeof(std::uint64_t)> CounterField(LogIndex index) noexcept
    {
        return WireFields::ToBigEndian<std::uint64_t>(index.value);
    }

    /// Frame a payload behind the fixed header.
    ///
    /// **The only place a frame header is written**, so the layout has exactly one
    /// author. The frame is sized once and filled in place, header and payload
    /// together, rather than encoding the payload separately and prepending —
    /// which would copy an AppendEntries' entries a second time.
    ///
    /// The message type is a **template parameter** so the field count the caller
    /// actually supplies can be `static_assert`ed against the one `MessageTable`
    /// declares. Without that the table's count is consumed by the decoder alone,
    /// and the encoder restates the arity as an array extent that nothing
    /// compares — so a row edited without its encoder arm emits frames this build
    /// cannot decode itself, caught only if somebody remembers to round-trip that
    /// type. Here it is a compile error.
    /// @tparam Type The message type; fixes the expected arity.
    /// @tparam N The field count the caller supplies, deduced.
    /// @param version Version to advertise.
    /// @param fields The payload's fields, in wire order.
    /// @return The framed message.
    template <MessageType Type, std::size_t N>
    [[nodiscard]] std::vector<std::byte> Frame(WireVersion version, std::array<std::span<std::byte const>, N> const& fields)
    {
        static_assert(FieldCountOf(Type) == N, "encoder field count disagrees with MessageTable");

        auto const payloadSize = WireFields::RequireEncodable(fields);

        std::vector<std::byte> frame(HeaderSize + payloadSize);
        std::span<std::byte> const out { frame };
        WireFrame::PutHeader(out, Magic, version, static_cast<std::uint8_t>(Type), static_cast<std::uint32_t>(payloadSize));
        WireFields::EncodeInto(out, HeaderSize, fields);
        return frame;
    }

    /// Encode the log entries of an AppendEntries into one field.
    ///
    /// Nested rather than flattened into the message's own field list: the entry
    /// count varies, and a variable arity at the top level would make the
    /// message's shape undecidable from its descriptor. One field holding a
    /// self-delimiting list keeps the outer message fixed-arity — which is what
    /// lets `SplitExactly` reject a truncated or padded frame — while the inner
    /// list is read with `SplitAll`.
    /// One entry's fields, with the storage they view.
    ///
    /// `Fields()` builds the views from `this` on each call rather than storing
    /// them. Storing them is the obvious spelling and is wrong: the views would
    /// point into whichever object they were constructed in, so copying or moving
    /// one — into a vector, say — leaves them dangling at the original. That is a
    /// use-after-free that a round-trip test does catch, and did.
    class EntryFields
    {
      public:
        /// @param entry The entry to lay out; its payload must outlive this.
        explicit EntryFields(LogEntry const& entry) noexcept:
            _term { CounterField(entry.term) },
            _kind { EnumField(entry.kind) },
            _payload { entry.payload }
        {
        }

        /// The entry's fields, in wire order.
        /// @return Views over this object's storage and the entry's payload.
        [[nodiscard]] std::array<std::span<std::byte const>, LogEntryFieldCount> Fields() const noexcept
        {
            return { std::span<std::byte const> { _term }, std::span<std::byte const> { _kind }, _payload };
        }

      private:
        std::array<std::byte, sizeof(std::uint64_t)> _term;
        std::array<std::byte, 1> _kind;
        std::span<std::byte const> _payload;
    };

    /// @param entries The entries, in index order.
    /// @return The packed entry list.
    /// @throws std::length_error When an entry, or the list, exceeds the u32
    ///         field length — which for a caller means it batched too many.
    [[nodiscard]] inline std::vector<std::byte> EncodeEntries(std::span<LogEntry const> entries)
    {
        // Sized in one pass and written in a second, rather than building a
        // buffer per entry and concatenating them. The obvious spelling costs
        // N+2 allocations and copies every payload three times; this costs one
        // allocation and copies each payload once, which is the same reason
        // `Frame` fills header and payload together instead of prepending.
        auto total = std::uint64_t { 0 };
        for (auto const& entry: entries)
        {
            EntryFields const laid { entry };
            total += WireFields::FieldPrefixSize + std::uint64_t { WireFields::RequireEncodable(laid.Fields()) };
        }
        if (total > WireFields::MaxPayload)
            throw std::length_error("raft entry list exceeds the u32 wire length");

        std::vector<std::byte> packed(static_cast<std::size_t>(total));
        std::span<std::byte> const out { packed };
        std::size_t offset = 0;
        for (auto const& entry: entries)
        {
            EntryFields const laid { entry };
            auto const fields = laid.Fields();
            auto const size = WireFields::RequireEncodable(fields);

            WireFields::PutBigEndian<std::uint32_t>(out, offset, static_cast<std::uint32_t>(size));
            offset += WireFields::FieldPrefixSize;
            WireFields::EncodeInto(out, offset, fields);
            offset += size;
        }
        return packed;
    }

    /// Decode the entry list of an AppendEntries.
    /// @param field The entries field's bytes.
    /// @return The entries, or nullopt when any of them is malformed.
    [[nodiscard]] inline std::optional<std::vector<LogEntry>> DecodeEntries(std::span<std::byte const> field)
    {
        auto const blobs = WireFields::SplitAll(field);
        if (!blobs.has_value())
            return std::nullopt;

        std::vector<LogEntry> entries;
        entries.reserve(blobs->size());
        for (auto const& blob: *blobs)
        {
            auto const parts = WireFields::SplitExactly(blob, LogEntryFieldCount);
            if (!parts.has_value())
                return std::nullopt;

            auto const term = WireFields::FromBigEndian<std::uint64_t>((*parts)[0]);
            auto const kind = DecodeEnum<EntryKind>((*parts)[1]);
            if (!term.has_value() || !kind.has_value())
                return std::nullopt;

            entries.push_back(LogEntry { .term = Term { .value = *term },
                                         .kind = *kind,
                                         .payload = std::vector<std::byte> { (*parts)[2].begin(), (*parts)[2].end() } });
        }
        return entries;
    }

} // namespace Detail

/// Frame a message for the wire.
///
/// One overload taking the variant rather than four public encoders, because the
/// caller is a transport handed a `RaftMessage` and has no reason to know which
/// alternative it holds. `std::visit` gives exhaustiveness: a fifth message type
/// fails to compile here until it is handled.
/// @param message The message to send.
/// @param version Version to advertise; overridable so tests can offer one the
///                peer does not support.
/// @return The framed message.
/// @throws std::length_error When the payload exceeds the u32 wire length, which
///         for an AppendEntries means the caller batched too many entries.
[[nodiscard]] inline std::vector<std::byte> Encode(RaftMessage const& message, WireVersion version = CurrentVersion)
{
    return std::visit(
        [version]<typename T>(T const& m) -> std::vector<std::byte> {
            if constexpr (std::is_same_v<T, PreVoteRequest>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const lastIndex = Detail::CounterField(m.lastLogIndex);
                auto const lastTerm = Detail::CounterField(m.lastLogTerm);
                std::array const fields { std::span<std::byte const> { term },
                                          WireFields::AsBytes(m.candidateId),
                                          std::span<std::byte const> { lastIndex },
                                          std::span<std::byte const> { lastTerm } };
                return Detail::Frame<MessageType::PreVote>(version, fields);
            }
            else if constexpr (std::is_same_v<T, PreVoteResponse>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const decision = Detail::EnumField(m.decision);
                std::array const fields { std::span<std::byte const> { term },
                                          std::span<std::byte const> { decision },
                                          WireFields::AsBytes(m.voterId) };
                return Detail::Frame<MessageType::PreVoteResponse>(version, fields);
            }
            else if constexpr (std::is_same_v<T, RequestVoteRequest>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const lastIndex = Detail::CounterField(m.lastLogIndex);
                auto const lastTerm = Detail::CounterField(m.lastLogTerm);
                std::array const fields { std::span<std::byte const> { term },
                                          WireFields::AsBytes(m.candidateId),
                                          std::span<std::byte const> { lastIndex },
                                          std::span<std::byte const> { lastTerm } };
                return Detail::Frame<MessageType::RequestVote>(version, fields);
            }
            else if constexpr (std::is_same_v<T, RequestVoteResponse>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const decision = Detail::EnumField(m.decision);
                std::array const fields { std::span<std::byte const> { term },
                                          std::span<std::byte const> { decision },
                                          WireFields::AsBytes(m.voterId) };
                return Detail::Frame<MessageType::RequestVoteResponse>(version, fields);
            }
            else if constexpr (std::is_same_v<T, AppendEntriesRequest>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const prevIndex = Detail::CounterField(m.prevLogIndex);
                auto const prevTerm = Detail::CounterField(m.prevLogTerm);
                auto const commit = Detail::CounterField(m.leaderCommit);
                auto const entries = Detail::EncodeEntries(m.entries);
                std::array const fields { std::span<std::byte const> { term },      WireFields::AsBytes(m.leaderId),
                                          std::span<std::byte const> { prevIndex }, std::span<std::byte const> { prevTerm },
                                          std::span<std::byte const> { commit },    std::span<std::byte const> { entries } };
                return Detail::Frame<MessageType::AppendEntries>(version, fields);
            }
            else if constexpr (std::is_same_v<T, InstallSnapshotRequest>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const lastIndex = Detail::CounterField(m.lastIncludedIndex);
                auto const lastTerm = Detail::CounterField(m.lastIncludedTerm);
                auto const configuration = Membership::Encode(m.configuration);
                std::array const fields {
                    std::span<std::byte const> { term },          WireFields::AsBytes(m.leaderId),
                    std::span<std::byte const> { lastIndex },     std::span<std::byte const> { lastTerm },
                    std::span<std::byte const> { configuration }, std::span<std::byte const> { m.state }
                };
                return Detail::Frame<MessageType::InstallSnapshot>(version, fields);
            }
            else if constexpr (std::is_same_v<T, InstallSnapshotResponse>)
            {
                auto const term = Detail::CounterField(m.term);
                auto const result = Detail::EnumField(m.result);
                auto const match = Detail::CounterField(m.matchIndex);
                std::array const fields { std::span<std::byte const> { term },
                                          std::span<std::byte const> { result },
                                          std::span<std::byte const> { match },
                                          WireFields::AsBytes(m.followerId) };
                return Detail::Frame<MessageType::InstallSnapshotResponse>(version, fields);
            }
            else
            {
                static_assert(std::is_same_v<T, AppendEntriesResponse>);
                auto const term = Detail::CounterField(m.term);
                auto const result = Detail::EnumField(m.result);
                auto const match = Detail::CounterField(m.matchIndex);
                std::array const fields { std::span<std::byte const> { term },
                                          std::span<std::byte const> { result },
                                          std::span<std::byte const> { match },
                                          WireFields::AsBytes(m.followerId) };
                return Detail::Frame<MessageType::AppendEntriesResponse>(version, fields);
            }
        },
        message);
}

/// Decode the fixed header at the front of `bytes`.
///
/// Fails only on a short buffer or a wrong magic — the two conditions under which
/// the reader has lost sync and cannot find where this frame ends. An unsupported
/// version and an unknown type both decode successfully here, because both are
/// recoverable and the caller needs `payloadLength` to step over them.
/// @param bytes At least `HeaderSize` bytes from the front of a frame.
/// @return The header, or nullopt when the buffer is short or the magic is wrong.
[[nodiscard]] inline std::optional<FrameHeader> DecodeHeader(std::span<std::byte const> bytes) noexcept
{
    return WireFrame::DecodeHeader(bytes, Magic);
}

/// Decode a frame's payload into a message.
///
/// Takes the header's `kindRaw` and `version` rather than re-reading them, so a
/// caller that has already used `payloadLength` to collect exactly this payload
/// does not have to keep the header bytes around.
/// @param header The already-decoded frame header.
/// @param payload Exactly `header.payloadLength` bytes following it.
/// @return The message, or why it could not be decoded.
[[nodiscard]] inline std::expected<RaftMessage, ConsensusError> DecodeMessage(FrameHeader const& header,
                                                                              std::span<std::byte const> payload)
{
    if (!IsSupported(header.version))
        // Naming the supported range, not merely the offending version: a
        // rejection that cannot say what would have worked cannot be acted on.
        return std::unexpected { UnsupportedWireVersion(std::format("raft frame version {} outside supported range [{}, {}]",
                                                                    unsigned { header.version },
                                                                    unsigned { MinSupportedVersion },
                                                                    unsigned { CurrentVersion })) };

    auto const* const descriptor = FindMessage(header.kindRaw);
    if (descriptor == nullptr)
        return std::unexpected { UnknownWireMessage(
            std::format("raft frame type 0x{:02X} is not known to this build", header.kindRaw)) };

    auto const fields = WireFields::SplitExactly(payload, descriptor->fieldCount);
    if (!fields.has_value())
        return std::unexpected { MalformedWireFrame(
            std::format("{} payload does not hold {} fields", descriptor->name, descriptor->fieldCount)) };

    auto const malformed = [&descriptor](std::string_view what) {
        return std::unexpected { MalformedWireFrame(std::format("{}: {}", descriptor->name, what)) };
    };

    switch (descriptor->type)
    {
        case MessageType::PreVote: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const lastIndex = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            auto const lastTerm = WireFields::FromBigEndian<std::uint64_t>((*fields)[3]);
            if (!term.has_value() || !lastIndex.has_value() || !lastTerm.has_value())
                return malformed("a counter field is not eight bytes");
            return RaftMessage { PreVoteRequest { .term = Term { .value = *term },
                                                  .candidateId = NodeId { WireFields::AsStringView((*fields)[1]) },
                                                  .lastLogIndex = LogIndex { .value = *lastIndex },
                                                  .lastLogTerm = Term { .value = *lastTerm } } };
        }
        case MessageType::PreVoteResponse: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const decision = Detail::DecodeEnum<VoteDecision>((*fields)[1]);
            if (!term.has_value())
                return malformed("a counter field is not eight bytes");
            if (!decision.has_value())
                return malformed("the vote decision names no known outcome");
            return RaftMessage { Consensus::PreVoteResponse { .term = Term { .value = *term },
                                                              .decision = *decision,
                                                              .voterId =
                                                                  NodeId { WireFields::AsStringView((*fields)[2]) } } };
        }
        case MessageType::RequestVote: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const lastIndex = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            auto const lastTerm = WireFields::FromBigEndian<std::uint64_t>((*fields)[3]);
            if (!term.has_value() || !lastIndex.has_value() || !lastTerm.has_value())
                return malformed("a counter field is not eight bytes");
            return RaftMessage { RequestVoteRequest { .term = Term { .value = *term },
                                                      .candidateId = NodeId { WireFields::AsStringView((*fields)[1]) },
                                                      .lastLogIndex = LogIndex { .value = *lastIndex },
                                                      .lastLogTerm = Term { .value = *lastTerm } } };
        }
        case MessageType::RequestVoteResponse: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const decision = Detail::DecodeEnum<VoteDecision>((*fields)[1]);
            if (!term.has_value())
                return malformed("a counter field is not eight bytes");
            if (!decision.has_value())
                return malformed("the vote decision names no known outcome");
            return RaftMessage { RequestVoteResponse { .term = Term { .value = *term },
                                                       .decision = *decision,
                                                       .voterId = NodeId { WireFields::AsStringView((*fields)[2]) } } };
        }
        case MessageType::AppendEntries: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const prevIndex = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            auto const prevTerm = WireFields::FromBigEndian<std::uint64_t>((*fields)[3]);
            auto const commit = WireFields::FromBigEndian<std::uint64_t>((*fields)[4]);
            if (!term.has_value() || !prevIndex.has_value() || !prevTerm.has_value() || !commit.has_value())
                return malformed("a counter field is not eight bytes");

            auto entries = Detail::DecodeEntries((*fields)[5]);
            if (!entries.has_value())
                return malformed("the entry list is malformed");

            return RaftMessage { AppendEntriesRequest { .term = Term { .value = *term },
                                                        .leaderId = NodeId { WireFields::AsStringView((*fields)[1]) },
                                                        .prevLogIndex = LogIndex { .value = *prevIndex },
                                                        .prevLogTerm = Term { .value = *prevTerm },
                                                        .entries = *std::move(entries),
                                                        .leaderCommit = LogIndex { .value = *commit } } };
        }
        case MessageType::InstallSnapshot: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const lastIndex = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            auto const lastTerm = WireFields::FromBigEndian<std::uint64_t>((*fields)[3]);
            if (!term.has_value() || !lastIndex.has_value() || !lastTerm.has_value())
                return malformed("a counter field is not eight bytes");

            auto configuration = Membership::Decode((*fields)[4]);
            if (!configuration.has_value())
                return malformed("the configuration is malformed");

            return RaftMessage { InstallSnapshotRequest {
                .term = Term { .value = *term },
                .leaderId = NodeId { WireFields::AsStringView((*fields)[1]) },
                .lastIncludedIndex = LogIndex { .value = *lastIndex },
                .lastIncludedTerm = Term { .value = *lastTerm },
                .configuration = *std::move(configuration),
                .state = std::vector<std::byte> { (*fields)[5].begin(), (*fields)[5].end() } } };
        }
        case MessageType::InstallSnapshotResponse: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const result = Detail::DecodeEnum<AppendResult>((*fields)[1]);
            auto const match = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            if (!term.has_value() || !match.has_value())
                return malformed("a counter field is not eight bytes");
            if (!result.has_value())
                return malformed("the append result names no known outcome");
            return RaftMessage { Consensus::InstallSnapshotResponse {
                .term = Term { .value = *term },
                .result = *result,
                .matchIndex = LogIndex { .value = *match },
                .followerId = NodeId { WireFields::AsStringView((*fields)[3]) } } };
        }
        case MessageType::AppendEntriesResponse: {
            auto const term = WireFields::FromBigEndian<std::uint64_t>((*fields)[0]);
            auto const result = Detail::DecodeEnum<AppendResult>((*fields)[1]);
            auto const match = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            if (!term.has_value() || !match.has_value())
                return malformed("a counter field is not eight bytes");
            if (!result.has_value())
                return malformed("the append result names no known outcome");
            return RaftMessage { AppendEntriesResponse { .term = Term { .value = *term },
                                                         .result = *result,
                                                         .matchIndex = LogIndex { .value = *match },
                                                         .followerId = NodeId { WireFields::AsStringView((*fields)[3]) } } };
        }
        case MessageType::Challenge:
        case MessageType::Proof:
        case MessageType::Verdict:
            // Rows of the table, and not messages: they are the handshake, decoded by
            // their own functions below, and a connection that sends one after the
            // handshake is out of step with this reader. Refused here as well as by
            // the phase check a reader makes first, so the answer does not depend on
            // every caller remembering to ask.
            return malformed("a handshake frame arrived where a Raft message belongs");
        case MessageType::Invalid:
            // Never a `MessageTable` row, so `FindMessage` cannot return one — but
            // named rather than swept up by a `default:`, because this switch has
            // none: a message type added without a decoder arm must be a compile
            // error here, and one `default:` would silence every one of them.
            break;
    }

    // Unreachable: `descriptor` came from the table, so its type is one of the
    // cases above. Spelled out rather than left to fall off the end, which is
    // undefined behaviour on a corrupted table.
    return malformed("the message table names a type this decoder does not handle");
}

// ---------------------------------------------------------------------------
// The handshake (#1308, #178). What these frames carry is here; what the signatures inside
// them cover, and in what order a connection may send them, is `RaftPeerSession`.

/// What an acceptor answers a proof whose signature verified.
///
/// **ORDINALS ARE A WIRE CONTRACT. Append only; never insert or reorder.** The byte is a
/// field of the Verdict frame and is inside the acceptor's signature.
///
/// Acceptance is deliberately NOT ordinal zero. A zeroed byte is what a buffer nobody
/// filled holds, and it should read as a refusal if it ever reads as anything.
enum class HandshakeVerdict : std::uint8_t
{
    WrongTarget, ///< The dialler asked for another member: its address for that member is stale.
    OwnId,       ///< The dialler proved this node's own id: a copied state directory.
    Accepted,    ///< The dialler proved its id, dialled this node, and is another member.

    /// The dialler's signature verified under a key this node's roster has REVOKED (#178). Signed
    /// like the others, and for their reason: only the machine holding that key can have produced
    /// the proof, so telling it is no oracle -- and without it a revoked machine reports every
    /// refused redial as a key problem on the other end.
    KeyRevoked,

    Last, ///< Not a verdict, and never travels. See `DecodeWireEnum`.
};

static_assert(static_cast<std::uint8_t>(HandshakeVerdict::WrongTarget) == 0,
              "HandshakeVerdict ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(HandshakeVerdict::OwnId) == 1, "HandshakeVerdict ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(HandshakeVerdict::Accepted) == 2, "HandshakeVerdict ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(HandshakeVerdict::KeyRevoked) == 3, "HandshakeVerdict ordinals are a wire contract");

/// The acceptor's opening: its fresh values, before it has read a byte.
struct ChallengeFrame
{
    Nonce nonce {};               ///< What the dialler's proof must answer.
    X25519PublicKey ephemeral {}; ///< The acceptor's half of this connection's Diffie-Hellman exchange.

    /// Value equality, so a round trip is asserted whole.
    [[nodiscard]] bool operator==(ChallengeFrame const&) const = default;
};

/// The dialler's proof of who it is.
struct ProofFrame
{
    NodeId dialler;                ///< Who is dialling.
    NodeId target;                 ///< Which member it believes it dialled.
    Nonce nonce {};                ///< The dialler's own nonce, so the acceptor's answer is fresh too.
    X25519PublicKey ephemeral {};  ///< The dialler's half of the Diffie-Hellman exchange.
    Ed25519Signature signature {}; ///< The dialler's signature over the whole transcript so far.

    /// Value equality, so a round trip is asserted whole.
    [[nodiscard]] bool operator==(ProofFrame const&) const = default;
};

/// The acceptor's signed answer to a proof whose signature verified.
struct VerdictFrame
{
    HandshakeVerdict verdict { HandshakeVerdict::WrongTarget }; ///< What it decided.
    NodeId acceptor;                                            ///< Which member answered.
    Ed25519Signature signature {}; ///< The acceptor's signature over the whole transcript and this verdict.

    /// Value equality, so a round trip is asserted whole.
    [[nodiscard]] bool operator==(VerdictFrame const&) const = default;
};

/// Frame a challenge.
/// @param challenge What to send.
/// @param version Version to advertise; overridable so a test can offer one a peer refuses.
/// @return The framed handshake message. It carries no trailer.
[[nodiscard]] inline std::vector<std::byte> EncodeChallenge(ChallengeFrame const& challenge,
                                                            WireVersion version = CurrentVersion)
{
    std::array const fields { std::span<std::byte const> { challenge.nonce },
                              std::span<std::byte const> { challenge.ephemeral } };
    return Detail::Frame<MessageType::Challenge>(version, fields);
}

/// Frame a proof.
/// @param proof What to send.
/// @param version Version to advertise.
/// @return The framed handshake message. It carries no trailer.
[[nodiscard]] inline std::vector<std::byte> EncodeProof(ProofFrame const& proof, WireVersion version = CurrentVersion)
{
    std::array const fields { WireFields::AsBytes(proof.dialler),
                              WireFields::AsBytes(proof.target),
                              std::span<std::byte const> { proof.nonce },
                              std::span<std::byte const> { proof.ephemeral },
                              std::span<std::byte const> { proof.signature } };
    return Detail::Frame<MessageType::Proof>(version, fields);
}

/// Frame a verdict.
/// @param verdict What to send.
/// @param version Version to advertise.
/// @return The framed handshake message. It carries no trailer.
[[nodiscard]] inline std::vector<std::byte> EncodeVerdict(VerdictFrame const& verdict, WireVersion version = CurrentVersion)
{
    auto const decided = Detail::EnumField(verdict.verdict);
    std::array const fields { std::span<std::byte const> { decided },
                              WireFields::AsBytes(verdict.acceptor),
                              std::span<std::byte const> { verdict.signature } };
    return Detail::Frame<MessageType::Verdict>(version, fields);
}

namespace Detail
{
    /// Where @p Type's row sits in `MessageTable`, found at compile time.
    ///
    /// An index rather than `FindMessage`'s pointer: a constant lookup folds to a pointer
    /// GCC then warns about comparing with null (`-Waddress`), and a row that is known to
    /// exist should not be asked whether it does. The `static_assert` below is what knows.
    /// @tparam Type A message type.
    template <MessageType Type>
    inline constexpr std::size_t RowIndexOf = static_cast<std::size_t>(
        std::ranges::distance(MessageTable.begin(), std::ranges::find(MessageTable, Type, &MessageDescriptor::type)));

    /// @p Type's row, as a constant rather than a reference into the table: a row is a
    /// few words, and a constant is what the naming rule lets a file-scope name be.
    /// @tparam Type A message type with a row in `MessageTable`.
    template <MessageType Type>
        requires(RowIndexOf<Type> < MessageTable.size())
    inline constexpr MessageDescriptor RowOf = MessageTable[RowIndexOf<Type>];

    /// The fields of a handshake frame of type @p Type, or why it is not one.
    ///
    /// Everything a handshake reader refuses before any field means anything, in one
    /// place: a version this build does not speak, a type other than the one this step
    /// of the handshake expects, a payload over the row's ceiling, and a field count
    /// the row does not declare.
    /// @tparam Type The handshake type this step expects.
    /// @param header The frame's decoded header.
    /// @param payload Exactly `header.payloadLength` bytes.
    /// @return The fields, borrowing from @p payload.
    template <MessageType Type>
    [[nodiscard]] std::expected<std::vector<std::span<std::byte const>>, ConsensusError> HandshakeFields(
        FrameHeader const& header, std::span<std::byte const> payload)
    {
        auto const& row = RowOf<Type>;
        if (!IsSupported(header.version))
            return std::unexpected { UnsupportedWireVersion(
                std::format("raft {} version {} outside supported range [{}, {}]",
                            row.name,
                            unsigned { header.version },
                            unsigned { MinSupportedVersion },
                            unsigned { CurrentVersion })) };

        if (header.kindRaw != static_cast<std::uint8_t>(Type))
            return std::unexpected { MalformedWireFrame(
                std::format("expected a raft {} frame, got type 0x{:02X}", row.name, header.kindRaw)) };

        if (payload.size() > row.phase.Ceiling())
            return std::unexpected { MalformedWireFrame(
                std::format("a raft {} payload of {} bytes is over its {}-byte ceiling",
                            row.name,
                            payload.size(),
                            row.phase.Ceiling())) };

        auto fields = WireFields::SplitExactly(payload, row.fieldCount);
        if (!fields.has_value())
            return std::unexpected { MalformedWireFrame(
                std::format("{} payload does not hold {} fields", row.name, row.fieldCount)) };
        return *std::move(fields);
    }

    /// Copy a fixed-width field, or refuse it.
    /// @tparam N The width.
    /// @param field The field's bytes.
    /// @return The bytes, or nullopt when the field is not exactly @p N wide.
    template <std::size_t N>
    [[nodiscard]] std::optional<std::array<std::byte, N>> FixedField(std::span<std::byte const> field) noexcept
    {
        if (field.size() != N)
            return std::nullopt;
        std::array<std::byte, N> out {};
        std::ranges::copy(field, out.begin());
        return out;
    }

    /// An id a handshake carries, or refused.
    ///
    /// Empty, over `MaxHandshakeIdBytes`, or not UTF-8 is refused as a malformed frame,
    /// before any signature: this is what the frame IS rather than what it claims, and an id
    /// that is not text is one no log line may print and no membership can name --
    /// `PeerDirectory::NoteBeacon`'s rule, at this door.
    /// @param field The field's bytes.
    /// @return The id, or nullopt.
    [[nodiscard]] inline std::optional<NodeId> HandshakeId(std::span<std::byte const> field)
    {
        auto const text = WireFields::AsStringView(field);
        if (text.empty() || text.size() > MaxHandshakeIdBytes || !IsValidUtf8(text))
            return std::nullopt;
        return NodeId { text };
    }
} // namespace Detail

/// Decode a challenge.
/// @param header The frame's decoded header.
/// @param payload Exactly `header.payloadLength` bytes.
/// @return The challenge, or why this is not one.
[[nodiscard]] inline std::expected<ChallengeFrame, ConsensusError> DecodeChallenge(FrameHeader const& header,
                                                                                   std::span<std::byte const> payload)
{
    return Detail::HandshakeFields<MessageType::Challenge>(header, payload)
        .and_then([](auto const& fields) -> std::expected<ChallengeFrame, ConsensusError> {
            auto const nonce = Detail::FixedField<NonceBytes>(fields[0]);
            auto const ephemeral = Detail::FixedField<EphemeralKeySize>(fields[1]);
            if (!nonce.has_value())
                return std::unexpected { MalformedWireFrame("Challenge: the nonce is not a nonce's width") };
            if (!ephemeral.has_value())
                return std::unexpected { MalformedWireFrame("Challenge: the ephemeral key is not a key's width") };
            return ChallengeFrame { .nonce = *nonce, .ephemeral = *ephemeral };
        });
}

/// Decode a proof.
/// @param header The frame's decoded header.
/// @param payload Exactly `header.payloadLength` bytes.
/// @return The proof, or why this is not one. Nothing about it is authenticated yet.
[[nodiscard]] inline std::expected<ProofFrame, ConsensusError> DecodeProof(FrameHeader const& header,
                                                                           std::span<std::byte const> payload)
{
    return Detail::HandshakeFields<MessageType::Proof>(header, payload)
        .and_then([](auto const& fields) -> std::expected<ProofFrame, ConsensusError> {
            auto dialler = Detail::HandshakeId(fields[0]);
            auto target = Detail::HandshakeId(fields[1]);
            auto const nonce = Detail::FixedField<NonceBytes>(fields[2]);
            auto const ephemeral = Detail::FixedField<EphemeralKeySize>(fields[3]);
            auto const signature = Detail::FixedField<SignatureSize>(fields[4]);
            if (!dialler.has_value() || !target.has_value())
                return std::unexpected { MalformedWireFrame("Proof: an id is empty, too long, or not UTF-8") };
            if (!nonce.has_value() || !ephemeral.has_value() || !signature.has_value())
                return std::unexpected { MalformedWireFrame(
                    "Proof: the nonce, the ephemeral key or the signature is the wrong width") };
            return ProofFrame { .dialler = *std::move(dialler),
                                .target = *std::move(target),
                                .nonce = *nonce,
                                .ephemeral = *ephemeral,
                                .signature = *signature };
        });
}

/// Decode a verdict.
/// @param header The frame's decoded header.
/// @param payload Exactly `header.payloadLength` bytes.
/// @return The verdict, or why this is not one. Nothing about it is authenticated yet.
[[nodiscard]] inline std::expected<VerdictFrame, ConsensusError> DecodeVerdict(FrameHeader const& header,
                                                                               std::span<std::byte const> payload)
{
    return Detail::HandshakeFields<MessageType::Verdict>(header, payload)
        .and_then([](auto const& fields) -> std::expected<VerdictFrame, ConsensusError> {
            auto const verdict = Detail::DecodeEnum<HandshakeVerdict>(fields[0]);
            auto acceptor = Detail::HandshakeId(fields[1]);
            auto const signature = Detail::FixedField<SignatureSize>(fields[2]);
            if (!verdict.has_value())
                return std::unexpected { MalformedWireFrame("Verdict: the verdict names no known outcome") };
            if (!acceptor.has_value())
                return std::unexpected { MalformedWireFrame("Verdict: the id is empty, too long, or not UTF-8") };
            if (!signature.has_value())
                return std::unexpected { MalformedWireFrame("Verdict: the signature is the wrong width") };
            return VerdictFrame { .verdict = *verdict, .acceptor = *std::move(acceptor), .signature = *signature };
        });
}

} // namespace FastCache::Consensus::RaftWire
