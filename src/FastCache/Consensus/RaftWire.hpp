// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Ranges.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

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
/// [u8 magic=0xFA][u8 version][u8 type][u32 payloadLength]
/// payload := field*        where field := [u32 len][len bytes]
/// ```
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
/// ## Why there is no handshake
///
/// Nodes exchange messages continuously and in both directions, so a version
/// negotiation would have to be re-run on every reconnect — and a reconnect
/// happens exactly when the cluster is already unhealthy. The version travels in
/// every frame instead, which costs one byte and makes each frame independently
/// interpretable. `CompileCacheWire` reaches the same conclusion from the
/// opposite direction (a fresh connection per operation); the shared consequence
/// is that a receiver must be able to answer "can I read this frame?" from the
/// frame alone.

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
inline constexpr WireVersion CurrentVersion = 1;

/// The oldest version this build still accepts. Equal to `CurrentVersion` while
/// only one exists; widen the range when a second ships and this build can still
/// decode the older shape.
inline constexpr WireVersion MinSupportedVersion = 1;

/// Size of the fixed frame header: magic, version, type, payload length.
inline constexpr std::size_t HeaderSize = WireFrame::HeaderSize;

/// Wire type codes. One byte, third in the frame header.
///
/// Explicit values because they are a published contract: a renumbering that
/// looked like a harmless reordering would make two builds disagree about what
/// every frame means. Numbered from `0x01` so a zeroed buffer is not a valid
/// type as well as not carrying a valid magic.
enum class MessageType : std::uint8_t
{
    RequestVote = 0x01,             ///< Candidate asking for a vote (§5.2).
    RequestVoteResponse = 0x02,     ///< A voter's answer.
    AppendEntries = 0x03,           ///< Leader replicating; a heartbeat when empty (§5.3).
    AppendEntriesResponse = 0x04,   ///< A follower's answer.
    PreVote = 0x05,                 ///< Asking whether an election could be won (thesis §9.6).
    PreVoteResponse = 0x06,         ///< A voter's answer to that question.
    InstallSnapshot = 0x07,         ///< State a compacted log can no longer replay.
    InstallSnapshotResponse = 0x08, ///< A follower's answer to that.
};

/// What one message type needs, as a row rather than as a branch.
///
/// The field count lives here so it has one home instead of being spelled again
/// at the encoder and the decoder — the arity is the one thing those two must
/// agree on, and it is exactly what drifts when each states it separately.
struct MessageDescriptor
{
    MessageType type {};       ///< The wire code.
    std::string_view name;     ///< For a log line naming what was refused.
    std::size_t fieldCount {}; ///< How many top-level fields the payload holds.
};

/// Every message type this build knows. Adding one is adding a row.
inline constexpr std::array MessageTable {
    MessageDescriptor { .type = MessageType::RequestVote, .name = "RequestVote", .fieldCount = 4 },
    MessageDescriptor { .type = MessageType::RequestVoteResponse, .name = "RequestVoteResponse", .fieldCount = 3 },
    MessageDescriptor { .type = MessageType::AppendEntries, .name = "AppendEntries", .fieldCount = 6 },
    MessageDescriptor { .type = MessageType::AppendEntriesResponse, .name = "AppendEntriesResponse", .fieldCount = 4 },
    MessageDescriptor { .type = MessageType::PreVote, .name = "PreVote", .fieldCount = 4 },
    MessageDescriptor { .type = MessageType::PreVoteResponse, .name = "PreVoteResponse", .fieldCount = 3 },
    MessageDescriptor { .type = MessageType::InstallSnapshot, .name = "InstallSnapshot", .fieldCount = 6 },
    MessageDescriptor { .type = MessageType::InstallSnapshotResponse, .name = "InstallSnapshotResponse", .fieldCount = 4 },
};

/// How many fields one log entry encodes as: term, kind, payload.
inline constexpr std::size_t LogEntryFieldCount = 3;

/// Look a raw type byte up in the table.
/// @param kindRaw The byte from a frame header.
/// @return The descriptor, or nullptr when this build does not know the type.
[[nodiscard]] constexpr MessageDescriptor const* FindMessage(std::uint8_t kindRaw) noexcept
{
    return FindOrNull(
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
    /// `Consensus::DecodeWireEnum`, whose bound lives beside the enum so this
    /// decoder and `FileRaftStorage`'s cannot disagree about what the highest
    /// enumerator is.
    /// @tparam E The enumeration.
    /// @param field The field's bytes.
    /// @return The value, or nullopt when the field is the wrong width or the
    ///         byte names no enumerator.
    template <typename E>
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
                auto const members = Membership::Encode(m.members);
                std::array const fields { std::span<std::byte const> { term },      WireFields::AsBytes(m.leaderId),
                                          std::span<std::byte const> { lastIndex }, std::span<std::byte const> { lastTerm },
                                          std::span<std::byte const> { members },   std::span<std::byte const> { m.state } };
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

            auto members = Membership::Decode((*fields)[4]);
            if (!members.has_value())
                return malformed("the member list is malformed");

            return RaftMessage { InstallSnapshotRequest {
                .term = Term { .value = *term },
                .leaderId = NodeId { WireFields::AsStringView((*fields)[1]) },
                .lastIncludedIndex = LogIndex { .value = *lastIndex },
                .lastIncludedTerm = Term { .value = *lastTerm },
                .members = *std::move(members),
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
    }

    // Unreachable: `descriptor` came from the table, so its type is one of the
    // cases above. Spelled out rather than left to fall off the end, which is
    // undefined behaviour on a corrupted table.
    return malformed("the message table names a type this decoder does not handle");
}

} // namespace FastCache::Consensus::RaftWire
