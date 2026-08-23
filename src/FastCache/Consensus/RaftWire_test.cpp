// SPDX-License-Identifier: Apache-2.0
//
// The Raft peer wire format. Two properties matter more than the round trips:
// that the bytes are the same on every machine, and that a frame this build
// cannot interpret is *skippable* rather than fatal -- which is what keeps a
// fleet that is mid-upgrade from partitioning itself.
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using FastCache::Testing::Unwrap;

namespace
{

/// Frame a message, then decode it back through the header.
///
/// The whole path a transport takes, so a test asserting on the result is
/// asserting about what actually goes over a socket rather than about one half
/// of the codec.
/// @param message The message to round-trip.
/// @return The decoded message, or the error that stopped it.
[[nodiscard]] std::expected<RaftMessage, ConsensusError> RoundTrip(RaftMessage const& message)
{
    auto const frame = RaftWire::Encode(message);
    auto const header = RaftWire::DecodeHeader(frame);
    if (!header.has_value())
        return std::unexpected { MalformedWireFrame("header did not decode") };

    auto const payload = std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize);
    if (payload.size() != header->payloadLength)
        return std::unexpected { MalformedWireFrame("declared length disagrees with the frame") };
    return RaftWire::DecodeMessage(*header, payload);
}

} // namespace

TEST_CASE("Every message type round-trips, field for field", "[consensus][raft][wire]")
{
    // One exemplar per row of `MessageTable`, and **every field carries a
    // different value**. That is the whole design of this table rather than a
    // flourish: five of the eight encoder arms are near-copies of each other --
    // PreVote of RequestVote, InstallSnapshotResponse of AppendEntriesResponse --
    // and the mistake copying invites is a transposed field index, which two
    // fields sharing a value would let through. The four types added with pre-vote
    // and snapshots had no positive round trip at all until this existed, so an
    // arm encoding `lastIncludedTerm` where `lastIncludedIndex` belongs passed the
    // entire suite.
    auto const exemplars = std::vector<RaftMessage> {
        RequestVoteRequest { .term = Term { .value = 11 },
                             .candidateId = "rv-candidate",
                             .lastLogIndex = LogIndex { .value = 12 },
                             .lastLogTerm = Term { .value = 13 } },
        RequestVoteResponse { .term = Term { .value = 21 }, .decision = VoteDecision::Granted, .voterId = "rv-voter" },
        AppendEntriesRequest { .term = Term { .value = 31 },
                               .leaderId = "ae-leader",
                               .prevLogIndex = LogIndex { .value = 32 },
                               .prevLogTerm = Term { .value = 33 },
                               .entries = { LogEntry { .term = Term { .value = 34 },
                                                       .kind = EntryKind::Configuration,
                                                       .payload = BytesFromString("ae-payload") } },
                               .leaderCommit = LogIndex { .value = 35 } },
        AppendEntriesResponse { .term = Term { .value = 41 },
                                .result = AppendResult::Accepted,
                                .matchIndex = LogIndex { .value = 42 },
                                .followerId = "ae-follower" },
        PreVoteRequest { .term = Term { .value = 51 },
                         .candidateId = "pv-candidate",
                         .lastLogIndex = LogIndex { .value = 52 },
                         .lastLogTerm = Term { .value = 53 } },
        PreVoteResponse { .term = Term { .value = 61 }, .decision = VoteDecision::Granted, .voterId = "pv-voter" },
        InstallSnapshotRequest { .term = Term { .value = 71 },
                                 .leaderId = "is-leader",
                                 .lastIncludedIndex = LogIndex { .value = 72 },
                                 .lastIncludedTerm = Term { .value = 73 },
                                 .members = { "is-m1", "is-m2" },
                                 .state = BytesFromString("is-state") },
        InstallSnapshotResponse { .term = Term { .value = 81 },
                                  .result = AppendResult::Accepted,
                                  .matchIndex = LogIndex { .value = 82 },
                                  .followerId = "is-follower" },
    };

    // A ninth row added without an exemplar fails here rather than going quietly
    // untested, which is how four of these came to be uncovered in the first place.
    REQUIRE(exemplars.size() == RaftWire::MessageTable.size());

    auto covered = std::vector<std::uint8_t> {};
    for (auto const& sent: exemplars)
    {
        auto const frame = RaftWire::Encode(sent);
        auto const header = RaftWire::DecodeHeader(frame);
        REQUIRE(header.has_value());
        covered.push_back(Unwrap(header).kindRaw);

        // Compared whole. Field-by-field checks are what the copied arms already
        // survived; equality on the message cannot be satisfied by a subset.
        auto const got = RoundTrip(sent);
        REQUIRE(got.has_value());
        CHECK(*got == sent);
    }

    // The types the exemplars produced are exactly the table's, so an exemplar
    // duplicating a type -- the natural slip when one is copied from the next --
    // cannot stand in for the row it left out.
    for (auto const& row: RaftWire::MessageTable)
        CHECK(std::ranges::count(covered, static_cast<std::uint8_t>(row.type)) == 1);
}

TEST_CASE("An AppendEntries carries its entries verbatim", "[consensus][raft][wire]")
{
    AppendEntriesRequest sent { .term = Term { .value = 4 },
                                .leaderId = "n1",
                                .prevLogIndex = LogIndex { .value = 2 },
                                .prevLogTerm = Term { .value = 3 },
                                .entries = {},
                                .leaderCommit = LogIndex { .value = 2 } };
    sent.entries = {
        LogEntry { .term = Term { .value = 3 }, .kind = EntryKind::Command, .payload = FastCache::BytesFromString("first") },
        LogEntry { .term = Term { .value = 4 }, .kind = EntryKind::NoOp, .payload = {} },
        LogEntry { .term = Term { .value = 4 }, .kind = EntryKind::Command, .payload = FastCache::BytesFromString("third") },
    };

    auto const got = RoundTrip(RaftMessage { sent });
    REQUIRE(got.has_value());
    REQUIRE(std::holds_alternative<AppendEntriesRequest>(*got));

    auto const& m = std::get<AppendEntriesRequest>(*got);
    CHECK(m.term == sent.term);
    CHECK(m.leaderId == sent.leaderId);
    CHECK(m.prevLogIndex == sent.prevLogIndex);
    CHECK(m.prevLogTerm == sent.prevLogTerm);
    CHECK(m.leaderCommit == sent.leaderCommit);

    // LogEntry has value equality, so this compares term, kind and payload of
    // every entry -- including that a NoOp stays a NoOp. Losing the kind would
    // deliver consensus' own entry to the application as a command.
    CHECK(m.entries == sent.entries);
}

TEST_CASE("A heartbeat is an AppendEntries with no entries", "[consensus][raft][wire]")
{
    // Not a message type of its own, on the wire or anywhere else: keeping them
    // one thing is what makes a heartbeat also the mechanism that discovers a
    // divergent follower.
    RaftMessage const sent { AppendEntriesRequest { .term = Term { .value = 5 },
                                                    .leaderId = "n1",
                                                    .prevLogIndex = LogIndex { .value = 8 },
                                                    .prevLogTerm = Term { .value = 5 },
                                                    .entries = {},
                                                    .leaderCommit = LogIndex { .value = 8 } } };
    auto const got = RoundTrip(sent);
    REQUIRE(got.has_value());
    REQUIRE(std::holds_alternative<AppendEntriesRequest>(*got));
    CHECK(std::get<AppendEntriesRequest>(*got).entries.empty());
}

TEST_CASE("An entry payload survives bytes that are not text", "[consensus][raft][wire]")
{
    // The log is opaque to consensus, so a payload may hold anything -- including
    // an embedded NUL, which is what a length-delimited field exists to carry and
    // what a NUL-terminated one would silently truncate.
    std::vector<std::byte> const raw { std::byte { 0x00 }, std::byte { 0xFF }, std::byte { 0x00 }, std::byte { 0x41 } };
    RaftMessage const sent { AppendEntriesRequest {
        .term = Term { .value = 1 },
        .leaderId = "n1",
        .prevLogIndex = LogIndex::BeforeFirst(),
        .prevLogTerm = Term::None(),
        .entries = { LogEntry { .term = Term { .value = 1 }, .kind = EntryKind::Command, .payload = raw } },
        .leaderCommit = LogIndex::BeforeFirst() } };

    auto const got = RoundTrip(sent);
    REQUIRE(got.has_value());
    auto const& entries = std::get<AppendEntriesRequest>(*got).entries;
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].payload == raw);
}

TEST_CASE("The frame header is exactly what the format documents", "[consensus][raft][wire]")
{
    // Pinned against literals rather than a round trip. A host-order length
    // round-trips perfectly on one machine and produces a frame the peer cannot
    // read, which is precisely the failure a round-trip test cannot see -- and
    // the magic and type codes are a published contract that must not drift.
    RaftMessage const sent { RequestVoteResponse {
        .term = Term { .value = 0x0102 }, .decision = VoteDecision::Granted, .voterId = "ab" } };
    auto const frame = RaftWire::Encode(sent);

    REQUIRE(frame.size() > RaftWire::HeaderSize);
    CHECK(frame[0] == std::byte { 0xFA });
    CHECK(frame[1] == std::byte { RaftWire::CurrentVersion });
    CHECK(frame[2] == std::byte { 0x02 }); // RequestVoteResponse

    // payloadLength, big-endian: three fields of 8, 1 and 2 bytes, each with a
    // four-byte prefix.
    constexpr std::uint32_t Expected = (4 + 8) + (4 + 1) + (4 + 2);
    CHECK(frame[3] == std::byte { 0x00 });
    CHECK(frame[4] == std::byte { 0x00 });
    CHECK(frame[5] == std::byte { 0x00 });
    CHECK(frame[6] == std::byte { Expected });

    CHECK(frame.size() == RaftWire::HeaderSize + Expected);

    // The term is big-endian inside its field: 0x0102 must not read back as
    // 0x0201 on the peer.
    CHECK(frame[RaftWire::HeaderSize + 4 + 6] == std::byte { 0x01 });
    CHECK(frame[RaftWire::HeaderSize + 4 + 7] == std::byte { 0x02 });
}

TEST_CASE("A header decodes only when the reader is in sync", "[consensus][raft][wire]")
{
    auto const frame = RaftWire::Encode(RaftMessage {
        RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Denied, .voterId = "n1" } });

    SECTION("a good frame")
    {
        auto const header = RaftWire::DecodeHeader(frame);
        REQUIRE(header.has_value());
        CHECK(Unwrap(header).version == RaftWire::CurrentVersion);
        CHECK(Unwrap(header).kindRaw == static_cast<std::uint8_t>(RaftWire::MessageType::RequestVoteResponse));
        CHECK(Unwrap(header).payloadLength == frame.size() - RaftWire::HeaderSize);
    }

    SECTION("a wrong magic is refused")
    {
        // The one condition under which the reader cannot find where the frame
        // ends, so it cannot skip and must not guess. A compile-cache frame
        // arriving on this port is the realistic case.
        auto wrong = frame;
        wrong[0] = std::byte { 0xFC };
        CHECK_FALSE(RaftWire::DecodeHeader(wrong).has_value());
    }

    SECTION("a short buffer is refused")
    {
        for (auto shortLength = std::size_t { 0 }; shortLength < RaftWire::HeaderSize; ++shortLength)
        {
            auto const partial = std::span<std::byte const> { frame }.first(shortLength);
            CHECK_FALSE(RaftWire::DecodeHeader(partial).has_value());
        }
    }
}

TEST_CASE("An unknown message type is skippable rather than fatal", "[consensus][raft][wire]")
{
    // The mixed-fleet property this format exists to provide. A node running a
    // newer build sends a type this one has never heard of; the header still
    // decodes, so the reader knows exactly how many bytes to step over and the
    // next frame -- which it does understand -- still arrives.
    auto frame = RaftWire::Encode(RaftMessage {
        RequestVoteResponse { .term = Term { .value = 2 }, .decision = VoteDecision::Granted, .voterId = "n1" } });
    frame[2] = std::byte { 0x7F };

    auto const header = RaftWire::DecodeHeader(frame);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).payloadLength == frame.size() - RaftWire::HeaderSize);

    auto const decoded =
        RaftWire::DecodeMessage(Unwrap(header), std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize));
    REQUIRE_FALSE(decoded.has_value());

    // Reported as its own code, not as corruption: a rolling upgrade must not
    // look like a peer sending garbage, or the one log line that matters gets
    // lost among the ones that do not.
    CHECK(decoded.error().code == ConsensusErrorCode::UnknownMessageType);
    CHECK(decoded.error().context.contains("7F"));
}

TEST_CASE("An unsupported version names the range that would have worked", "[consensus][raft][wire]")
{
    auto const frame =
        RaftWire::Encode(RaftMessage { RequestVoteResponse {
                             .term = Term { .value = 1 }, .decision = VoteDecision::Denied, .voterId = "n1" } },
                         RaftWire::CurrentVersion + 1);

    auto const header = RaftWire::DecodeHeader(frame);
    REQUIRE(header.has_value());

    auto const decoded =
        RaftWire::DecodeMessage(Unwrap(header), std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);

    // A rejection that cannot say what would have worked cannot be acted on.
    CHECK(decoded.error().context.contains(std::to_string(RaftWire::MinSupportedVersion)));
    CHECK(decoded.error().context.contains(std::to_string(RaftWire::CurrentVersion)));
}

TEST_CASE("A malformed payload is refused rather than half-read", "[consensus][raft][wire]")
{
    auto const good = RaftWire::Encode(RaftMessage { RequestVoteRequest { .term = Term { .value = 1 },
                                                                          .candidateId = "n1",
                                                                          .lastLogIndex = LogIndex { .value = 1 },
                                                                          .lastLogTerm = Term { .value = 1 } } });
    auto const header = RaftWire::DecodeHeader(good);
    REQUIRE(header.has_value());
    auto const payload = std::span<std::byte const> { good }.subspan(RaftWire::HeaderSize);

    SECTION("a truncated payload")
    {
        auto const decoded = RaftWire::DecodeMessage(Unwrap(header), payload.first(payload.size() - 1));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("a payload with an extra trailing byte")
    {
        std::vector<std::byte> padded { payload.begin(), payload.end() };
        padded.push_back(std::byte { 0 });
        auto const decoded = RaftWire::DecodeMessage(Unwrap(header), padded);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("an empty payload")
    {
        auto const decoded = RaftWire::DecodeMessage(Unwrap(header), {});
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("the refusal names the message it was reading")
    {
        auto const decoded = RaftWire::DecodeMessage(Unwrap(header), {});
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().context.contains("RequestVote"));
    }
}

TEST_CASE("A counter field of the wrong width is refused", "[consensus][raft][wire]")
{
    // A term is eight bytes. A peer that sent four would otherwise have its term
    // silently misread, and a wrong term is a wrong election.
    auto const term = WireFields::ToBigEndian<std::uint32_t>(1);
    auto const index = WireFields::ToBigEndian<std::uint64_t>(1);
    std::array<std::span<std::byte const>, 4> const fields { std::span<std::byte const> { term },
                                                             WireFields::AsBytes("n1"),
                                                             std::span<std::byte const> { index },
                                                             std::span<std::byte const> { index } };
    auto const payload = WireFields::Encode(fields);

    RaftWire::FrameHeader const header { .version = RaftWire::CurrentVersion,
                                         .kindRaw = static_cast<std::uint8_t>(RaftWire::MessageType::RequestVote),
                                         .payloadLength = static_cast<std::uint32_t>(payload.size()) };

    auto const decoded = RaftWire::DecodeMessage(header, payload);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
}

TEST_CASE("An enum byte naming no enumerator is refused", "[consensus][raft][wire]")
{
    // Casting an arbitrary byte into an enumeration produces a value no switch
    // handles and no invariant covers, and the byte came from a peer. So it is a
    // malformed frame to refuse, never a precondition to assert on.
    SECTION("a vote decision")
    {
        auto const term = WireFields::ToBigEndian<std::uint64_t>(1);
        auto const bogus = WireFields::ToBigEndian<std::uint8_t>(0x7F);
        std::array<std::span<std::byte const>, 3> const fields { std::span<std::byte const> { term },
                                                                 std::span<std::byte const> { bogus },
                                                                 WireFields::AsBytes("n1") };
        auto const payload = WireFields::Encode(fields);
        RaftWire::FrameHeader const header { .version = RaftWire::CurrentVersion,
                                             .kindRaw =
                                                 static_cast<std::uint8_t>(RaftWire::MessageType::RequestVoteResponse),
                                             .payloadLength = static_cast<std::uint32_t>(payload.size()) };

        auto const decoded = RaftWire::DecodeMessage(header, payload);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("an entry kind")
    {
        auto const entryTerm = WireFields::ToBigEndian<std::uint64_t>(1);
        auto const bogusKind = WireFields::ToBigEndian<std::uint8_t>(0x7F);
        std::array<std::span<std::byte const>, 3> const entryFields { std::span<std::byte const> { entryTerm },
                                                                      std::span<std::byte const> { bogusKind },
                                                                      WireFields::AsBytes("payload") };
        auto const entry = WireFields::Encode(entryFields);
        std::array<std::span<std::byte const>, 1> const entryList { std::span<std::byte const> { entry } };
        auto const entries = WireFields::Encode(entryList);

        auto const counter = WireFields::ToBigEndian<std::uint64_t>(1);
        std::array<std::span<std::byte const>, 6> const fields {
            std::span<std::byte const> { counter }, WireFields::AsBytes("n1"),
            std::span<std::byte const> { counter }, std::span<std::byte const> { counter },
            std::span<std::byte const> { counter }, std::span<std::byte const> { entries }
        };
        auto const payload = WireFields::Encode(fields);
        RaftWire::FrameHeader const header { .version = RaftWire::CurrentVersion,
                                             .kindRaw = static_cast<std::uint8_t>(RaftWire::MessageType::AppendEntries),
                                             .payloadLength = static_cast<std::uint32_t>(payload.size()) };

        auto const decoded = RaftWire::DecodeMessage(header, payload);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }
}

TEST_CASE("The message table and the decoder agree on every type", "[consensus][raft][wire]")
{
    // Walks the table rather than listing the types again, so a row added
    // without a decoder arm is caught here instead of at a peer.
    for (auto const& row: RaftWire::MessageTable)
    {
        CAPTURE(row.name);
        auto const* const found = RaftWire::FindMessage(static_cast<std::uint8_t>(row.type));
        REQUIRE(found != nullptr);
        CHECK(found->fieldCount == row.fieldCount);

        // An empty payload cannot satisfy any real message, so every row must
        // report a *malformed* frame -- never "unknown type", which would mean
        // the table and `FindMessage` disagree.
        RaftWire::FrameHeader const header { .version = RaftWire::CurrentVersion,
                                             .kindRaw = static_cast<std::uint8_t>(row.type),
                                             .payloadLength = 0 };
        auto const decoded = RaftWire::DecodeMessage(header, {});
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }
}

TEST_CASE("Every enum field carries both of its values", "[consensus][raft][wire]")
{
    // The exemplar table above fixes ONE value per enum field, which is what keeps
    // its values all distinct and so makes a transposition visible; this sweeps
    // the other. Splitting the two questions is deliberate — a single table trying
    // to answer both would have to repeat a value, and then a transposed pair
    // would encode identically.
    for (auto const decision: { VoteDecision::Denied, VoteDecision::Granted })
    {
        for (auto const& sent:
             { RaftMessage { RequestVoteResponse { .term = Term { .value = 5 }, .decision = decision, .voterId = "rv" } },
               RaftMessage { PreVoteResponse { .term = Term { .value = 5 }, .decision = decision, .voterId = "pv" } } })
        {
            auto const got = RoundTrip(sent);
            REQUIRE(got.has_value());
            CHECK(*got == sent);
        }
    }

    for (auto const result: { AppendResult::Rejected, AppendResult::Accepted })
    {
        for (auto const& sent: { RaftMessage { AppendEntriesResponse { .term = Term { .value = 6 },
                                                                       .result = result,
                                                                       .matchIndex = LogIndex { .value = 7 },
                                                                       .followerId = "ae" } },
                                 RaftMessage { InstallSnapshotResponse { .term = Term { .value = 6 },
                                                                         .result = result,
                                                                         .matchIndex = LogIndex { .value = 7 },
                                                                         .followerId = "is" } } })
        {
            auto const got = RoundTrip(sent);
            REQUIRE(got.has_value());
            CHECK(*got == sent);
        }
    }
}

TEST_CASE("A snapshot with no members and no state round-trips", "[consensus][raft][wire]")
{
    // The degenerate shape of the only message carrying two variable-length
    // fields: empty ones must survive as empty rather than collapsing into each
    // other, which is exactly what a length-prefixed field grammar is for.
    RaftMessage const sent { InstallSnapshotRequest { .term = Term { .value = 2 },
                                                      .leaderId = "n1",
                                                      .lastIncludedIndex = LogIndex { .value = 9 },
                                                      .lastIncludedTerm = Term { .value = 1 },
                                                      .members = {},
                                                      .state = {} } };
    auto const got = RoundTrip(sent);
    REQUIRE(got.has_value());
    CHECK(*got == sent);
}
