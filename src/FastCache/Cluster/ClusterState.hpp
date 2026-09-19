// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cluster
{

/// Whether a member has ever had a scheduler endpoint recorded.
///
/// **Persisted and transmitted**: one byte per member, in a snapshot and in every
/// `ClusterStatus` reply. The ordinals are explicit and append only, and `Last` never
/// travels.
///
/// It exists because an empty `schedulerEndpoint` is two states (#1340). A member
/// that has never led has not said, which is ordinary. A member whose endpoint a
/// re-admit cleared HAD said, and `AddMember` wiped it wholesale -- right for a move,
/// and reachable without one, because enrollment recovery re-approves through the same
/// verb. The causes differ and so do the remedies, and the endpoint alone cannot tell
/// them apart.
///
/// **A history rather than a three-state status**, because *announced* is already what
/// a non-empty endpoint says: storing it a second time would be a second source of
/// truth, while this records only what the endpoint cannot. `Apply` is the one writer,
/// and `DecodeState` refuses the one combination `Apply` never produces -- an endpoint
/// with no announcement behind it.
enum class SchedulerEndpointHistory : std::uint8_t
{
    NeverAnnounced = 0, ///< No endpoint recorded since this id was admitted from absence.
    Announced = 1,      ///< An endpoint was recorded at least once; empty now means cleared.
    Last = 2,           ///< Not a history, and never travels. See `DecodeWireEnum`.
};

/// Which of the consensus configuration's two sets a member is recorded in (#1449).
///
/// **Persisted and transmitted**: one byte per member, in a snapshot and in every
/// `ClusterStatus` reply. The ordinals are explicit and append only, and `Last` never
/// travels.
///
/// The RECORD of what the operator decided, never a reading of what consensus
/// currently counts: the leader moves the configuration towards it one change at a
/// time (`NextQuorumChange`), so between an admit and its commit the two may differ,
/// and a report that needs the one in force asks consensus rather than this.
enum class MemberSeat : std::uint8_t
{
    Voter = 0,   ///< Counted by every quorum: commitment, elections and CheckQuorum.
    Learner = 1, ///< Replicated to and counted by nothing, and never stands for election.
    Last = 2,    ///< Not a seat, and never travels. See `DecodeWireEnum`.
};

/// One member of the cluster, as the replicated state records it.
///
/// **The endpoint is the point.** `Consensus::RaftMembership` carries ids and
/// nothing else, which is correct for consensus — an id is all the algorithm needs
/// to count a quorum — and leaves a node the cluster has agreed to admit
/// unreachable until something else supplies its address. That residual was
/// recorded when membership landed; this is where it is closed. A member is a
/// `(id, endpoint)` pair here, so agreeing to admit somebody and knowing where they
/// answer are one decision replicated together rather than two facts that can
/// disagree.
struct ClusterMember
{
    Consensus::NodeId id; ///< Stable identity; what consensus counts.

    /// host:port this member's consensus port answers on.
    ///
    /// Always present -- a member with no address is the thing this struct exists to
    /// make impossible -- and always dialable, because it is what every other member
    /// opens a socket to.
    std::string raftEndpoint;

    /// host:port clients reach the fleet on while this member LEADS; may be empty.
    ///
    /// **A second endpoint rather than one**, and the reason is a defect this pairing
    /// closes rather than a generality. `NotLeader` carries a redirect, and the one
    /// address recorded here used to be the consensus port -- so a follower answered
    /// "ask the leader, at its Raft peer port", and a client that took the advice
    /// spoke the scheduler protocol at a socket that has never heard of it. Two
    /// ports, two facts, and collapsing them made every redirect in a real cluster
    /// point somewhere nothing could be done with.
    ///
    /// Empty is legitimate and means "this member has not said". Only a *leader's*
    /// matters, and a leader announces its own record on election -- so the value is
    /// absent exactly for the members whose value nobody needs, and a bootstrap peer
    /// that has never led carries none rather than carrying a guess.
    ///
    /// Empty has a second cause, which `schedulerEndpointHistory` tells apart: a
    /// re-admit clears it. A reader reporting WHY it is empty asks
    /// `SchedulerEndpointStateOf`, never this field and a second guess.
    std::string schedulerEndpoint;

    /// Whether `schedulerEndpoint` has ever held a value since this id was admitted.
    SchedulerEndpointHistory schedulerEndpointHistory { SchedulerEndpointHistory::NeverAnnounced };

    /// Which set the operator admitted this member into (#1449).
    ///
    /// Written by the verb that admitted it -- `AddMember` records a voter and
    /// `AddLearner` a learner -- so re-admitting through the other verb is how a member
    /// is promoted or demoted, and nothing else writes it.
    MemberSeat seat { MemberSeat::Voter };

    /// This member's identity key, or absent when nothing has stated one (#178).
    ///
    /// **Absent is not a key of zeroes, and it is not "revoked".** A member admitted before
    /// keys existed, or admitted by a verb that named none, simply has not said; what asserts
    /// one is the member itself, announcing its own record, or an operator's `@<key>`. A
    /// re-admit that names no key KEEPS what is recorded -- unlike the scheduler endpoint, a
    /// machine that moves keeps its identity -- and only a forget takes it away, revoking it
    /// with the record (`CommandKind::Forget`).
    std::optional<Ed25519PublicKey> publicKey;

    [[nodiscard]] friend bool operator==(ClusterMember const&, ClusterMember const&) = default;
};

/// What a principal is admitted to do (#178).
///
/// **Persisted and transmitted**: one byte per principal in a snapshot and in every
/// `ClusterStatus` reply, and one byte in an `AdmitPrincipal` command. The ordinals are
/// explicit and append only, and `Last` never travels.
enum class PrincipalRole : std::uint8_t
{
    Worker = 0, ///< Registers with the scheduler and runs compiles; never joins consensus.
    Last = 1,   ///< Not a role, and never travels. See `DecodeWireEnum`.
};

/// How one `PrincipalRole` is spelled.
struct PrincipalRoleRow
{
    PrincipalRole role;    ///< The role this row describes.
    std::string_view name; ///< Its one spelling: a table cell, a JSON value and a report word alike.
};

/// One row per `PrincipalRole`, in enumerator order.
inline constexpr EnumTable<PrincipalRole, PrincipalRoleRow> PrincipalRoleTable { {
    { .role = PrincipalRole::Worker, .name = "worker" },
} };

static_assert(RowsInEnumeratorOrder(PrincipalRoleTable, &PrincipalRoleRow::role),
              "PrincipalRoleTable must hold one row per PrincipalRole, in enumerator order");

/// The spelling of `role`, from `PrincipalRoleTable`.
/// @param role A role.
/// @return Its row's name.
[[nodiscard]] constexpr std::string_view PrincipalRoleName(PrincipalRole role) noexcept
{
    return PrincipalRoleTable[static_cast<std::size_t>(role)].name;
}

/// A machine the cluster admits by its KEY without counting it (#178).
///
/// **Not a member**, and the difference is the whole reason this is a second list rather
/// than a third `MemberSeat`. A member is somewhere consensus replicates to -- it has a Raft
/// endpoint every other member dials -- while a principal is a machine that never joins
/// consensus at all: a roaming worker whose address the VPN reassigns. What the cluster
/// records about it is the one thing that does not move, its key, and what it may do.
///
/// An id is a member or a principal, never both; `Apply` holds that, and `DecodeState`
/// refuses a state that breaks it.
struct ClusterPrincipal
{
    Consensus::NodeId id;                         ///< Its identity, as it names itself.
    Ed25519PublicKey publicKey {};                ///< The key it proves that identity with.
    PrincipalRole role { PrincipalRole::Worker }; ///< What it is admitted to do.

    [[nodiscard]] friend bool operator==(ClusterPrincipal const&, ClusterPrincipal const&) = default;
};

/// A key the cluster will never admit again, and whose it was (#178).
///
/// **The whole key, never a digest or a prefix**: a revoked machine still holds every byte
/// it ever held, and whatever it presents next is checked against this -- so the record has
/// to be something a signature can be verified under, which a digest is not. That is what
/// lets a later verifier check the proof BEFORE it reports `revoked`, so the refusal is
/// never an oracle for a caller who holds no key at all.
struct RevokedKey
{
    /// Whose it was: the id the forget removed, taken from that record (`CommandKind::Forget`).
    ///
    /// The WIRE keys nothing on it, because the key is what is refused there, whatever id a
    /// proof then claims. The QUORUM does: a member consensus still counts, recorded nowhere
    /// and named here, was forgotten rather than never recorded, and whoever leads removes it
    /// (`NextQuorumChange`) -- a member the configuration counts keeps its own key on the
    /// consensus wire (`RosterKeys`), so one nobody removed would go on voting (#1555).
    Consensus::NodeId id;
    Ed25519PublicKey publicKey {}; ///< The key.

    [[nodiscard]] friend bool operator==(RevokedKey const&, RevokedKey const&) = default;
};

/// What a member's scheduler endpoint is, as a reader asks it.
///
/// Derived from `ClusterMember` and never stored or sent, so its ordinals carry no
/// contract. It is the one question every renderer of a member asks, so the fleet page,
/// `--cluster-status` and `fastcache-cli cluster-members` cannot answer it three ways.
enum class SchedulerEndpointState : std::uint8_t
{
    Announced,      ///< Recorded: where clients reach the fleet while this member leads.
    NeverAnnounced, ///< Never recorded. A member that has not led; the ordinary case.
    Cleared,        ///< Recorded once and wiped by a re-admit; it returns when the member next leads.
    Last,           ///< Not a state, and has no row: the length of a table keyed by one.
};

/// How one `SchedulerEndpointState` is spelled.
struct SchedulerEndpointStateRow
{
    SchedulerEndpointState state; ///< The state this row describes.
    std::string_view name;        ///< Its one spelling: a table cell, a JSON value and a report word alike.
};

/// One row per `SchedulerEndpointState`, in enumerator order.
inline constexpr EnumTable<SchedulerEndpointState, SchedulerEndpointStateRow> SchedulerEndpointStateTable { {
    { .state = SchedulerEndpointState::Announced, .name = "announced" },
    { .state = SchedulerEndpointState::NeverAnnounced, .name = "never-announced" },
    { .state = SchedulerEndpointState::Cleared, .name = "cleared" },
} };

static_assert(RowsInEnumeratorOrder(SchedulerEndpointStateTable, &SchedulerEndpointStateRow::state),
              "SchedulerEndpointStateTable must hold one row per SchedulerEndpointState, in enumerator order");

/// Which of the three states `member`'s scheduler endpoint is in.
/// @param member The member.
/// @return Announced when an endpoint is recorded; otherwise what its history says.
[[nodiscard]] SchedulerEndpointState SchedulerEndpointStateOf(ClusterMember const& member) noexcept;

/// The spelling of `member`'s scheduler endpoint state, from `SchedulerEndpointStateTable`.
/// @param member The member.
/// @return The row's name.
[[nodiscard]] std::string_view SchedulerEndpointStateName(ClusterMember const& member) noexcept;

/// Parse one `<id>=<host>:<port>[@<key>]` member specification.
///
/// The grammar an operator types, in the one place the type it produces lives. It
/// has two callers that must not disagree — `--raft-peer` names a member at
/// startup and `--cluster-admit` names one at runtime, and the documentation tells
/// an operator to copy the same token between them — so a second implementation
/// would be two flags accepting different token sets for one concept, with only one
/// of them being what the transport actually dials.
///
/// Split at the **first** `=`, so an endpoint may contain one and an id may not.
/// The other way round makes `n1=host=1:6675` parse as an id of `n1=host`, which is
/// an id no operator wrote and which would silently never match a vote.
///
/// The endpoint must be one a peer can dial, which takes both halves of what a
/// dialer asks: a bare port names no machine, and `host:0` names no port, and a
/// member recorded either way is one the cluster counts towards quorum and cannot
/// reach.
///
/// **`@<key>` states the member's identity key** (#178), in the one spelling
/// `FormatEd25519PublicKey` prints. Split at the FIRST `@` after the `=`: no host
/// contains one and no key does, so a token with two is refused rather than read as a
/// host nobody can resolve. A key that does not parse is refused with the sentence
/// that says what a key looks like, never read as part of the endpoint.
/// @param spec The token as an operator wrote it.
/// @return The member, or why the token is not one -- a sentence naming the token.
[[nodiscard]] std::expected<ClusterMember, std::string> ParseMemberSpec(std::string_view spec);

/// Render @p member the way `ParseMemberSpec` reads it: `<id>=<host>:<port>`, and `@<key>`
/// when a key is recorded.
///
/// The inverse, beside the parser, for the reason the key's own two functions are a pair:
/// a service registration re-renders every `--raft-peer` from its parsed form, and a
/// rendering that dropped the key would install a node whose next start no longer knows
/// what its own operator typed.
/// @param member The member.
/// @return The token.
[[nodiscard]] std::string FormatMemberSpec(ClusterMember const& member);

/// A setting every member of the cluster must agree on.
///
/// A **table** rather than an open string map, so an unknown key is refused at the
/// leader rather than replicated to every node and ignored differently by each. The
/// failure that prevents is the quiet one: a setting somebody typo'd would be
/// accepted, stored, snapshotted and carried across restarts while doing nothing,
/// and the only symptom would be that the thing they configured did not happen.
///
/// Adding a setting is adding a row.
struct SettingSpec
{
    std::string_view name;    ///< The key as an operator writes it.
    std::string_view summary; ///< What it does, for `--help` and diagnostics.

    /// Refuse a value this setting may not take, or null when any text will do.
    ///
    /// A COLUMN rather than an arm of `Validate`'s switch, because the switch is
    /// per-verb and this is per-setting: a second constrained setting would otherwise
    /// grow a second `if` inside `SetSetting`'s arm, and adding a setting stops being
    /// adding a row. Null is the honest spelling for `fleet-open`, whose value this
    /// build does not constrain, rather than a function that accepts everything --
    /// one says *nothing to check*, the other says *checked and fine*, and only the
    /// first is true.
    ///
    /// Runs on the LEADER before the append, so an operator gets an error rather than
    /// an entry replicated to every node. It REFUSES and never repairs: a clamped
    /// value would be answered `accepted` while the cluster adopted a different
    /// number, which is a setting that did not do what it said -- the failure this
    /// whole table exists to prevent.
    ///
    /// @param value The value as the operator typed it.
    /// @return Why it may not be set, or nullopt when it may.
    std::optional<std::string> (*refuse)(std::string_view value) = nullptr;

    /// Where the value is READ BACK, or empty when nothing reads it.
    ///
    /// `Class::Function`, never a line number: a citation carrying a line rots at the
    /// next edit above it and sends a reader somewhere arbitrary.
    ///
    /// A CLAIM a reader can check, and deliberately not a mechanism (#1124). The scan
    /// that would mechanise it is the weakest option available, and the ticket says
    /// why: a row's name is a string literal, one of them is a named constant, and
    /// `upstream`'s name collided with a per-node FLAG spelled the same way -- so the
    /// natural grep reported the deadest row in the table as wired. A check with a
    /// false negative built into it is worse than a claim somebody has to write down.
    std::string_view readBy {};

    /// Why NOTHING reads this row yet, naming the issue that will -- or empty.
    ///
    /// The opt-out, and a separate COLUMN rather than an empty `readBy`, for
    /// `RefuseWithoutCounter`'s reason in the metrics rules: *nobody has wired this*
    /// must not be spelled the way *forgot the column* is, or a placeholder says
    /// `forgot` in the vocabulary of `decided`. Exactly one of the two is set --
    /// neither and both are the same refusal, since both mean nobody has decided.
    std::string_view unreadBecause {};
};

/// Whether every row of @p table says what reads it.
///
/// **The failure this closes is the one `SettingTable`'s own header describes, for the
/// key an operator spells CORRECTLY** (#1124). `FindSetting` refuses a typo; nothing
/// refused a row that was accepted, replicated, snapshotted and carried across
/// restarts while being read by nobody -- and two of the three rows were in that state
/// at once, which is a pattern rather than luck. `fleet-open` was closed by wiring it
/// (#1112) and `upstream` by removing it (#1123); this is the guard each of those
/// instances left standing.
///
/// MANDATORY with an explicit opt-out rather than opt-in, because opt-in is silent
/// about the row that never opted in -- #492's argument inside a table. It takes the
/// table as a PARAMETER rather than reading `SettingTable` directly, and that is what
/// lets a test drive it in the REFUSING direction: a guard nobody has watched refuse
/// is not a guard, and one nobody has watched accept is not known to work (#1031).
///
/// `consteval`, so it cannot be called at runtime and mistaken for a test -- a runtime
/// check of it could not fail in a translation unit that compiled.
///
/// @param table The rows to check.
/// @return True when every row names a reader, or names the issue that will add one.
[[nodiscard]] consteval bool RowsCarryAConsumer(std::span<SettingSpec const> table) noexcept
{
    return std::ranges::all_of(table, [](SettingSpec const& row) {
        // Neither is the omission this exists to catch; BOTH is a row saying that a
        // reader exists and also that none does. One condition, because they are one
        // fact: nobody has decided.
        if (row.readBy.empty() == row.unreadBecause.empty())
            return false;
        // An opt-out names the issue that will close it, exactly as `RefuseUntriaged`
        // does, or *nobody has decided yet* reads as *decided against*.
        return !row.readBy.empty() || row.unreadBecause.contains('#');
    });
}

/// The key naming how long a lease -- and therefore a dispatched compile -- may live.
///
/// One named constant rather than a literal at each site, for the reason the wire
/// error codes are one: this string is spelled by the table row that declares it, by
/// the scheduler that reads it back, and by the documentation, and three surfaces
/// spelling it separately is exactly how they drift apart.
inline constexpr std::string_view LeaseLifetimeSetting = "lease-lifetime";

/// The key naming whether this fleet admits every caller or only its members.
///
/// A named constant for `LeaseLifetimeSetting`'s reason, and #1112 is what it costs
/// when there is not one: the row was spelled here and read NOWHERE, so
/// `--cluster-set fleet-open=1` was accepted, replicated, snapshotted and carried
/// across restarts while changing no admission decision. `FindSetting` closes that
/// for a misspelled key; nothing closed it for a correctly spelled one.
///
/// **Searching for this row is itself a trap**: `--fleet-open` names a per-node FLAG
/// of the same words, so a grep for the name finds the flag and reads as a reader.
/// The reliable question is who passes this constant to `SettingOf`.
inline constexpr std::string_view FleetOpenSetting = "fleet-open";

/// Read a `lease-lifetime` value, or say why it is not one.
///
/// **The one predicate both the validator and every reader ask**, rather than one
/// constant they each compare against. Two sites that spell the comparison separately
/// can agree about the bounds and still disagree about what parses -- which is the
/// argument the spend-once retention window is built on, one layer along: it is the
/// comparison that is easy to get wrong twice, not the number.
///
/// Declared here and defined in the translation unit, so this header does not have to
/// include the wire constants it bounds against; `SettingTable` only needs an address.
///
/// @param value A duration (`20min`), as the operator typed it.
/// @return The lifetime, or a sentence naming why it may not be set.
[[nodiscard]] std::expected<std::chrono::milliseconds, std::string> ParseLeaseLifetime(std::string_view value);

/// Refuse a `lease-lifetime` this cluster may not agree on.
///
/// The `SettingSpec::refuse` adapter over `ParseLeaseLifetime`. Separate only because
/// the column answers *may this be set* and a reader wants the value.
///
/// @param value A duration (`20min`), as the operator typed it.
/// @return Why it may not be set, or nullopt when it may.
[[nodiscard]] std::optional<std::string> RefuseLeaseLifetime(std::string_view value);

/// Every replicated setting, in one place.
///
/// Deliberately short. A setting belongs here when **every node must agree** on it
/// — otherwise it is local configuration and belongs on the command line, where it
/// can differ per machine because the machines differ. `--slots` is the counter-
/// example worth naming: it describes one host and replicating it would impose one
/// machine's size on all of them.
inline constexpr std::array<SettingSpec, 2> SettingTable {
    SettingSpec { .name = FleetOpenSetting,
                  .summary = R"('1' to admit every caller to the fleet, '0' for members only)",
                  .readBy = "NodeMembership::AgreedOpenness" },
    SettingSpec { .name = LeaseLifetimeSetting,
                  .summary = "how long a compile lease lives END TO END, as a duration (20min) -- upload, wait "
                             "for a slot, compile, and the object coming back -- not how long a compiler may run",
                  .refuse = &RefuseLeaseLifetime,
                  .readBy = "SchedulerService::AgreedLeaseLifetime" },
};

static_assert(RowsCarryAConsumer(SettingTable),
              "every SettingTable row must name what READS it, or name the issue that will wire it: a row "
              "nothing reads is accepted, replicated, snapshotted and carried across restarts while the thing "
              "the operator configured does not happen, and a correctly spelled key reaches no other guard");

/// Whether `name` is a setting this cluster replicates.
/// @param name The key.
/// @return Its row, or nullptr when this build does not know it.
[[nodiscard]] constexpr SettingSpec const* FindSetting(std::string_view name) noexcept
{
    for (auto const& row: SettingTable)
        if (row.name == name)
            return &row;
    return nullptr;
}

/// A key this cluster refuses to replicate, and what an operator should do instead.
///
/// A refusal by ROW rather than by ABSENCE, which is the `--allow-compile-arg`
/// argument in `.agent/rules/distributed-compilation.md` one surface along: absence
/// and a row are the same answer only while nothing else is consulted, and here the
/// OPERATOR is. A key this build never had and a key it deliberately stopped
/// replicating both come out of `FindSetting` as a null pointer, so both would be
/// answered *no such cluster setting* -- which reads as a typo or as a node too old,
/// and sends somebody to upgrade a machine over a decision. The row is what lets the
/// answer say WHY and name the flag that does the job.
///
/// No named constant for the key, deliberately, and `LeaseLifetimeSetting`'s reason is
/// why: a constant exists because several surfaces spell one string and drift apart.
/// A refused key has no reader by construction -- that is what refused means -- so it
/// is spelled once here, and the test spells its own literal, which is what lets the
/// test catch this row naming the wrong key.
struct RefusedSettingSpec
{
    std::string_view name;   ///< The key as an operator writes it.
    std::string_view reason; ///< Why this cluster will not agree on it, and what to do instead.
};

/// Every key this cluster refuses to replicate, in one place.
///
/// **A replicated setting must not decide where a node sends a credential.** That is
/// the property, stated ahead of the row it was drawn from, because the next candidate
/// will not be an address: anything a majority can commit which every node then
/// presents a secret to has this shape.
///
/// `upstream` was a `SettingTable` row until #1123, and the LOSING reading is worth
/// recording because it is what put the row here. An address looks inert beside
/// #1112's `fleet-open`, which decides ADMISSION: replicating one reads as telling
/// every member where the shared cache moved to. What that misses is that a node does
/// not merely dial it. `CacheTier.cpp:227`/`:228` construct the `RemoteUpstream` from
/// `cfg.upstream` AND this node's `ICredentialSource`, and `RemoteUpstream.cpp:135`
/// and `:175` present `_credential.Current()` on every `CacheFetch` and every
/// `CacheStore`. The address and the secret are then governed by different mechanisms
/// -- `--requirepass` is per machine and reloadable one node at a time, a setting is
/// committed by a majority -- so wiring the row would have let one committed entry
/// redirect every member's `--requirepass` to an address of the committer's choosing,
/// each node presenting it on its next fetch.
///
/// Nothing read the row, so removing it takes no behaviour with it: the per-node
/// `--upstream` flag has always been what decides this, and the refusal names it.
inline constexpr std::array<RefusedSettingSpec, 1> RefusedSettingTable { {
    RefusedSettingSpec { .name = "upstream",
                         .reason = "it decides where a node presents its --requirepass credential, so it is "
                                   "per-machine configuration -- set --upstream on the node that reads through" },
} };

/// Whether `name` is a key this cluster refuses to replicate.
/// @param name The key.
/// @return Its row, or nullptr when this build has no opinion about the name.
[[nodiscard]] constexpr RefusedSettingSpec const* FindRefusedSetting(std::string_view name) noexcept
{
    for (auto const& row: RefusedSettingTable)
        if (row.name == name)
            return &row;
    return nullptr;
}

static_assert(std::ranges::none_of(RefusedSettingTable,
                                   [](RefusedSettingSpec const& row) { return FindSetting(row.name) != nullptr; }),
              "a refused key must not also be a SettingTable row: a refusal must not be escapable by a row "
              "arriving later and shadowing it, and this is what makes the order the two are asked in irrelevant");

/// One replicated setting and its value.
struct Setting
{
    std::string name;  ///< A key from `SettingTable`.
    std::string value; ///< Whatever the operator set it to.

    [[nodiscard]] friend bool operator==(Setting const&, Setting const&) = default;
};

/// Everything the cluster agrees on.
///
/// Deliberately small, and deliberately **not** the cache. The log that carries this
/// is replicated to every member and kept until it is snapshotted, so what goes in it
/// has to be state that changes rarely and matters everywhere. Cache entries are the
/// opposite of both — multi-megabyte objects written constantly — which is why they
/// live in a `fastcached` this state merely names.
struct ClusterState
{
    /// Members, sorted by id so two nodes that applied the same entries hold
    /// byte-identical state. A snapshot is compared across machines only by
    /// accident today, but a state whose serialization depended on insertion order
    /// would make that comparison impossible the day somebody wants it.
    std::vector<ClusterMember> members;

    /// Settings, sorted by name for the same reason.
    std::vector<Setting> settings;

    /// Hosts the cluster admits as CLIENTS, sorted and unique (#1309).
    ///
    /// A client -- a developer's laptop, a CI runner, anything running `fastcache-cc`
    /// against the fleet -- never joins consensus, so it had no replicated route at all:
    /// it was admitted only by each node's `--fleet-member` list, and removing one meant
    /// a reload on every machine. Host only, because that is all admission compares: a
    /// caller dials from an ephemeral port.
    std::vector<std::string> clients;

    /// Hosts the cluster has FORGOTTEN, sorted and unique (#1309): a tombstone per host.
    ///
    /// **Recorded, because absence cannot say it.** A host missing from `members` and
    /// `clients` is the ordinary state of every machine a node's own list names, so a
    /// forget that only erased would leave nothing a node could narrow its local list
    /// by -- and a local list is exactly where a decommissioned machine lingers. Written
    /// by `ForgetClient` and by `Forget` (the removed member's consensus host),
    /// cleared by `AdmitClient` and `AddMember` for that host.
    ///
    /// **Bounded by the distinct hosts ever forgotten and not re-admitted**, never by
    /// traffic: nothing but a committed command adds one, and a re-admit removes one.
    std::vector<std::string> forgotten;

    /// Machines admitted by key rather than as members, sorted by id (#178).
    ///
    /// Written by `AdmitPrincipal`, which enrollment proposes for a worker (#178 PR 4), and
    /// removed by `Forget`, which revokes the key with it (#1555).
    std::vector<ClusterPrincipal> principals;

    /// Keys the cluster will never admit again, sorted by id and then key, one entry per
    /// key (#178).
    ///
    /// **Bounded by the keys ever revoked**, never by traffic, for `forgotten`'s reason:
    /// nothing but a committed `Forget` of an id holding a key adds one. Nothing ever
    /// shortens it -- a revocation that could be undone would be a key that could come
    /// back, and the property is that it cannot.
    std::vector<RevokedKey> revokedKeys;

    /// How many times the ROSTER has changed: the members' ids, endpoints, seats and keys,
    /// the principals and the revoked keys (#178).
    ///
    /// **Derived by `Apply`, never carried by a command**, and bumped only when the roster's
    /// projection actually differs afterwards -- so every voter applying the same log reaches
    /// the same number for the same roster, which is what lets their endorsements of it add
    /// up to a majority. The applied log INDEX is the rejected alternative: two voters a
    /// moment apart would endorse one roster under two versions, and neither would ever
    /// reach a majority.
    ///
    /// A worker adopts only a version at least as new as the one it holds, so this is also
    /// what stops a replayed old roster -- endorsed when it was current -- from winding a
    /// worker back.
    std::uint64_t rosterVersion {};

    [[nodiscard]] friend bool operator==(ClusterState const&, ClusterState const&) = default;

    /// The consensus endpoint recorded for `id`, if any.
    /// @param id The member.
    /// @return Its Raft endpoint, or nullopt when it is not a member.
    [[nodiscard]] std::optional<std::string> RaftEndpointOf(std::string_view id) const;

    /// Where clients reach the fleet while `id` leads, if it has said.
    ///
    /// Absent for a member that is not known, for one that has never announced
    /// itself **and** for one a re-admit cleared, which are deliberately the same
    /// answer here: all three mean there is nowhere to send a client, and a caller
    /// routing one would have nothing different to do about any of them. Telling the
    /// last two apart is a question for a person reading a report, and
    /// `SchedulerEndpointStateOf` answers it.
    /// @param id The member.
    /// @return Its scheduler endpoint, or nullopt.
    [[nodiscard]] std::optional<std::string> SchedulerEndpointOf(std::string_view id) const;

    /// The value of `name`, if it has been set.
    /// @param name The setting.
    /// @return Its value, or nullopt when nobody set it.
    [[nodiscard]] std::optional<std::string> SettingOf(std::string_view name) const;

    /// Every member's consensus endpoint, in id order.
    ///
    /// The Raft one rather than the scheduler one, because this feeds
    /// `Distributed::ClusterMembership`, which matches on the HOST part and admits a
    /// peer whatever port it dialed from. Both endpoints name the same host, and only
    /// this one is guaranteed to be there at all.
    /// @return The endpoints, which is what `Distributed::ClusterMembership` takes.
    [[nodiscard]] std::vector<std::string> Endpoints() const;

    /// Whether the cluster admits `host` as a client.
    ///
    /// Through `SameHost`, the one comparison admission makes, so a client recorded as
    /// `10.0.0.1` is the one a dual-stack listener reports as `::ffff:10.0.0.1`.
    /// @param host A caller's host, without a port.
    /// @return True when a `clients` entry names the same machine.
    [[nodiscard]] bool AdmitsClient(std::string_view host) const;

    /// Whether the cluster has forgotten `host` and not admitted it again.
    /// @param host A caller's host, without a port.
    /// @return True when a `forgotten` entry names the same machine.
    [[nodiscard]] bool HasForgotten(std::string_view host) const;

    /// Whether `key` has been revoked.
    /// @param key A public key.
    /// @return True when a `revokedKeys` entry holds it.
    [[nodiscard]] bool IsRevoked(Ed25519PublicKey const& key) const;

    /// Who holds `key` LIVE, as a member or as a principal.
    /// @param key A public key.
    /// @return The holder's id, or nullopt when no member and no principal holds it.
    [[nodiscard]] std::optional<std::string> HolderOf(Ed25519PublicKey const& key) const;
};

/// What a command does to the state.
///
/// An `enum class` rather than three payload types, so the wire carries one byte a
/// receiver switches on and an unknown verb is refused rather than mistaken for a
/// known one.
///
/// **Transmitted and persisted, and append only.** The numeric values are a wire contract
/// twice over -- the byte a peer decodes and a log entry keeps, and the index of a table
/// keyed by this enum -- so reordering these silently remaps every verb a running fleet
/// has already replicated. `Last` is the count and never travels.
///
/// **Only the first enumerator states its value, and the enforcement is the BYTE PINS in
/// `ClusterState_test.cpp` rather than the declaration.** Writing `= N` on every verb is
/// the tempting reading of the rule and was this branch's first version: it leaves `Last`
/// inconsistent with the rest, which `readability-enum-initial-value` fails the build
/// over, and the only ways out are worse. A literal `Last = N` is a hand-maintained count
/// beside a hand-maintained list, which is the shape that drifts; anchoring the length on
/// a verb by name is a guard that fires only when nothing is wrong. A test asserting each
/// byte outranks both, because a red test cannot be failed to notice the way an absent
/// `= N` can.
///
/// **A verb is added without moving `CommandVersion`** when the layout does not change,
/// and that has a consequence a fleet mid-upgrade lives with: a member running a build
/// that predates a verb meets its committed entries and SKIPS them by name
/// (`ClusterStateMachine::Apply`), applying the rest of the log around them. So it holds
/// the state as if that command had never been proposed -- which for #1309's two verbs
/// means such a member admits no replicated client (closed, and healed by the upgrade)
/// and ignores a client forget (OPEN for that host, until it is upgraded). #178's two
/// verbs DID move it, because they brought two fields the layout had no room for.
enum class CommandKind : std::uint8_t
{
    /// Add a member, or update the endpoint of one already present.
    ///
    /// One verb for both, because they are one intention: a node that moved has the
    /// same identity and a new address, and making the operator remove it first
    /// would leave a window in which the cluster has agreed it does not exist.
    AddMember = 0,

    /// Forget an id: remove it wherever the cluster records it, as a member or as a
    /// principal, and REVOKE the key that record held (#1555). `--cluster-forget`.
    ///
    /// **One act, because an operator removing a machine has one intention.** The two
    /// halves apart are a state nobody asked for: a record gone and its key live is a
    /// machine every node whose `--raft-peer` still types that key goes on accepting --
    /// removal failing OPEN -- and a key revoked under a record that stays is a member the
    /// configuration goes on counting, so on the consensus wire the revocation never takes
    /// effect. So there is no verb for either half alone.
    ///
    /// What it takes is DERIVED from the record being removed, as a member's host tombstone
    /// is (#1309), so it cannot disagree with what the state holds when it commits -- a key
    /// replaced between the proposal and the commit is the one revoked. A member leaves a
    /// tombstone for its host too; a principal has no host. Beside that, the command may
    /// carry the key the proposing LEADER holds live for the id (`PrepareForget`): the one
    /// thing the state cannot derive, because a member a `--raft-peer` line typed with its
    /// key is recorded without one, or not at all, and its key lives only on the command
    /// lines that type it. Never another id's key -- refused at the proposal, skipped at
    /// commit.
    ///
    /// The ordinal is `RemoveMember`'s, and the verb is that one widened rather than a new
    /// one: every entry a RELEASED build wrote names a member with no key, which this
    /// applies exactly as `RemoveMember` did, and no released build can be a member of a
    /// cluster that has keys -- the Raft peer wire refuses its version
    /// (`RaftWire::MinSupportedVersion`).
    Forget,
    SetSetting,

    /// Admit a client host to the fleet, clearing any tombstone for it (#1309).
    ///
    /// The replicated counterpart of a `--fleet-member` entry, for a machine that never
    /// joins consensus. Also the route BACK for a member that was forgotten and now
    /// serves as a plain worker or client: its forget tombstoned its host.
    AdmitClient,

    /// Forget a client host: stop admitting it and record that it was forgotten (#1309).
    ///
    /// A POSITIVE act, and recorded as one, because a node's own `--fleet-member` list
    /// may still name the host -- a tombstone is what lets every node refuse it with one
    /// committed entry rather than a reload on each machine.
    ForgetClient,

    /// `AddMember`, recording the member as a LEARNER (#1449).
    ///
    /// A verb rather than a field on `AddMember`, for the reason `AdmitClient` is one:
    /// the layout stays as it is and `CommandVersion` does not move. Everything else is
    /// `AddMember`'s -- the same fields, the same wholesale record, the same move -- so
    /// admitting a voter through this verb DEMOTES it, and admitting a learner through
    /// `AddMember` promotes it. `MemberSeatTable` says which verb writes which seat.
    AddLearner,

    /// Admit a machine by its key, as a principal rather than a member (#178).
    ///
    /// Refused for a key that is revoked, for a key another id holds, and for an id that
    /// is a member. Re-admitting a principal's id replaces its record, which is how a
    /// principal's key is rotated -- the old key is then simply nobody's, refused on every
    /// wire as a key nobody holds. A key an operator wants refused FOR GOOD is revoked by
    /// forgetting the id before it is admitted again under its new one (`Forget`).
    AdmitPrincipal,

    Last, ///< Not a verb, and has no row: the length of a table keyed by one.
};

/// How one `MemberSeat` is spelled, and which verb records a member in it.
struct MemberSeatRow
{
    MemberSeat seat;        ///< The seat this row describes.
    std::string_view name;  ///< Its one spelling: a table cell, a JSON value and a report word alike.
    CommandKind admittedBy; ///< The verb that records a member in this seat.

    /// The set of a consensus configuration a member in this seat is placed in.
    std::vector<Consensus::NodeId> Consensus::Configuration::* set;

    /// Whether a quorum counts a member in this seat.
    ///
    /// What decides whether moving a member INTO the seat must wait until every node
    /// can dial it: a member counted before its votes can arrive is a quorum that has
    /// grown and cannot be satisfied.
    bool counted;
};

/// One row per `MemberSeat`, in enumerator order.
///
/// The one statement of which verb writes which seat and which configuration set it
/// means, so `Apply`, the scheduler that builds the command, `NextQuorumChange` that
/// moves consensus towards it and every renderer read the same answer.
inline constexpr EnumTable<MemberSeat, MemberSeatRow> MemberSeatTable { {
    { .seat = MemberSeat::Voter,
      .name = "voter",
      .admittedBy = CommandKind::AddMember,
      .set = &Consensus::Configuration::voters,
      .counted = true },
    { .seat = MemberSeat::Learner,
      .name = "learner",
      .admittedBy = CommandKind::AddLearner,
      .set = &Consensus::Configuration::learners,
      .counted = false },
} };

static_assert(RowsInEnumeratorOrder(MemberSeatTable, &MemberSeatRow::seat),
              "MemberSeatTable must hold one row per MemberSeat, in enumerator order");

/// The seat `kind` admits a member into, if it admits one at all.
/// @param kind A verb.
/// @return The seat whose row names @p kind, or nullopt for a verb that admits no member.
[[nodiscard]] constexpr std::optional<MemberSeat> SeatAdmittedBy(CommandKind kind) noexcept
{
    for (auto const& row: MemberSeatTable)
        if (row.admittedBy == kind)
            return row.seat;
    return std::nullopt;
}

/// The seat `id` is recorded in, or a voter when `state` has no record of it.
///
/// The reading of *no opinion about the seat* for an operator's re-admission -- an
/// enrollment approval, which recovery repeats -- so it cannot promote a demoted member by
/// re-proposing it as a voter. A voter for a member with no record, because an approval is
/// an operator's act. The membership RECONCILER does not use it (#1535): what it records
/// is an observation, and a member nothing has placed joins there as a learner
/// (`Cluster::NewcomerSeat`). Declared here and defined in the translation unit, beside
/// `ClusterState`'s lookups.
/// @param state The replicated state.
/// @param id The member.
/// @return Its recorded seat, or `MemberSeat::Voter`.
[[nodiscard]] MemberSeat RecordedSeatOf(ClusterState const& state, std::string_view id);

/// The spelling of `seat`, from `MemberSeatTable`.
/// @param seat A seat.
/// @return Its row's name.
[[nodiscard]] constexpr std::string_view MemberSeatName(MemberSeat seat) noexcept
{
    return MemberSeatTable[static_cast<std::size_t>(seat)].name;
}

/// One change to the cluster's state, as it travels in a log entry.
struct Command
{
    CommandKind kind { CommandKind::AddMember };
    /// The member id for `AddMember`/`AddLearner`, the setting name for `SetSetting`, the
    /// client's host (a port, if given, is ignored) for `AdmitClient`/`ForgetClient`, the
    /// principal's id for `AdmitPrincipal`, and the id -- a member's or a principal's -- for
    /// `Forget`.
    std::string key;
    /// The consensus endpoint for `AddMember`/`AddLearner`, the value for `SetSetting`,
    /// empty otherwise.
    std::string value;

    /// `AddMember`/`AddLearner` only: where clients reach the fleet while this member leads.
    ///
    /// Applied **wholesale**, so an empty one clears whatever was recorded rather
    /// than leaving it. That is the right way round: a member is re-admitted when its
    /// record has changed, and a node that moved has moved both ports -- keeping the
    /// old scheduler endpoint would redirect clients to an address that member no
    /// longer answers, which is worse than redirecting them nowhere. The member keeps
    /// the fact that it had one (`SchedulerEndpointHistory`), which `Apply` derives
    /// rather than this command carrying it. Refused for the other two verbs, because a
    /// field a verb ignores is a field somebody misunderstood.
    std::string schedulerEndpoint;

    /// The key the verb acts on (#178): the member's for `AddMember`/`AddLearner`, where
    /// absent is NO OPINION and keeps what is recorded; the principal's for
    /// `AdmitPrincipal`, where it is required; and for `Forget`, the key the proposing leader
    /// holds live for the id, revoked beside whatever the record holds (`PrepareForget`).
    /// Refused for every other verb.
    std::optional<Ed25519PublicKey> publicKey;

    /// `AdmitPrincipal` only, and required there: what the principal may do. Refused for
    /// every other verb.
    std::optional<PrincipalRole> role;

    [[nodiscard]] friend bool operator==(Command const&, Command const&) = default;
};

/// Serialize a command for a log entry's payload.
/// @param command The change.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> Encode(Command const& command);

/// Read a command back.
///
/// Refused **by name**, in the three ways a peer-wire frame is: another build's encoding is
/// `UnsupportedVersion` with both versions stated, a verb this build does not know is
/// `UnknownMessageType`, and only bytes that are not a command are `MalformedFrame`. A
/// committed entry that will not decode is skipped, so the reason is what the log line
/// says -- *upgrade that node* and *these bytes are damaged* are different remedies.
/// @param payload The entry's payload.
/// @return The command; or why it is not one.
[[nodiscard]] std::expected<Command, ConsensusError> DecodeCommand(std::span<std::byte const> payload);

/// Serialize a whole state, for a snapshot.
/// @param state The state.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> Encode(ClusterState const& state);

/// Read a whole state back.
///
/// A state another build encoded is refused **by name** -- `UnsupportedVersion`, with
/// both versions in the context -- and never as `MalformedFrame`. The version is how a
/// mismatch is detected, and a reader told only *malformed* is sent looking for damage
/// in bytes that are intact.
/// @param bytes A snapshot or a `ClusterStatus` body previously produced by `Encode`.
/// @return The state; `UnsupportedVersion` for another build's encoding, `MalformedFrame`
///         for bytes that are not a state.
[[nodiscard]] std::expected<ClusterState, ConsensusError> DecodeState(std::span<std::byte const> bytes);

/// Apply one command, in place.
///
/// Total: every well-formed command has an effect or is a no-op, and none can fail.
/// That is a property consensus needs rather than a convenience — an entry is
/// applied *after* it is committed, so there is nobody left to report a failure to
/// and no way to un-commit it. Anything that could be refused has to be refused
/// **before** it is proposed, which is what `Validate` is for.
/// @param state The state to change.
/// @param command The change.
void Apply(ClusterState& state, Command const& command);

/// Whether `command` may be proposed at all.
///
/// The only place a change can be refused, for the reason `Apply` cannot be. Called
/// by the leader before it appends, so an operator gets an error rather than a
/// silently ignored entry replicated to the whole cluster -- and called by
/// `SchedulerService::Offer` as well, so the surface an operator types at refuses
/// whatever sits behind its cluster seam rather than only a proposer that happens to
/// ask this question itself.
///
/// **Everything it records has to be text**, and which strings those are is a
/// property of the VERB. `AddMember`'s three become a `ClusterMember`, which
/// `/fleet.json` emits -- RFC 8259 requires that document to be UTF-8 -- which the
/// fleet page embeds in an SVG, which is XML, and which `--cluster-status` and the
/// logs print. `SetSetting` constrains its value; its name is already settled by
/// `FindSetting`. And `Forget` constrains **nothing**, which is the rule that
/// matters: its key *is* the id being forgotten, so a member that reached replicated
/// state through a peer built before any of this existed has to stay forgettable.
/// One rule for every verb alike would refuse the one id an operator most needs to
/// type, and that member would count towards quorum forever -- which is the trap
/// #159 records.
///
/// `AdmitClient` and `ForgetClient` constrain their host, which both record: it must
/// name a machine, and it must not be this one's loopback -- a caller on the node's own
/// machine is always admitted to it, so a record about loopback would be accepted,
/// replicated and snapshotted while deciding nothing.
///
/// **A function of the command alone**, so it cannot know what the state holds: a key
/// that is revoked, or held by somebody else, is `ValidateAgainst`'s question.
/// @param command The change.
/// @return Nothing when it may be proposed, or why it may not.
[[nodiscard]] std::expected<void, ConsensusError> Validate(Command const& command);

/// Whether `command` may be proposed against the state as it stands (#178).
///
/// `Validate`, and then the rules only the state can answer, which are all about KEYS: a
/// revoked key is never admitted again (`KeyRevoked`, a refusal of the command -- nothing
/// the state can later do un-revokes it); a key is held by one id at a time; and an id is a
/// member or a principal, never both. Every proposer asks this -- the leader before it
/// appends, and the scheduler surface an operator types at -- so the refusal reaches whoever
/// asked, with its reason.
///
/// **It is a courtesy, and `Apply` is the guarantee.** Two proposals judged against one state
/// can both be appended before either commits -- a forget and an admission of the same key --
/// so `Apply` enforces the same rules on commit and drops a command that has stopped
/// satisfying them. What that costs is a silent drop; what this buys is that the ordinary case
/// is refused where somebody reads the answer.
/// @param state The state the command would apply to.
/// @param command The change.
/// @return Nothing when it may be proposed, or why it may not.
[[nodiscard]] std::expected<void, ConsensusError> ValidateAgainst(ClusterState const& state, Command const& command);

} // namespace FastCache::Cluster
