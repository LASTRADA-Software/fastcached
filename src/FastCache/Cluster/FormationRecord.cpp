// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Consensus/DurableFile.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Platform/FileTrust.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

namespace
{
    /// Fields after the header: the own cluster's id and creation time, then the four optional
    /// parts in `FormationRecord`'s order, then the asked fleets.
    constexpr std::size_t RecordFields = 7;

    /// The fixed bytes ahead of the fields: the magic, the format byte and the mode byte.
    constexpr std::size_t HeaderBytes = FormationRecordModeOffset + 1;

    /// The presence tag an optional part opens with.
    constexpr std::array AbsentTag { std::byte { 0 } };
    constexpr std::array PresentTag { std::byte { 1 } };

    /// Fields in a pending node's join target: the summary, the key that proved it, when it asked.
    constexpr std::size_t JoinTargetFields = 3;

    /// Fields in a fleet membership: the fleet's id, the roster and the fleet's creation time.
    constexpr std::size_t FleetMembershipFields = 3;

    /// Fields in one asked fleet: its id, the key that proved it, when this node asked, and whether it
    /// admitted this node (one byte, 0 or 1).
    constexpr std::size_t AskedJoinFields = 4;

    /// Fields in a rejection memo: the fleet's id and when it refused.
    constexpr std::size_t RejectionMemoFields = 2;

    /// Build the refusal for a record whose bytes are damaged.
    /// @param what What about them is wrong.
    /// @return The refusal.
    [[nodiscard]] ConsensusError Damaged(std::string_view what)
    {
        return MalformedWireFrame(std::format("the formation record is damaged: {}", what));
    }

    /// Encode an optional part as its presence tag, then its body when present.
    /// @param part The part.
    /// @param encodeBody Turns a present value into the one field that carries it.
    /// @return The part's blob.
    template <typename T, typename Encoder>
    [[nodiscard]] std::vector<std::byte> EncodePart(std::optional<T> const& part, Encoder encodeBody)
    {
        if (!part.has_value())
            return WireFields::Encode({ std::span<std::byte const> { AbsentTag } });
        auto const body = encodeBody(*part);
        return WireFields::Encode({ std::span<std::byte const> { PresentTag }, std::span<std::byte const> { body } });
    }

    /// Read an optional part written by `EncodePart`.
    /// @param blob The part's blob.
    /// @param what The part's name, for the refusal.
    /// @param decodeBody Reads the one field a present value was written as; nothing when damaged.
    /// @return The part, disengaged when it was written absent, or why it cannot be read.
    template <typename T, typename Decoder>
    [[nodiscard]] std::expected<std::optional<T>, ConsensusError> DecodePart(std::span<std::byte const> blob,
                                                                             std::string_view what,
                                                                             Decoder decodeBody)
    {
        if (auto const absent = WireFields::SplitExactly(blob, 1);
            absent.has_value() && std::ranges::equal((*absent)[0], AbsentTag))
            return std::optional<T> {};
        auto const present = WireFields::SplitExactly(blob, 2);
        if (!present.has_value() || !std::ranges::equal((*present)[0], PresentTag))
            return std::unexpected(Damaged(std::format("its {} is neither absent nor present", what)));
        auto body = decodeBody((*present)[1]);
        if (!body.has_value())
            return std::unexpected(Damaged(std::format("its {} cannot be read", what)));
        return body;
    }

    /// Text read out of a field, owning its characters.
    /// @param field The field.
    /// @return The text.
    [[nodiscard]] std::string OwnedText(std::span<std::byte const> field)
    {
        return std::string { AsStringView(field) };
    }

    /// A join target's body: the summary as it was proven, the key that proved it, when it asked.
    /// @param target The target.
    /// @return The body.
    [[nodiscard]] std::vector<std::byte> EncodeJoinTarget(JoinTarget const& target)
    {
        auto const summary = CompileCacheWire::EncodeFleetSummaryFields(target.summary);
        auto const asked = WireFields::ToBigEndian<std::uint64_t>(target.askedAtUnixSeconds);
        return WireFields::Encode({ std::span<std::byte const> { summary },
                                    std::span<std::byte const> { target.provenKey },
                                    std::span<std::byte const> { asked } });
    }

    /// Read a body `EncodeJoinTarget` wrote.
    /// @param body The body.
    /// @return The target, or nothing when damaged.
    [[nodiscard]] std::optional<JoinTarget> DecodeJoinTarget(std::span<std::byte const> body)
    {
        auto const fields = WireFields::SplitExactly(body, JoinTargetFields);
        if (!fields.has_value())
            return std::nullopt;
        // At the REPLY's cap, the larger: a target may have been proven by a seed's answer, and
        // the record keeps the summary exactly as it was proven.
        auto summary =
            CompileCacheWire::DecodeFleetSummaryFields((*fields)[0], CompileCacheWire::MaxFleetSummaryReplyMembers);
        auto const asked = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
        if (!summary.has_value() || (*fields)[1].size() != Ed25519PublicKeyBytes || !asked.has_value())
            return std::nullopt;
        auto target = JoinTarget { .summary = *std::move(summary), .provenKey = {}, .askedAtUnixSeconds = *asked };
        std::ranges::copy((*fields)[1], target.provenKey.begin());
        return target;
    }

    /// A fleet membership's body: the fleet's id, then the roster bytes as they were approved.
    /// @param fleet The membership.
    /// @return The body.
    [[nodiscard]] std::vector<std::byte> EncodeFleetMembership(FleetMembership const& fleet)
    {
        auto const created = WireFields::ToBigEndian<std::uint64_t>(fleet.createdAtUnixSeconds);
        return WireFields::Encode({ WireFields::AsBytes(fleet.clusterId),
                                    std::span<std::byte const> { fleet.roster },
                                    std::span<std::byte const> { created } });
    }

    /// Read a body `EncodeFleetMembership` wrote.
    /// @param body The body.
    /// @return The membership, or nothing when damaged.
    [[nodiscard]] std::optional<FleetMembership> DecodeFleetMembership(std::span<std::byte const> body)
    {
        auto const fields = WireFields::SplitExactly(body, FleetMembershipFields);
        if (!fields.has_value())
            return std::nullopt;
        auto const created = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
        if (!created.has_value())
            return std::nullopt;
        return FleetMembership { .clusterId = OwnedText((*fields)[0]),
                                 .roster = std::vector<std::byte> { (*fields)[1].begin(), (*fields)[1].end() },
                                 .createdAtUnixSeconds = *created };
    }

    /// A pending archive's body: the cluster id, as it stands.
    /// @param clusterId The cluster whose store awaits archiving.
    /// @return The body.
    [[nodiscard]] std::vector<std::byte> EncodeArchivePending(std::string const& clusterId)
    {
        auto const text = WireFields::AsBytes(clusterId);
        return std::vector<std::byte> { text.begin(), text.end() };
    }

    /// Read a body `EncodeArchivePending` wrote. Any bytes are a cluster id, the empty one included.
    /// @param body The body.
    /// @return The cluster id.
    [[nodiscard]] std::optional<std::string> DecodeArchivePending(std::span<std::byte const> body)
    {
        return OwnedText(body);
    }

    /// A rejection memo's body: the fleet's id, then when it refused.
    /// @param memo The memo.
    /// @return The body.
    [[nodiscard]] std::vector<std::byte> EncodeRejectionMemo(RejectionMemo const& memo)
    {
        auto const at = WireFields::ToBigEndian<std::uint64_t>(memo.atUnixSeconds);
        return WireFields::Encode({ WireFields::AsBytes(memo.clusterId), std::span<std::byte const> { at } });
    }

    /// Read a body `EncodeRejectionMemo` wrote.
    /// @param body The body.
    /// @return The memo, or nothing when damaged.
    [[nodiscard]] std::optional<RejectionMemo> DecodeRejectionMemo(std::span<std::byte const> body)
    {
        auto const fields = WireFields::SplitExactly(body, RejectionMemoFields);
        if (!fields.has_value())
            return std::nullopt;
        auto const at = WireFields::FromBigEndian<std::uint64_t>((*fields)[1]);
        if (!at.has_value())
            return std::nullopt;
        return RejectionMemo { .clusterId = OwnedText((*fields)[0]), .atUnixSeconds = *at };
    }

    /// The asked fleets' field: one nested entry per memo, oldest first.
    /// @param asked The memos.
    /// @return The field.
    [[nodiscard]] std::vector<std::byte> EncodeAskedJoins(std::vector<AskedJoin> const& asked)
    {
        auto entries = std::vector<std::vector<std::byte>> {};
        entries.reserve(asked.size());
        for (auto const& memo: asked)
        {
            auto const at = WireFields::ToBigEndian<std::uint64_t>(memo.askedAtUnixSeconds);
            auto const admitted = std::array { static_cast<std::byte>(memo.admitted ? 1 : 0) };
            entries.push_back(WireFields::Encode({ WireFields::AsBytes(memo.clusterId),
                                                   std::span<std::byte const> { memo.provenKey },
                                                   std::span<std::byte const> { at },
                                                   std::span<std::byte const> { admitted } }));
        }
        auto const views = std::vector<std::span<std::byte const>> { entries.begin(), entries.end() };
        return WireFields::Encode(WireFields::FieldList { views });
    }

    /// Read the field `EncodeAskedJoins` wrote.
    ///
    /// More than `MaxAskedJoins` is damage, since no build writes more; so is an entry whose key is
    /// not a key's width, whose time is not eight bytes, or whose admitted byte is not 0 or 1.
    /// @param field The field.
    /// @return The memos, or nothing when damaged.
    [[nodiscard]] std::optional<std::vector<AskedJoin>> DecodeAskedJoins(std::span<std::byte const> field)
    {
        auto const entries = WireFields::SplitAll(field);
        if (!entries.has_value() || entries->size() > MaxAskedJoins)
            return std::nullopt;
        auto asked = std::vector<AskedJoin> {};
        asked.reserve(entries->size());
        for (auto const& entry: *entries)
        {
            auto const fields = WireFields::SplitExactly(entry, AskedJoinFields);
            if (!fields.has_value() || (*fields)[1].size() != Ed25519PublicKeyBytes)
                return std::nullopt;
            auto const at = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
            auto const admitted = (*fields)[3];
            if (!at.has_value() || admitted.size() != 1 || std::to_integer<unsigned>(admitted[0]) > 1)
                return std::nullopt;
            auto memo = AskedJoin { .clusterId = OwnedText((*fields)[0]),
                                    .provenKey = {},
                                    .askedAtUnixSeconds = *at,
                                    .admitted = admitted[0] == std::byte { 1 } };
            std::ranges::copy((*fields)[1], memo.provenKey.begin());
            asked.push_back(std::move(memo));
        }
        return asked;
    }

    /// What an operator does about a kept record this build cannot read, by the refusal's code.
    struct KeptRecordRemedy
    {
        ConsensusErrorCode code; ///< The refusal.
        std::string_view remedy; ///< What to do, as the refusal's closing clause.
    };

    /// One row per code `DecodeFormationRecord` refuses with. Another layout is intact and belongs
    /// to another build, so the remedy is that build; only damage is answered by moving the file
    /// aside, because that re-forms the node and leaves its fleet.
    constexpr std::array KeptRecordRemedies {
        KeptRecordRemedy { .code = ConsensusErrorCode::UnsupportedFormatVersion,
                           .remedy = "start the build that wrote that format; moving the file aside re-forms this "
                                     "node as a new solitary cluster, which leaves the fleet it recorded" },
        KeptRecordRemedy { .code = ConsensusErrorCode::MalformedFrame,
                           .remedy = "move the file aside to re-form this node as a new solitary cluster, which "
                                     "leaves the fleet it recorded" },
    };

    /// The remedy for a kept record refused with @p code.
    /// @param code The refusal's code.
    /// @return Its row's remedy; damage's for a code no row names, which the decoder never answers.
    [[nodiscard]] std::string_view KeptRecordRemedyFor(ConsensusErrorCode code) noexcept
    {
        for (auto const& row: KeptRecordRemedies)
            if (row.code == code)
                return row.remedy;
        return KeptRecordRemedies.back().remedy;
    }

    /// A kept record's refusal, keeping its code, naming the file and saying what to do.
    /// @param refusal Why the record does not decode.
    /// @param path The record's file.
    /// @param consequence What this refusal does to the file, or empty.
    /// @return The refusal to report.
    [[nodiscard]] ConsensusError KeptRecordRefusal(ConsensusError refusal,
                                                   std::filesystem::path const& path,
                                                   std::string_view consequence)
    {
        refusal.context =
            std::format("{}: {} -- {}{}", path.string(), refusal.context, consequence, KeptRecordRemedyFor(refusal.code));
        return refusal;
    }

    /// The record's file in @p directory.
    /// @param directory The cluster dir.
    /// @return The path.
    [[nodiscard]] std::filesystem::path RecordPath(std::filesystem::path const& directory)
    {
        return directory / std::string { FormationRecordFileName };
    }
} // namespace

std::vector<std::byte> EncodeFormationRecord(FormationRecord const& record)
{
    auto const created = WireFields::ToBigEndian<std::uint64_t>(record.own.createdAtUnixSeconds);
    auto const joining = EncodePart(record.joining, EncodeJoinTarget);
    auto const fleet = EncodePart(record.fleet, EncodeFleetMembership);
    auto const archive = EncodePart(record.archivePending, EncodeArchivePending);
    auto const rejected = EncodePart(record.rejectedBy, EncodeRejectionMemo);
    auto const asked = EncodeAskedJoins(record.askedJoins);
    auto const fields = WireFields::Encode({ WireFields::AsBytes(record.own.clusterId),
                                             std::span<std::byte const> { created },
                                             std::span<std::byte const> { joining },
                                             std::span<std::byte const> { fleet },
                                             std::span<std::byte const> { archive },
                                             std::span<std::byte const> { rejected },
                                             std::span<std::byte const> { asked } });

    std::vector<std::byte> out;
    out.reserve(HeaderBytes + fields.size());
    out.insert(out.end(), FormationRecordMagic.begin(), FormationRecordMagic.end());
    out.push_back(static_cast<std::byte>(FormationRecordFormat));
    out.push_back(static_cast<std::byte>(record.mode));
    out.insert(out.end(), fields.begin(), fields.end());
    return out;
}

std::expected<FormationRecord, ConsensusError> DecodeFormationRecord(std::span<std::byte const> bytes)
{
    if (bytes.size() <= FormationRecordFormatOffset
        || !std::ranges::equal(bytes.first(FormationRecordMagic.size()), FormationRecordMagic))
        return std::unexpected(MalformedWireFrame("the bytes are not a formation record"));

    // By name, before anything else about the bytes is judged: a layout this build did not write
    // is intact, and reading it as this build's would report damage that is not there.
    if (auto const format = static_cast<std::uint8_t>(bytes[FormationRecordFormatOffset]); format != FormationRecordFormat)
        return std::unexpected(UnsupportedFormatVersion(std::format(
            "the formation record was written in format {}; this build reads format {}", format, FormationRecordFormat)));

    if (bytes.size() < HeaderBytes)
        return std::unexpected(Damaged("it ends before its mode"));
    auto const mode = static_cast<NodeMode>(bytes[FormationRecordModeOffset]);
    if (!std::ranges::contains(KnownNodeModes, mode))
        return std::unexpected(Damaged(
            std::format("its mode byte {:#04x} names no mode", static_cast<unsigned>(bytes[FormationRecordModeOffset]))));

    auto const fields = WireFields::SplitExactly(bytes.subspan(HeaderBytes), RecordFields);
    if (!fields.has_value())
        return std::unexpected(Damaged(std::format("it is not its {} fields", RecordFields)));
    auto const created = WireFields::FromBigEndian<std::uint64_t>((*fields)[1]);
    if (!created.has_value())
        return std::unexpected(Damaged("its creation time is not eight bytes"));

    auto joining = DecodePart<JoinTarget>((*fields)[2], "join target", DecodeJoinTarget);
    if (!joining.has_value())
        return std::unexpected(std::move(joining).error());
    auto fleet = DecodePart<FleetMembership>((*fields)[3], "fleet membership", DecodeFleetMembership);
    if (!fleet.has_value())
        return std::unexpected(std::move(fleet).error());
    auto archive = DecodePart<std::string>((*fields)[4], "pending archive", DecodeArchivePending);
    if (!archive.has_value())
        return std::unexpected(std::move(archive).error());
    auto rejected = DecodePart<RejectionMemo>((*fields)[5], "rejection memo", DecodeRejectionMemo);
    if (!rejected.has_value())
        return std::unexpected(std::move(rejected).error());
    auto asked = DecodeAskedJoins((*fields)[6]);
    if (!asked.has_value())
        return std::unexpected(
            Damaged(std::format("its asked fleets cannot be read, or are more than the {} a build keeps", MaxAskedJoins)));

    auto record = FormationRecord { .mode = mode,
                                    .own = { .clusterId = OwnedText((*fields)[0]), .createdAtUnixSeconds = *created },
                                    .joining = *std::move(joining),
                                    .fleet = *std::move(fleet),
                                    .archivePending = *std::move(archive),
                                    .rejectedBy = *std::move(rejected),
                                    .askedJoins = *std::move(asked) };

    // Every cluster id the record names, within the bound every reader of an id holds it to: no
    // build writes a longer one, so one is damage, and it is refused by name rather than carried
    // into a beacon every peer drops. The join target's is its summary's, which its codec bounds.
    auto const named = std::array<std::pair<std::string_view, std::string const*>, 4> { {
        { "own cluster", &record.own.clusterId },
        { "fleet membership", record.fleet.has_value() ? &record.fleet->clusterId : nullptr },
        { "pending archive", record.archivePending.has_value() ? &*record.archivePending : nullptr },
        { "rejection memo", record.rejectedBy.has_value() ? &record.rejectedBy->clusterId : nullptr },
    } };
    auto const tooLong = [](std::string_view what, std::size_t bytes) {
        return Damaged(std::format("its {} names a cluster id of {} bytes, longer than the {} any build writes",
                                   what,
                                   bytes,
                                   CompileCacheWire::MaxIdBytes));
    };
    for (auto const& [what, id]: named)
        if (id != nullptr && id->size() > CompileCacheWire::MaxIdBytes)
            return std::unexpected(tooLong(what, id->size()));
    for (auto const& memo: record.askedJoins)
        if (memo.clusterId.size() > CompileCacheWire::MaxIdBytes)
            return std::unexpected(tooLong("asked fleet", memo.clusterId.size()));
    return record;
}

std::expected<std::string, SecureRandomError> MintClusterId(ISecureRandom& random)
{
    std::array<std::byte, ClusterIdBytes> drawn {};
    if (auto const filled = random.Fill(drawn); !filled.has_value())
        return std::unexpected(filled.error());
    std::string id;
    id.reserve(ClusterIdBytes * 2);
    for (auto const byte: drawn)
        std::format_to(std::back_inserter(id), "{:02x}", std::to_integer<unsigned>(byte));
    return id;
}

std::expected<FormationRecord, SecureRandomError> MintSolitary(ISecureRandom& random, core::platform::IWallClock const& wall)
{
    // A clock set before 1970 is a broken clock; see `UnbelievableClockCreatedAt`.
    auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(wall.now().time_since_epoch()).count();
    auto const created = seconds < 0 ? UnbelievableClockCreatedAt : static_cast<std::uint64_t>(seconds);
    return MintClusterId(random).transform([created](std::string id) {
        return FormationRecord { .mode = NodeMode::Solitary,
                                 .own = { .clusterId = std::move(id), .createdAtUnixSeconds = created },
                                 .joining = std::nullopt,
                                 .fleet = std::nullopt,
                                 .archivePending = std::nullopt,
                                 .rejectedBy = std::nullopt,
                                 .askedJoins = {} };
    });
}

void RememberAsked(FormationRecord& record, AskedJoin asked)
{
    auto const known = std::ranges::any_of(record.askedJoins, [&asked](AskedJoin const& memo) {
        return memo.clusterId == asked.clusterId && memo.provenKey == asked.provenKey;
    });
    if (known)
        return;
    record.askedJoins.push_back(std::move(asked));
    // One past the bound at most, since every write goes through here: the oldest memo nothing
    // admitted goes first, and an admitted one only when every memo is. The memo just added is
    // never its own victim -- it is not admitted yet, so it would always be "the oldest not admitted"
    // once every older memo is, and the ask would be forgotten the moment it was made.
    if (record.askedJoins.size() <= MaxAskedJoins)
        return;
    auto const older = std::prev(record.askedJoins.end());
    auto const unadmitted = std::ranges::find(record.askedJoins.begin(), older, false, &AskedJoin::admitted);
    record.askedJoins.erase(unadmitted != older ? unadmitted : record.askedJoins.begin());
}

void RememberAdmitted(FormationRecord& record, std::string_view clusterId, Ed25519PublicKey const& provenKey)
{
    for (auto& memo: record.askedJoins)
        if (memo.clusterId == clusterId && memo.provenKey == provenKey)
            memo.admitted = true;
}

bool FoundedHere(FormationRecord const& record) noexcept
{
    return !record.fleet.has_value();
}

std::string const& CurrentClusterId(FormationRecord const& record) noexcept
{
    return record.fleet.has_value() ? record.fleet->clusterId : record.own.clusterId;
}

FileFormationStore::FileFormationStore(std::filesystem::path directory):
    _directory { std::move(directory) }
{
}

std::expected<std::optional<FormationRecord>, ConsensusError> FileFormationStore::Load() const
{
    auto const path = RecordPath(_directory);
    auto const bytes = Consensus::ReadFileIfPresent(path);
    if (!bytes.has_value())
        return std::unexpected(bytes.error());
    if (!bytes->has_value())
        return std::optional<FormationRecord> {};

    auto record = DecodeFormationRecord(**bytes);
    if (!record.has_value())
        return std::unexpected(KeptRecordRefusal(std::move(record).error(), path, {}));
    return std::optional { *std::move(record) };
}

std::expected<void, ConsensusError> FileFormationStore::Save(FormationRecord const& record)
{
    auto const path = RecordPath(_directory);
    auto const kept = Consensus::ReadFileIfPresent(path);
    if (!kept.has_value())
        return std::unexpected(kept.error());
    if (kept->has_value())
    {
        if (auto decoded = DecodeFormationRecord(**kept); !decoded.has_value())
            return std::unexpected(KeptRecordRefusal(
                std::move(decoded).error(), path, "it is kept as it stands and no record is written over it; "));
    }

    // Its owner's alone when this creates it, as the state directory always is.
    if (auto const created = CreateOwnerOnlyDirectory(_directory); !created.has_value())
        return std::unexpected(
            StorageFailure(std::format("cannot create {}: {}", _directory.string(), created.error().message())));
    return Consensus::ReplaceFileAtomically(path, EncodeFormationRecord(record), StateFile::Formation);
}

} // namespace FastCache::Cluster
