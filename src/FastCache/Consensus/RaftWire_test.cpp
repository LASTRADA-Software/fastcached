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
#include <ranges>
#include <span>
#include <string>
#include <utility>
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
                                 .configuration = { .voters = { "is-m1", "is-m2" }, .learners = { "is-l1" } },
                                 .state = BytesFromString("is-state") },
        InstallSnapshotResponse { .term = Term { .value = 81 },
                                  .result = AppendResult::Accepted,
                                  .matchIndex = LogIndex { .value = 82 },
                                  .followerId = "is-follower" },
    };

    // A ninth row added without an exemplar fails here rather than going quietly
    // untested, which is how four of these came to be uncovered in the first place.
    // The handshake rows are not messages and have their own round trip below.
    auto const sessionRows = std::ranges::count_if(
        RaftWire::MessageTable, [](RaftWire::MessageDescriptor const& row) { return !row.phase.IsHandshake(); });
    REQUIRE(std::cmp_equal(exemplars.size(), sessionRows));

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
        if (!row.phase.IsHandshake())
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
        for (auto const shortLength: std::views::iota(std::size_t { 0 }, RaftWire::HeaderSize))
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
                                                      .configuration = {},
                                                      .state = {} } };
    auto const got = RoundTrip(sent);
    REQUIRE(got.has_value());
    CHECK(*got == sent);
}

namespace
{

/// A nonce whose every byte differs, so a mis-offset shows.
/// @param base The first byte.
/// @return The nonce.
[[nodiscard]] Nonce DistinctNonce(unsigned base)
{
    Nonce nonce {};
    for (std::size_t index = 0; auto& byte: nonce)
        byte = static_cast<std::byte>(base + index++);
    return nonce;
}

/// A tag whose every byte differs from any nonce `DistinctNonce` makes below 0x80.
/// @return The tag.
[[nodiscard]] Sha256::Digest DistinctTag()
{
    Sha256::Digest tag {};
    for (std::size_t index = 0; auto& byte: tag)
        byte = static_cast<std::byte>(0x80 + index++);
    return tag;
}

/// Split a handshake frame into its header and payload.
/// @param frame The frame.
/// @return The header and a view of the payload.
[[nodiscard]] std::pair<RaftWire::FrameHeader, std::span<std::byte const>> Split(std::vector<std::byte> const& frame)
{
    auto const header = RaftWire::DecodeHeader(frame);
    REQUIRE(header.has_value());
    auto const payload = std::span<std::byte const> { frame }.subspan(RaftWire::HeaderSize);
    REQUIRE(payload.size() == Unwrap(header).payloadLength);
    return { Unwrap(header), payload };
}

} // namespace

TEST_CASE("Every handshake frame round-trips, field for field", "[consensus][raft][wire][handshake]")
{
    // Distinct values in every field, for the exemplar table's reason above: two fields
    // sharing a value would let a transposition through, and a proof's two ids and its
    // nonce and tag are exactly the fields a copied arm transposes.
    RaftWire::ChallengeFrame const challenge { .nonce = DistinctNonce(0x10) };
    RaftWire::ProofFrame const proof {
        .dialler = "the-dialler", .target = "the-target", .nonce = DistinctNonce(0x40), .tag = DistinctTag()
    };
    RaftWire::VerdictFrame const verdict { .verdict = RaftWire::HandshakeVerdict::OwnId,
                                           .acceptor = "the-acceptor",
                                           .tag = DistinctTag() };

    auto const challengeFrame = RaftWire::EncodeChallenge(challenge);
    auto const proofFrame = RaftWire::EncodeProof(proof);
    auto const verdictFrame = RaftWire::EncodeVerdict(verdict);

    auto const [challengeHeader, challengePayload] = Split(challengeFrame);
    auto const [proofHeader, proofPayload] = Split(proofFrame);
    auto const [verdictHeader, verdictPayload] = Split(verdictFrame);

    CHECK(challengeHeader.kindRaw == static_cast<std::uint8_t>(RaftWire::MessageType::Challenge));
    CHECK(proofHeader.kindRaw == static_cast<std::uint8_t>(RaftWire::MessageType::Proof));
    CHECK(verdictHeader.kindRaw == static_cast<std::uint8_t>(RaftWire::MessageType::Verdict));

    CHECK(RaftWire::DecodeChallenge(challengeHeader, challengePayload) == challenge);
    CHECK(RaftWire::DecodeProof(proofHeader, proofPayload) == proof);
    CHECK(RaftWire::DecodeVerdict(verdictHeader, verdictPayload) == verdict);
}

TEST_CASE("Every verdict travels as its own byte", "[consensus][raft][wire][handshake]")
{
    for (auto const decided: { RaftWire::HandshakeVerdict::WrongTarget,
                               RaftWire::HandshakeVerdict::OwnId,
                               RaftWire::HandshakeVerdict::Accepted })
    {
        RaftWire::VerdictFrame const verdict { .verdict = decided, .acceptor = "a", .tag = DistinctTag() };
        auto const frame = RaftWire::EncodeVerdict(verdict);
        auto const [header, payload] = Split(frame);
        CHECK(RaftWire::DecodeVerdict(header, payload) == verdict);
    }
}

TEST_CASE("A handshake frame this reader did not ask for is refused", "[consensus][raft][wire][handshake]")
{
    auto const proofFrame =
        RaftWire::EncodeProof({ .dialler = "d", .target = "a", .nonce = DistinctNonce(0x01), .tag = DistinctTag() });
    auto const [proofHeader, proofPayload] = Split(proofFrame);

    SECTION("another handshake type in its place")
    {
        auto const decoded = RaftWire::DecodeChallenge(proofHeader, proofPayload);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("a Raft message in its place, which is what a build from before the handshake sends first")
    {
        auto const message = RaftWire::Encode(RaftMessage {
            RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n1" } });
        auto const header = RaftWire::DecodeHeader(message);
        REQUIRE(header.has_value());
        auto const decoded =
            RaftWire::DecodeProof(Unwrap(header), std::span<std::byte const> { message }.subspan(RaftWire::HeaderSize));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }

    SECTION("a handshake frame where a Raft message belongs")
    {
        auto const decoded = RaftWire::DecodeMessage(proofHeader, proofPayload);
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error().code == ConsensusErrorCode::MalformedFrame);
    }
}

TEST_CASE("A handshake at the version before the handshake existed is refused", "[consensus][raft][wire][handshake]")
{
    // Version 1 authenticated nothing. A build that still accepted it would be the
    // per-connection fallback #1308 exists to refuse, so the floor moved with the grammar.
    CHECK_FALSE(RaftWire::IsSupported(1));
    CHECK(RaftWire::IsSupported(RaftWire::CurrentVersion));

    auto const frame = RaftWire::EncodeProof(
        { .dialler = "d", .target = "a", .nonce = DistinctNonce(0x01), .tag = DistinctTag() }, /*version=*/1);
    auto const [header, payload] = Split(frame);
    auto const decoded = RaftWire::DecodeProof(header, payload);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);
}

TEST_CASE("A peer that spells a configuration without learners is refused at its handshake",
          "[consensus][raft][wire][handshake][learner]")
{
    // Version 3 is #1449's: a configuration is two nested lists, voters and learners,
    // where version 2 had one flat one. The frame's arity did not move, so nothing but
    // the version tells a version 2 peer's configuration from this build's -- and the
    // version is refused at the handshake, before a configuration can be misread.
    //
    // The VALUE is pinned beside the name: every other case here spells
    // `CurrentVersion`, which would go on passing if the constant moved back.
    CHECK(RaftWire::CurrentVersion == 3);
    CHECK(RaftWire::MinSupportedVersion == 3);
    CHECK_FALSE(RaftWire::IsSupported(2));

    auto const frame = RaftWire::EncodeProof(
        { .dialler = "d", .target = "a", .nonce = DistinctNonce(0x01), .tag = DistinctTag() }, /*version=*/2);
    auto const [header, payload] = Split(frame);
    auto const decoded = RaftWire::DecodeProof(header, payload);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ConsensusErrorCode::UnsupportedVersion);
}

TEST_CASE("A handshake frame is refused before any field is trusted", "[consensus][raft][wire][handshake]")
{
    auto const refusedProof = [](RaftWire::ProofFrame const& proof) {
        auto const frame = RaftWire::EncodeProof(proof);
        auto const [header, payload] = Split(frame);
        return !RaftWire::DecodeProof(header, payload).has_value();
    };
    auto const good =
        RaftWire::ProofFrame { .dialler = "d", .target = "a", .nonce = DistinctNonce(0x01), .tag = DistinctTag() };
    CHECK_FALSE(refusedProof(good));

    SECTION("an empty id")
    {
        auto proof = good;
        proof.dialler.clear();
        CHECK(refusedProof(proof));
    }

    SECTION("an id that is not text")
    {
        auto proof = good;
        proof.target = std::string { "\xC3\x28" };
        CHECK(refusedProof(proof));
    }

    SECTION("an id at the bound is carried, and one past it is not")
    {
        auto proof = good;
        proof.dialler = std::string(RaftWire::MaxHandshakeIdBytes, 'x');
        CHECK_FALSE(refusedProof(proof));
        proof.dialler.push_back('x');
        CHECK(refusedProof(proof));
    }

    SECTION("a nonce or a tag of the wrong width")
    {
        auto const frame = RaftWire::Detail::Frame<RaftWire::MessageType::Proof>(
            RaftWire::CurrentVersion,
            std::array { WireFields::AsBytes(std::string_view { "d" }),
                         WireFields::AsBytes(std::string_view { "a" }),
                         WireFields::AsBytes(std::string_view { "short" }),
                         std::span<std::byte const> { DistinctTag() } });
        auto const [header, payload] = Split(frame);
        CHECK_FALSE(RaftWire::DecodeProof(header, payload).has_value());
    }

    SECTION("a verdict byte naming no verdict")
    {
        auto frame = RaftWire::EncodeVerdict(
            { .verdict = RaftWire::HandshakeVerdict::Accepted, .acceptor = "a", .tag = DistinctTag() });
        // The verdict is the first field: header, then a four-byte length, then the byte.
        frame[RaftWire::HeaderSize + WireFields::FieldPrefixSize] =
            std::byte { static_cast<std::uint8_t>(RaftWire::HandshakeVerdict::Last) };
        auto const [header, payload] = Split(frame);
        CHECK_FALSE(RaftWire::DecodeVerdict(header, payload).has_value());
    }
}

TEST_CASE("A handshake payload over its row's ceiling is refused", "[consensus][raft][wire][handshake]")
{
    // The reader refuses a declared length over the ceiling before it buffers anything;
    // this is the decoder refusing one that somehow arrived anyway, so the bound does not
    // depend on every reader remembering to ask first.
    auto const* const row = RaftWire::FindMessage(static_cast<std::uint8_t>(RaftWire::MessageType::Challenge));
    REQUIRE(row != nullptr);
    REQUIRE(row->phase.IsHandshake());

    std::vector<std::byte> const oversized(row->phase.Ceiling() + 1);
    RaftWire::FrameHeader const header { .version = RaftWire::CurrentVersion,
                                         .kindRaw = static_cast<std::uint8_t>(RaftWire::MessageType::Challenge),
                                         .payloadLength = static_cast<std::uint32_t>(oversized.size()) };
    auto const decoded = RaftWire::DecodeChallenge(header, oversized);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().context.contains("ceiling"));
}

TEST_CASE("Every handshake row has a ceiling and every Raft message has none", "[consensus][raft][wire][handshake]")
{
    // The phase of each row, stated: the handshake is exactly these three, and each
    // declares the payload a stranger may make this node buffer for it.
    for (auto const& row: RaftWire::MessageTable)
    {
        CAPTURE(row.name);
        auto const handshake = row.type == RaftWire::MessageType::Challenge || row.type == RaftWire::MessageType::Proof
                               || row.type == RaftWire::MessageType::Verdict;
        CHECK(row.phase.IsHandshake() == handshake);
        if (handshake)
        {
            CHECK(row.phase.Ceiling() > 0);
            CHECK(row.phase.Ceiling() <= RaftWire::MaxHandshakePayload);
        }
    }
}
