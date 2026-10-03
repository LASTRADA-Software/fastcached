// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/StateFiles.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file FormationRecord.hpp
/// How a node formed, kept in its cluster dir: the mode it is in, the solitary cluster it minted,
/// the fleet it asked to join or joined, and the memos that outlive a restart.
///
/// A node IS its state directory, so this record is read at every start and written, whole and
/// indivisibly, BEFORE the node acts on any change to it. A record that cannot be read refuses the
/// start by name rather than being replaced by a fresh solitary one: a node that re-minted over it
/// would silently leave the fleet it belongs to.
namespace FastCache::Cluster
{

/// The solitary cluster a node minted for itself.
struct OwnCluster
{
    std::string clusterId;                    ///< Minted from `ISecureRandom`, as lowercase hex.
    std::uint64_t createdAtUnixSeconds { 0 }; ///< When it was minted, from the wall clock.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(OwnCluster const&, OwnCluster const&) = default;
};

/// The fleet a pending node asked to join.
struct JoinTarget
{
    CompileCacheWire::FleetSummary summary; ///< What the fleet said about itself, as it was proven.
    Ed25519PublicKey provenKey {};          ///< The key that signed that summary.
    std::uint64_t askedAtUnixSeconds { 0 }; ///< When this node first asked.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(JoinTarget const&, JoinTarget const&) = default;
};

/// The fleet a node was admitted to.
struct FleetMembership
{
    std::string clusterId;         ///< The fleet's id.
    std::vector<std::byte> roster; ///< `EncodeRoster` bytes, as the approval carried them.

    /// When the fleet's founder created it, as the fleet's summary said when this node was
    /// admitted; zero when that was never said.
    ///
    /// Kept because a member ANNOUNCES it: a learner's or promoted voter's summary carries the
    /// fleet's age, and an encounter's tie-break reads it. Zero would read as the oldest fleet there
    /// is and win every tie-break it enters, which is why a node that knows the age says it.
    std::uint64_t createdAtUnixSeconds { 0 };

    /// The key that SIGNED the admission this node acted on: one it PROVED at the endpoint it asked,
    /// through the chain from the key that proved the fleet's summary.
    ///
    /// What `--fleet-id` judges a joined node's record by (`Cluster::AdmitsFleet`), at every start and
    /// every move -- never the voters the roster above lists. A roster is public keys, and an approval
    /// signed by some other key could list a pinned voter's as easily as its own: judged by what the
    /// roster CLAIMS, the record would be no check of its own on the answer it came from.
    Ed25519PublicKey admittedBy {};

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(FleetMembership const&, FleetMembership const&) = default;
};

/// The last fleet that refused this node.
struct RejectionMemo
{
    std::string clusterId;             ///< The fleet that refused.
    std::uint64_t atUnixSeconds { 0 }; ///< When it refused.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(RejectionMemo const&, RejectionMemo const&) = default;
};

/// A fleet this node once asked to admit it, remembered after the ask ended.
///
/// The evidence a split of one fleet is TOLD on (`Cluster::SplitEvidence::WeAskedAndTheyListUs`): a
/// machine that asked a fleet under the key it was proven by can tell that fleet, and only that fleet,
/// apart from anybody claiming its id -- so the memo outlives an abandoned, rejected or approved ask,
/// and is dropped only past `MaxAskedJoins`. It never heals a split by itself: the key it names is one
/// this node trusted on first use, so an operator decides (`Cluster::SplitHealing::ByOperator`).
struct AskedJoin
{
    std::string clusterId;                  ///< The fleet it asked.
    Ed25519PublicKey provenKey {};          ///< The key that proved that fleet's summary when it asked.
    std::uint64_t askedAtUnixSeconds { 0 }; ///< When it first asked.

    /// Whether that fleet ADMITTED this node under that key: an admission signed over this node's own
    /// request by the key it proved there. The memos an ask that went nowhere must never displace, since
    /// a fleet lists a machine because it admitted it -- but admitted is not trusted: every memo, admitted
    /// or not, is evidence an operator decides on, and none heals by itself.
    bool admitted { false };

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(AskedJoin const&, AskedJoin const&) = default;
};

/// How many asked fleets a record remembers; the oldest is dropped to make room.
///
/// A handful, because a machine asks a fleet rarely -- at install, and again only after a rejection
/// window or an abandonment -- and a bound, because the record is read at every start.
inline constexpr std::size_t MaxAskedJoins = 8;

// Every memo a record keeps travels in one announcement, so the leader holds all of them.
static_assert(CompileCacheWire::MaxAnnouncedJoinMemos == MaxAskedJoins,
              "a NODE-ANNOUNCE carries exactly the join memos a formation record keeps");

/// Where the fleets this node once asked are read from, as they stand now.
///
/// What a node hands its leader in every announcement, so the leader holds every member's evidence
/// that a fleet it sees is this one split -- whichever machine did the asking.
class IAskedJoinsSource
{
  public:
    IAskedJoinsSource() = default;
    IAskedJoinsSource(IAskedJoinsSource const&) = delete;
    IAskedJoinsSource(IAskedJoinsSource&&) = delete;
    IAskedJoinsSource& operator=(IAskedJoinsSource const&) = delete;
    IAskedJoinsSource& operator=(IAskedJoinsSource&&) = delete;
    virtual ~IAskedJoinsSource() = default;

    /// @return The memos, oldest first; at most `MaxAskedJoins`.
    [[nodiscard]] virtual std::vector<AskedJoin> AskedJoins() const = 0;
};

/// Everything a node keeps about how it formed.
struct FormationRecord
{
    NodeMode mode { NodeMode::Solitary };      ///< What the node is.
    OwnCluster own;                            ///< The solitary cluster this node minted.
    std::optional<JoinTarget> joining;         ///< Present exactly while Pending.
    std::optional<FleetMembership> fleet;      ///< The fleet it joined: Learner, and Voter unless it founded.
    std::optional<std::string> archivePending; ///< A cluster whose Raft store still sits in the root.
    std::optional<RejectionMemo> rejectedBy;   ///< The last fleet that refused it.
    std::vector<AskedJoin> askedJoins {};      ///< The fleets it asked, oldest first; at most `MaxAskedJoins`.

    /// Field-wise equality, so a round trip is asserted whole.
    [[nodiscard]] friend bool operator==(FormationRecord const&, FormationRecord const&) = default;
};

/// The layout `EncodeFormationRecord` writes. A record in any other layout is refused as
/// `UnsupportedFormatVersion` and left where it is; a record a LATER build wrote is never read
/// as this build's, and never written over.
///
/// 3 because a join target nests the fleet's summary as the summary's own codec writes it, and that
/// grew from seven fields to ten: a format 2 record is intact and holds a seven-field summary, which
/// read as this build's would be reported as damage that is not there.
///
/// 4 because a fleet membership EMBEDS its approval's roster as the roster's own codec writes it
/// (`FleetMembership::roster`), and that moved to `Cluster::RosterFormatVersion` 2 when a member
/// gained its recorded `0xFC` endpoint: a format 3 record is intact and holds a version-1 roster,
/// which this build refuses here, by the record's own number, before anything decodes the roster.
///
/// 5 because a fleet membership gained a fourth field, the PROVEN key that signed the admission
/// (`FleetMembership::admittedBy`), which the fleet pin judges a joined node's record by: a format 4
/// record is intact and holds a membership of three fields, and nothing in it says which key signed
/// the approval, so it is refused by number rather than judged by the roster's claims.
///
/// **Format 5 is this branch's own, UNRELEASED definition, and is FINAL only at the lane-0 flag
/// day.** Format 3 changed twice without moving -- an asked-fleet memo went from three fields to
/// four, and the nested summary from ten fields to eleven -- because no build that wrote those
/// shapes ever shipped; the embedded roster moved it to 4, and the admitting key to 5, because a
/// learner's record is on disk on the one installation there is. From the flag day on, every change
/// to this layout -- nested codecs included -- moves this number.
inline constexpr std::uint8_t FormationRecordFormat = 5;

/// The four bytes every formation record starts with, so a file that is not one is told apart
/// from one in another layout.
inline constexpr std::array<std::byte, 4> FormationRecordMagic {
    std::byte { 'F' }, std::byte { 'C' }, std::byte { 'F' }, std::byte { 'R' }
};

/// Where the format byte sits: straight after the magic, ahead of anything a layout may change.
inline constexpr std::size_t FormationRecordFormatOffset = FormationRecordMagic.size();

/// Where the mode byte sits: straight after the format byte.
inline constexpr std::size_t FormationRecordModeOffset = FormationRecordFormatOffset + 1;

/// The file a cluster dir keeps its formation record in.
inline constexpr std::string_view FormationRecordFileName = StateFileName(StateFile::Formation);

/// How many random bytes a cluster id is drawn from.
inline constexpr std::size_t ClusterIdBytes = 16;

// A minted id is its bytes in hex, and every reader of a cluster id -- the summary's codec, a
// discovery challenge, this record -- refuses one past `MaxIdBytes`.
static_assert(2 * ClusterIdBytes <= CompileCacheWire::MaxIdBytes,
              "a minted cluster id must be one every reader of an id accepts");

/// The creation time `MintSolitary` records when the wall clock reads before the Unix epoch.
///
/// Such a clock is broken, and the least harmful reading of it is the NEWEST possible cluster:
/// between two solitary clusters the older one is yielded to, so reading the epoch instead would
/// hand every tie-break to the one machine whose clock cannot be believed. A consumer recognises
/// it by this name and never converts it to a signed duration, where it would wrap negative and
/// become the oldest cluster after all.
inline constexpr std::uint64_t UnbelievableClockCreatedAt = std::numeric_limits<std::uint64_t>::max();

/// Encode a record for its file.
///
/// The magic, the format byte and the mode byte, then the fields: the own cluster's id and
/// creation time, then each optional part as a nested blob whose first field is a one-byte
/// presence tag. The tag rather than an empty blob, because an empty string is a legal value and
/// absent is not empty.
/// @param record The record.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> EncodeFormationRecord(FormationRecord const& record);

/// Read a record back.
///
/// The magic first, then the format byte BEFORE anything else about the bytes is judged -- a
/// layout this build did not write is intact, and reading it as this build's would report damage
/// that is not there -- then the mode against `KnownNodeModes`, then the fields.
/// @param bytes What `EncodeFormationRecord` wrote.
/// @return The record; `UnsupportedFormatVersion` for another layout; `MalformedFrame` for bytes
///         that are not a formation record, or are a damaged one -- a cluster id longer than
///         `CompileCacheWire::MaxIdBytes` included, named, since no build writes one.
[[nodiscard]] std::expected<FormationRecord, ConsensusError> DecodeFormationRecord(std::span<std::byte const> bytes);

/// Mint a cluster id: `ClusterIdBytes` drawn from @p random, as lowercase hex.
///
/// Lowercase hex of the bytes in draw order, so comparing two ids as text compares their bytes.
/// @param random The operating system's generator.
/// @return The id, or why the draw failed -- in which case NO id is minted, since a weak one
///         would let two machines imaged from one disk mint the same cluster.
[[nodiscard]] std::expected<std::string, SecureRandomError> MintClusterId(ISecureRandom& random);

/// Whether @p clusterId is spelled the way `MintClusterId` spells every id it mints: `ClusterIdBytes`
/// as lowercase hex, nothing shorter, longer or upper-case.
///
/// The grammar an operator's `--fleet-id` is held to, so a pin is refused by name where it is typed
/// rather than matching no fleet ever: a truncated copy, a pasted space, an id in capitals. One
/// predicate beside the mint, never a copy of it at the flag.
/// @param clusterId The text.
/// @return True when a mint could have produced it.
[[nodiscard]] bool IsMintedClusterId(std::string_view clusterId) noexcept;

/// Mint the record of a node that has formed nothing yet: solitary, in a cluster of its own.
/// @param random The generator the cluster id is drawn from.
/// @param wall The clock the creation time is read from, as whole seconds since the Unix epoch;
///             `UnbelievableClockCreatedAt` when it reads before the epoch.
/// @return The record, or why the id could not be drawn.
[[nodiscard]] std::expected<FormationRecord, SecureRandomError> MintSolitary(ISecureRandom& random,
                                                                             core::platform::IWallClock const& wall);

/// Remember that this node asked @p asked's fleet.
///
/// One memo per `(clusterId, provenKey)`: asking the same fleet under the same key again changes
/// nothing, so a memo keeps the time it was FIRST asked; the same id under ANOTHER key is a different
/// claim and a memo of its own -- the evidence a split is told on must not let a later key stand in for
/// the one the node actually asked.
///
/// **Past `MaxAskedJoins` the oldest memo NOT admitted is dropped**, and an admitted one only when
/// every memo is. Asking costs a fleet nothing it cannot mint -- a fresh id, or the same id under a
/// fresh key, proves itself to any challenge -- so a node led to ask eight of them would otherwise
/// lose the one memo it holds for a fleet that admitted it, and with it the split's heal. An
/// admission is signed by the key the node proved (`Cluster::VerifyAdmission`), so displacing an
/// admitted memo takes eight fleets that each really admitted it. The direction when that happens
/// anyway: the heal it would have given becomes `ForeignFleet`, told to an operator -- CLOSED.
/// @param record The record to change.
/// @param asked The fleet asked.
void RememberAsked(FormationRecord& record, AskedJoin asked);

/// Mark the memo of asking @p clusterId under @p provenKey as ADMITTED: the fleet it names took this
/// node in, under that key. Nothing changes when the record holds no such memo.
/// @param record The record to change.
/// @param clusterId The fleet that admitted this node.
/// @param provenKey The key it was proven by when this node asked it.
void RememberAdmitted(FormationRecord& record, std::string_view clusterId, Ed25519PublicKey const& provenKey);

/// Whether the node is in the cluster it minted itself rather than one it joined.
/// @param record The record.
/// @return True when no fleet membership is recorded.
[[nodiscard]] bool FoundedHere(FormationRecord const& record) noexcept;

/// The id of the cluster the node is in now.
/// @param record The record.
/// @return The joined fleet's id when there is one, else the node's own.
[[nodiscard]] std::string const& CurrentClusterId(FormationRecord const& record) noexcept;

/// The cluster @p record commits this node to that it did not mint for itself: the fleet a pending
/// node asked, or the one whose consensus a learner or a voter runs (`ConsensusScope::Fleet`).
///
/// What `--fleet-id` is judged against (`Cluster::AdmitsFleet`). A solitary node is committed to
/// nothing -- its own cluster is the one every node starts in, and a pin to another names where it
/// is going, not where it is.
/// @param record The record.
/// @return The id, or nothing for a node in a cluster of its own that asked nobody.
[[nodiscard]] std::optional<std::string_view> CommittedClusterId(FormationRecord const& record) noexcept;

/// The seam every formation change is written through, BEFORE the node acts on it.
class IFormationStore
{
  public:
    IFormationStore() = default;
    IFormationStore(IFormationStore const&) = delete;
    IFormationStore(IFormationStore&&) = delete;
    IFormationStore& operator=(IFormationStore const&) = delete;
    IFormationStore& operator=(IFormationStore&&) = delete;
    virtual ~IFormationStore() = default;

    /// Read the record kept.
    /// @return The record, DISENGAGED when none was ever written, or why it cannot be read.
    [[nodiscard]] virtual std::expected<std::optional<FormationRecord>, ConsensusError> Load() const = 0;

    /// Replace the record kept with @p record.
    ///
    /// Write then rename: a crash leaves the previous record whole. A record kept that does not
    /// decode is refused rather than replaced, whoever asks: one in another layout belongs to a
    /// build that is not this one, and a damaged one is still the only account of the fleet this
    /// node recorded -- writing over either is the node silently leaving that fleet.
    /// @param record The record to keep.
    /// @return Nothing; the kept record's own refusal (`UnsupportedFormatVersion` or
    ///         `MalformedFrame`) when it does not decode; or why it could not be kept.
    [[nodiscard]] virtual std::expected<void, ConsensusError> Save(FormationRecord const& record) = 0;
};

/// The formation record kept in `<directory>/formation`.
class FileFormationStore final: public IFormationStore
{
  public:
    /// @param directory The cluster dir; created on the first save when it does not exist.
    explicit FileFormationStore(std::filesystem::path directory);

    /// @copydoc IFormationStore::Load
    [[nodiscard]] std::expected<std::optional<FormationRecord>, ConsensusError> Load() const override;

    /// @copydoc IFormationStore::Save
    [[nodiscard]] std::expected<void, ConsensusError> Save(FormationRecord const& record) override;

  private:
    std::filesystem::path _directory;
};

} // namespace FastCache::Cluster
