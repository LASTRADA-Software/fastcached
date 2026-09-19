// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

std::expected<ClusterMember, std::string> ParseMemberSpec(std::string_view spec)
{
    auto const notASpec = [spec] {
        return std::unexpected { std::format("not <id>=<host>:<port>[@<key>]: {}", spec) };
    };

    auto const split = spec.find('=');
    if (split == std::string_view::npos)
        return notASpec();

    auto const id = spec.substr(0, split);
    auto endpoint = spec.substr(split + 1);

    // The key, when there is one, split off at the FIRST `@`: no host contains one and no
    // key does, so a second `@` is a token nobody wrote correctly, and reading it as part
    // of a host would record an address nothing can resolve.
    auto publicKey = std::optional<Ed25519PublicKey> {};
    if (auto const at = endpoint.find('@'); at != std::string_view::npos)
    {
        auto const keyText = endpoint.substr(at + 1);
        endpoint = endpoint.substr(0, at);
        auto parsed = ParseEd25519PublicKey(keyText);
        if (!parsed.has_value())
            return std::unexpected { std::format(
                "{} names a key that is not one ({}): {}", spec, keyText, DescribePublicKeyTextFault(parsed.error())) };
        publicKey = *parsed;
    }

    if (id.empty() || endpoint.empty())
        return notASpec();

    // Both halves of the question a dialer asks. A split alone is not enough:
    // `10.0.0.4:0` splits and names no port anybody can connect to, so a member
    // accepted on that basis is one the cluster counts and never reaches.
    auto const parts = SplitHostPort(endpoint);
    if (!parts.has_value() || !ParseTcpPort(parts->second).has_value())
        return notASpec();

    // Every field named, including the one this token cannot carry. A member's
    // scheduler endpoint is a port peers never connect to, so nothing an operator
    // types about a PEER could supply it -- the node announces its own. Saying so
    // with `{}` rather than leaving it out is what keeps a field added to the
    // middle of the struct from becoming a silent zero here.
    //
    // A voter, because that is what `--raft-peer` bootstraps and what `--cluster-admit`
    // records; the learner spelling is a different flag rather than a different token,
    // so the one grammar an operator copies between them stays one grammar.
    return ClusterMember { .id = std::string { id },
                           .raftEndpoint = std::string { endpoint },
                           .schedulerEndpoint = {},
                           .schedulerEndpointHistory = SchedulerEndpointHistory::NeverAnnounced,
                           .seat = MemberSeat::Voter,
                           .publicKey = publicKey };
}

std::string FormatMemberSpec(ClusterMember const& member)
{
    if (!member.publicKey.has_value())
        return std::format("{}={}", member.id, member.raftEndpoint);
    return std::format("{}={}@{}", member.id, member.raftEndpoint, FormatEd25519PublicKey(*member.publicKey));
}

namespace
{
    /// Wire tag in front of every encoded command: a log entry's payload.
    ///
    /// Versioned for the reason every other format here is: a node must refuse bytes
    /// a build that arranged the fields differently wrote, rather than read them as
    /// this build's arrangement.
    ///
    /// **Not the state's version, and the two must not become one constant again.**
    /// They were one until #1340, which was safe only while every change moved both
    /// layouts. It moves when a command's LAYOUT does, and when a verb an entry already
    /// written may carry stops meaning what it meant. A fact `Apply` derives from commands
    /// it already reads -- `SchedulerEndpointHistory` -- is state, and moves `StateVersion`
    /// alone. So does a new VERB (#1309's `AdmitClient`/`ForgetClient`): the layout is
    /// unchanged, and a build that lacks the verb refuses its byte by name as
    /// `UnknownMessageType` rather than as another encoding.
    ///
    /// What a move costs is a node whose OWN log holds an older entry: it refuses to start,
    /// by name, rather than replay entries it would read differently (#1542). This used to
    /// say such a node SKIPPED every entry, which was true before #1542 and was the reason
    /// the rule above stopped at the layout. A committed entry from a PEER on another
    /// command version is still skipped (`ClusterStateMachine::Apply`), which is what a
    /// mixed fleet costs and why its consensus members upgrade together.
    ///
    /// 3 added the key and the role every command carries (#178), for `AdmitPrincipal` and a
    /// member's key on `AddMember` -- two fields the layout had no room for.
    ///
    /// 4 left the layout alone and changed what the verbs MEAN (#1555): `RevokeKey` was
    /// deleted, and `RemoveMember`'s ordinal became `Forget`, which also revokes the key the
    /// removed record held. A v3 entry naming a keyed member decodes cleanly here and would
    /// be replayed as a forget that revokes a key the build that wrote it never revoked. A
    /// NEW verb's byte is refused by name; a CHANGED verb's is not, so only this can detect it.
    constexpr std::uint8_t CommandVersion = 4;

    /// Fields in an encoded command: the header, then key, value, scheduler endpoint,
    /// public key and role.
    constexpr std::size_t CommandFields = 6;

    /// Wire tag in front of every encoded state: a snapshot, and a `ClusterStatus` body.
    ///
    /// A snapshot is the one thing here that outlives a process, so a format that could
    /// not say which build wrote it would make an upgrade a silent corruption. 3 added
    /// each member's `SchedulerEndpointHistory` (#1340). 4 added the admitted clients and
    /// the forgotten hosts, and states every group's count up front (#1309). 5 added each
    /// member's `MemberSeat` (#1449). 6 added each member's public key, the principals and
    /// the revoked keys (#178). 7 added the roster version every voter endorses (#178).
    constexpr std::uint8_t StateVersion = 7;

    /// Fields one member occupies in an encoded state: id, Raft, scheduler, the
    /// scheduler endpoint's history, the seat, and the public key.
    ///
    /// Named because the decoder's arithmetic is otherwise six unexplained sixes,
    /// and getting one of them wrong reads every member's scheduler endpoint as the
    /// next member's id -- which decodes, and produces a state nothing would report as
    /// wrong.
    constexpr std::size_t MemberFields = 6;

    /// Fields one setting occupies: name, value.
    constexpr std::size_t SettingFields = 2;

    /// Fields one principal occupies: id, public key, role.
    constexpr std::size_t PrincipalFields = 3;

    /// Fields one revoked key occupies: whose it was, and the key.
    constexpr std::size_t RevokedKeyFields = 2;

    /// Fields in front of the groups: the version, then the member, setting, client,
    /// forgotten-host, principal and revoked-key counts, then the roster version.
    constexpr std::size_t StateHeaderFields = 8;

    /// Where the roster version sits in the header.
    constexpr std::size_t RosterVersionField = 7;

    /// A key field's bytes: empty when no key is stated, the 32 bytes when one is.
    /// @param key The key, or nothing.
    /// @return A view of the key's bytes, or an empty span.
    [[nodiscard]] std::span<std::byte const> OptionalKeyBytes(std::optional<Ed25519PublicKey> const& key) noexcept
    {
        return key.has_value() ? std::span<std::byte const> { *key } : std::span<std::byte const> {};
    }

    /// Read a key field: empty is no key, exactly 32 bytes is one, anything else is refused.
    ///
    /// The one reader of a key's width, so a member's optional key, a principal's required
    /// one and a command's cannot come to disagree about what a short field means.
    /// @param field The field.
    /// @return Absent, a key, or nullopt-of-the-outer when the width is wrong.
    [[nodiscard]] std::optional<std::optional<Ed25519PublicKey>> ReadKeyField(std::span<std::byte const> field)
    {
        if (field.empty())
            return std::optional<Ed25519PublicKey> {};
        if (field.size() != Ed25519PublicKeyBytes)
            return std::nullopt;
        auto key = Ed25519PublicKey {};
        std::ranges::copy(field, key.begin());
        return std::optional { key };
    }

    /// What a state says about one key, asked on behalf of one id.
    ///
    /// **Private, and the one author of the rule** `ValidateAgainst` refuses by and `Apply`
    /// drops by, so the courtesy and the guarantee cannot come to disagree about what a
    /// revoked or a borrowed key is.
    enum class KeyStanding : std::uint8_t
    {
        Available,     ///< Nobody else holds it, and it was never revoked.
        Revoked,       ///< In `revokedKeys`: never admitted again, whoever asks.
        HeldElsewhere, ///< Held live by another id: one key, one identity.
    };

    /// Where @p key stands for a command admitting it under @p id.
    /// @param state The state.
    /// @param id Who the command would give the key to.
    /// @param key The key.
    /// @return Its standing.
    [[nodiscard]] KeyStanding StandingOf(ClusterState const& state, std::string_view id, Ed25519PublicKey const& key)
    {
        // Revoked first: a key that is both revoked and held would be a state `Apply` never
        // makes, and the permanent answer is the one worth giving if one ever arrived.
        if (state.IsRevoked(key))
            return KeyStanding::Revoked;
        if (auto const holder = state.HolderOf(key); holder.has_value() && *holder != id)
            return KeyStanding::HeldElsewhere;
        return KeyStanding::Available;
    }

    /// Whether @p id is recorded as a principal.
    /// @param state The state.
    /// @param id The id.
    /// @return True when a principal carries it.
    [[nodiscard]] bool IsPrincipal(ClusterState const& state, std::string_view id)
    {
        return std::ranges::contains(state.principals, id, &ClusterPrincipal::id);
    }

    /// Whether @p id is recorded as a member.
    /// @param state The state.
    /// @param id The id.
    /// @return True when a member carries it.
    [[nodiscard]] bool IsMember(ClusterState const& state, std::string_view id)
    {
        return std::ranges::contains(state.members, id, &ClusterMember::id);
    }

    /// The first rule of the roster @p state breaks, or nothing.
    ///
    /// The combinations `Apply` never produces, asked of a decoded state for the reason
    /// `DecodeState` refuses an endpoint with no announcement behind it: read as it stands,
    /// a state holding a revoked key live, one key under two ids, or one id in both lists
    /// would make every reader pick which half to believe.
    /// @param state A decoded state.
    /// @return Why it cannot be one `Apply` produced, or nullopt.
    [[nodiscard]] std::optional<std::string_view> BrokenRosterRule(ClusterState const& state)
    {
        auto live = std::vector<Ed25519PublicKey> {};
        for (auto const& member: state.members)
            if (member.publicKey.has_value())
                live.push_back(*member.publicKey);
        for (auto const& principal: state.principals)
        {
            if (IsMember(state, principal.id))
                return "an id is recorded both as a member and as a principal";
            live.push_back(principal.publicKey);
        }

        if (std::ranges::any_of(live, [&state](Ed25519PublicKey const& key) { return state.IsRevoked(key); }))
            return "a revoked key is still held";
        std::ranges::sort(live);
        if (std::ranges::adjacent_find(live) != live.end())
            return "one key is held by two ids";
        return std::nullopt;
    }

    /// Record `host` in a sorted host list, unless an entry already names the same machine.
    ///
    /// Unique by `SameHost`, the comparison admission makes, so `::ffff:10.0.0.1` and
    /// `10.0.0.1` are one entry rather than two a forget would have to find separately.
    /// @param hosts The list, sorted.
    /// @param host The host to record.
    void InsertHost(std::vector<std::string>& hosts, std::string_view host)
    {
        if (std::ranges::any_of(hosts, [host](std::string const& entry) { return SameHost(entry, host); }))
            return;
        hosts.emplace_back(host);
        std::ranges::sort(hosts);
    }

    /// Remove every entry naming the same machine as `host`.
    /// @param hosts The list.
    /// @param host The host to remove.
    void EraseHost(std::vector<std::string>& hosts, std::string_view host)
    {
        std::erase_if(hosts, [host](std::string const& entry) { return SameHost(entry, host); });
    }

    /// Keep a sorted-by-key vector's ordering after an insertion.
    /// @param entries The vector to sort.
    /// @param key How to read the ordering key from an entry.
    template <typename T, typename Key>
    void SortByKey(std::vector<T>& entries, Key key)
    {
        std::ranges::sort(entries, {}, key);
    }
} // namespace

SchedulerEndpointState SchedulerEndpointStateOf(ClusterMember const& member) noexcept
{
    if (!member.schedulerEndpoint.empty())
        return SchedulerEndpointState::Announced;
    return member.schedulerEndpointHistory == SchedulerEndpointHistory::Announced ? SchedulerEndpointState::Cleared
                                                                                  : SchedulerEndpointState::NeverAnnounced;
}

std::string_view SchedulerEndpointStateName(ClusterMember const& member) noexcept
{
    return SchedulerEndpointStateTable[static_cast<std::size_t>(SchedulerEndpointStateOf(member))].name;
}

std::optional<std::string> ClusterState::RaftEndpointOf(std::string_view id) const
{
    auto const it = std::ranges::find(members, id, &ClusterMember::id);
    return it != members.end() ? std::optional { it->raftEndpoint } : std::nullopt;
}

std::optional<std::string> ClusterState::SchedulerEndpointOf(std::string_view id) const
{
    auto const it = std::ranges::find(members, id, &ClusterMember::id);
    if (it == members.end() || it->schedulerEndpoint.empty())
        return std::nullopt;
    return it->schedulerEndpoint;
}

MemberSeat RecordedSeatOf(ClusterState const& state, std::string_view id)
{
    auto const it = std::ranges::find(state.members, id, &ClusterMember::id);
    return it != state.members.end() ? it->seat : MemberSeat::Voter;
}

std::optional<std::string> ClusterState::SettingOf(std::string_view name) const
{
    auto const it = std::ranges::find(settings, name, &Setting::name);
    return it != settings.end() ? std::optional { it->value } : std::nullopt;
}

std::vector<std::string> ClusterState::Endpoints() const
{
    std::vector<std::string> out;
    out.reserve(members.size());
    for (auto const& member: members)
        out.push_back(member.raftEndpoint);
    return out;
}

bool ClusterState::AdmitsClient(std::string_view host) const
{
    return std::ranges::any_of(clients, [host](std::string const& entry) { return SameHost(entry, host); });
}

bool ClusterState::HasForgotten(std::string_view host) const
{
    return std::ranges::any_of(forgotten, [host](std::string const& entry) { return SameHost(entry, host); });
}

bool ClusterState::IsRevoked(Ed25519PublicKey const& key) const
{
    return std::ranges::contains(revokedKeys, key, &RevokedKey::publicKey);
}

std::optional<std::string> ClusterState::HolderOf(Ed25519PublicKey const& key) const
{
    for (auto const& member: members)
        if (member.publicKey == key)
            return member.id;
    for (auto const& principal: principals)
        if (principal.publicKey == key)
            return principal.id;
    return std::nullopt;
}

std::vector<std::byte> Encode(Command const& command)
{
    auto const header = std::array { static_cast<std::byte>(CommandVersion), static_cast<std::byte>(command.kind) };

    // Absent travels as a zero-length field, as it does for every optional on these wires:
    // a role byte of zero would be `Worker`, which is a claim, not an absence.
    auto const role =
        command.role.has_value() ? std::vector { static_cast<std::byte>(*command.role) } : std::vector<std::byte> {};
    return WireFields::Encode({ std::span<std::byte const> { header },
                                WireFields::AsBytes(command.key),
                                WireFields::AsBytes(command.value),
                                WireFields::AsBytes(command.schedulerEndpoint),
                                OptionalKeyBytes(command.publicKey),
                                std::span<std::byte const> { role } });
}

std::expected<Command, ConsensusError> DecodeCommand(std::span<std::byte const> payload)
{
    // The version is read before the arity is judged, so a command another build laid out
    // is refused by NAME rather than as a malformed frame: a v2 command is four fields, and
    // counting them first would report an intact entry as damage.
    auto const all = WireFields::SplitAll(payload);
    if (all.has_value() && !all->empty() && !(*all)[0].empty() && static_cast<std::uint8_t>((*all)[0][0]) != CommandVersion)
        return std::unexpected(
            UnsupportedWireVersion(std::format("cluster command encoding version {} (this build reads {})",
                                               static_cast<std::uint8_t>((*all)[0][0]),
                                               CommandVersion)));

    auto const fields = WireFields::SplitExactly(payload, CommandFields);
    if (!fields.has_value())
        return std::unexpected(MalformedWireFrame("a cluster command is not six fields"));

    auto const header = (*fields)[0];
    if (header.empty())
        return std::unexpected(MalformedWireFrame("a cluster command's header is empty"));
    if (auto const version = static_cast<std::uint8_t>(header[0]); version != CommandVersion)
        return std::unexpected(UnsupportedWireVersion(
            std::format("cluster command encoding version {} (this build reads {})", version, CommandVersion)));
    if (header.size() != 2)
        return std::unexpected(MalformedWireFrame("a cluster command's header is not two bytes"));

    // The verb is checked against the enum here rather than cast and switched on
    // later: a byte this build does not know is a peer speaking a vocabulary it
    // lacks, and applying it as whichever enumerator it happens to alias would
    // change the cluster's state in a way nobody wrote down.
    //
    // Through `DecodeWireEnum` rather than an open-coded bound, which is what this
    // used to be (#197). Why the bound is derived rather than named is documented
    // once, on that function; restating it here is how the two drift.
    auto const kind = Consensus::DecodeWireEnum<CommandKind>(static_cast<std::uint8_t>(header[1]));
    if (!kind.has_value())
        return std::unexpected(UnknownWireMessage(
            std::format("cluster command verb {} this build does not know", static_cast<unsigned>(header[1]))));

    auto const publicKey = ReadKeyField((*fields)[4]);
    if (!publicKey.has_value())
        return std::unexpected(MalformedWireFrame("a cluster command's key is neither absent nor 32 bytes"));

    // A role byte this build does not name is a newer vocabulary, refused by name as an
    // unknown verb is -- applying it as whichever role it aliases would admit a machine to
    // do something nobody granted.
    auto role = std::optional<PrincipalRole> {};
    if (auto const roleField = (*fields)[5]; !roleField.empty())
    {
        if (roleField.size() != 1)
            return std::unexpected(MalformedWireFrame("a cluster command's role is not one byte"));
        role = Consensus::DecodeWireEnum<PrincipalRole>(static_cast<std::uint8_t>(roleField[0]));
        if (!role.has_value())
            return std::unexpected(UnknownWireMessage(
                std::format("cluster command role {} this build does not know", static_cast<unsigned>(roleField[0]))));
    }

    return Command { .kind = *kind,
                     .key = std::string { WireFields::AsStringView((*fields)[1]) },
                     .value = std::string { WireFields::AsStringView((*fields)[2]) },
                     .schedulerEndpoint = std::string { WireFields::AsStringView((*fields)[3]) },
                     .publicKey = *publicKey,
                     .role = role };
}

std::vector<std::byte> Encode(ClusterState const& state)
{
    // Two counted groups rather than a nested record per entry, because a snapshot is
    // read only by this same code: what the nesting buys elsewhere -- a peer stepping
    // over a group it does not understand -- has no reader here, and the version byte
    // already refuses a layout this build did not write.
    std::vector<std::span<std::byte const>> fields;
    auto const header = std::array { static_cast<std::byte>(StateVersion) };
    auto const countOf = [](std::size_t size) {
        return WireFields::ToBigEndian<std::uint32_t>(static_cast<std::uint32_t>(size));
    };
    auto const memberCount = countOf(state.members.size());
    auto const settingCount = countOf(state.settings.size());
    auto const clientCount = countOf(state.clients.size());
    auto const forgottenCount = countOf(state.forgotten.size());
    auto const principalCount = countOf(state.principals.size());
    auto const revokedCount = countOf(state.revokedKeys.size());
    auto const rosterVersion = WireFields::ToBigEndian<std::uint64_t>(state.rosterVersion);

    // Every history and seat byte is written before any span into them is taken,
    // because the list below holds spans and a vector that grew under them would leave
    // each pointing at freed storage. Two bytes per member, interleaved, so one cursor
    // walks both.
    std::vector<std::byte> memberBytes;
    memberBytes.reserve(state.members.size() * 2);
    for (auto const& member: state.members)
    {
        memberBytes.push_back(static_cast<std::byte>(member.schedulerEndpointHistory));
        memberBytes.push_back(static_cast<std::byte>(member.seat));
    }

    // One role byte per principal, for the same reason and under the same rule.
    std::vector<std::byte> roleBytes;
    roleBytes.reserve(state.principals.size());
    for (auto const& principal: state.principals)
        roleBytes.push_back(static_cast<std::byte>(principal.role));

    fields.emplace_back(header);
    fields.emplace_back(memberCount);
    fields.emplace_back(settingCount);
    fields.emplace_back(clientCount);
    fields.emplace_back(forgottenCount);
    fields.emplace_back(principalCount);
    fields.emplace_back(revokedCount);
    fields.emplace_back(rosterVersion);
    auto cursor = std::span<std::byte const> { memberBytes };
    for (auto const& member: state.members)
    {
        fields.push_back(WireFields::AsBytes(member.id));
        fields.push_back(WireFields::AsBytes(member.raftEndpoint));
        fields.push_back(WireFields::AsBytes(member.schedulerEndpoint));
        fields.push_back(cursor.first(1));
        fields.push_back(cursor.subspan(1, 1));
        fields.push_back(OptionalKeyBytes(member.publicKey));
        cursor = cursor.subspan(2);
    }
    for (auto const& setting: state.settings)
    {
        fields.push_back(WireFields::AsBytes(setting.name));
        fields.push_back(WireFields::AsBytes(setting.value));
    }
    for (auto const& client: state.clients)
        fields.push_back(WireFields::AsBytes(client));
    for (auto const& host: state.forgotten)
        fields.push_back(WireFields::AsBytes(host));
    auto roles = std::span<std::byte const> { roleBytes };
    for (auto const& principal: state.principals)
    {
        fields.push_back(WireFields::AsBytes(principal.id));
        fields.emplace_back(principal.publicKey);
        fields.push_back(roles.first(1));
        roles = roles.subspan(1);
    }
    for (auto const& revoked: state.revokedKeys)
    {
        fields.push_back(WireFields::AsBytes(revoked.id));
        fields.emplace_back(revoked.publicKey);
    }
    return WireFields::Encode(WireFields::FieldList { fields });
}

std::expected<ClusterState, ConsensusError> DecodeState(std::span<std::byte const> bytes)
{
    auto const fields = WireFields::SplitAll(bytes);
    if (!fields.has_value() || fields->empty() || (*fields)[0].size() != 1)
        return std::unexpected(MalformedWireFrame("the bytes are not a cluster state"));

    // By name, before anything else about the bytes is judged: a layout this build did
    // not write is intact, and reading it as this build's would report damage that is
    // not there.
    if (auto const version = static_cast<std::uint8_t>((*fields)[0][0]); version != StateVersion)
        return std::unexpected(UnsupportedWireVersion(
            std::format("cluster state encoding version {} (this build reads {})", version, StateVersion)));

    if (fields->size() < StateHeaderFields)
        return std::unexpected(MalformedWireFrame("a cluster state does not state its six counts and its roster version"));
    auto const memberCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[1]);
    auto const settingCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[2]);
    auto const clientCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[3]);
    auto const forgottenCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[4]);
    auto const principalCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[5]);
    auto const revokedCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[6]);
    if (!memberCount.has_value() || !settingCount.has_value() || !clientCount.has_value() || !forgottenCount.has_value()
        || !principalCount.has_value() || !revokedCount.has_value())
        return std::unexpected(MalformedWireFrame("a cluster state's counts are not four bytes each"));
    auto const rosterVersion = WireFields::FromBigEndian<std::uint64_t>((*fields)[RosterVersionField]);
    if (!rosterVersion.has_value())
        return std::unexpected(MalformedWireFrame("a cluster state's roster version is not eight bytes"));

    // Members as sextuples, settings as pairs, clients and forgotten hosts one field each,
    // principals as triples and revoked keys as pairs -- and the TOTAL is checked against
    // what actually arrived. A truncated snapshot must be refused rather than read as a
    // member with an empty endpoint -- that member would be replicated onward as an address
    // nobody can dial -- and a declared count larger than the fields present is the same
    // fault stated by the other end. 64-bit arithmetic, so six counts near `UINT32_MAX`
    // cannot wrap into agreement.
    auto const memberSpan = std::uint64_t { *memberCount } * MemberFields;
    auto const settingSpan = std::uint64_t { *settingCount } * SettingFields;
    auto const principalSpan = std::uint64_t { *principalCount } * PrincipalFields;
    auto const revokedSpan = std::uint64_t { *revokedCount } * RevokedKeyFields;
    auto const expected =
        StateHeaderFields + memberSpan + settingSpan + *clientCount + *forgottenCount + principalSpan + revokedSpan;
    if (expected != fields->size())
        return std::unexpected(MalformedWireFrame("a cluster state's fields do not match its counts"));

    auto const at = [&](std::size_t index) {
        return std::string { WireFields::AsStringView((*fields)[StateHeaderFields + index]) };
    };

    ClusterState state;
    state.rosterVersion = *rosterVersion;
    state.members.reserve(*memberCount);
    state.settings.reserve(*settingCount);
    state.clients.reserve(*clientCount);
    state.forgotten.reserve(*forgottenCount);
    state.principals.reserve(*principalCount);
    state.revokedKeys.reserve(*revokedCount);
    // Walked by member rather than by field: `memberSpan` is exactly `*memberCount` sextuples,
    // so each member's first field is its ordinal times `MemberFields` -- the same indices the
    // stepped counter visited, with the stride stated once instead of in the head.
    for (auto const ordinal: std::views::iota(std::size_t { 0 }, std::size_t { *memberCount }))
    {
        auto const index = ordinal * MemberFields;
        auto const historyField = (*fields)[StateHeaderFields + index + 3];
        auto const history =
            historyField.size() == 1
                ? Consensus::DecodeWireEnum<SchedulerEndpointHistory>(static_cast<std::uint8_t>(historyField[0]))
                : std::nullopt;
        if (!history.has_value())
            return std::unexpected(MalformedWireFrame("a member's scheduler endpoint history names none this build knows"));

        auto const seatField = (*fields)[StateHeaderFields + index + 4];
        auto const seat = seatField.size() == 1
                              ? Consensus::DecodeWireEnum<MemberSeat>(static_cast<std::uint8_t>(seatField[0]))
                              : std::nullopt;
        if (!seat.has_value())
            return std::unexpected(MalformedWireFrame("a member's seat names none this build knows"));

        auto const publicKey = ReadKeyField((*fields)[StateHeaderFields + index + 5]);
        if (!publicKey.has_value())
            return std::unexpected(MalformedWireFrame("a member's key is neither absent nor 32 bytes"));

        auto member = ClusterMember { .id = at(index),
                                      .raftEndpoint = at(index + 1),
                                      .schedulerEndpoint = at(index + 2),
                                      .schedulerEndpointHistory = *history,
                                      .seat = *seat,
                                      .publicKey = *publicKey };

        // The one combination `Apply` never produces. Read as it stands it would be a
        // member holding an endpoint it reports never having announced, and every
        // renderer would have to pick which half to believe.
        if (!member.schedulerEndpoint.empty() && member.schedulerEndpointHistory == SchedulerEndpointHistory::NeverAnnounced)
            return std::unexpected(MalformedWireFrame("a member records a scheduler endpoint it never announced"));
        state.members.push_back(std::move(member));
    }
    auto const settingsEnd = memberSpan + settingSpan;
    for (auto const ordinal: std::views::iota(std::size_t { 0 }, std::size_t { *settingCount }))
    {
        auto const index = memberSpan + (ordinal * SettingFields);
        state.settings.push_back(Setting { .name = at(index), .value = at(index + 1) });
    }
    auto const clientsEnd = settingsEnd + *clientCount;
    for (auto const index: std::views::iota(settingsEnd, clientsEnd))
        state.clients.push_back(at(index));
    auto const forgottenEnd = clientsEnd + *forgottenCount;
    for (auto const index: std::views::iota(clientsEnd, forgottenEnd))
        state.forgotten.push_back(at(index));

    // A principal's key is REQUIRED, so absent is as malformed as a short one here, and a
    // role byte this build does not name is refused rather than read as `Worker`.
    auto const field = [&](std::size_t index) {
        return (*fields)[StateHeaderFields + index];
    };
    for (auto const ordinal: std::views::iota(std::size_t { 0 }, std::size_t { *principalCount }))
    {
        auto const index = forgottenEnd + (ordinal * PrincipalFields);
        auto const publicKey = ReadKeyField(field(index + 1));
        auto const roleField = field(index + 2);
        auto const role = roleField.size() == 1
                              ? Consensus::DecodeWireEnum<PrincipalRole>(static_cast<std::uint8_t>(roleField[0]))
                              : std::nullopt;
        if (!publicKey.has_value() || !publicKey->has_value() || !role.has_value())
            return std::unexpected(
                MalformedWireFrame("a principal carries no 32-byte key, or a role this build does not know"));
        state.principals.push_back(ClusterPrincipal { .id = at(index), .publicKey = **publicKey, .role = *role });
    }
    auto const principalsEnd = forgottenEnd + principalSpan;
    for (auto const ordinal: std::views::iota(std::size_t { 0 }, std::size_t { *revokedCount }))
    {
        auto const index = principalsEnd + (ordinal * RevokedKeyFields);
        auto const publicKey = ReadKeyField(field(index + 1));
        if (!publicKey.has_value() || !publicKey->has_value())
            return std::unexpected(MalformedWireFrame("a revoked key is not 32 bytes"));
        state.revokedKeys.push_back(RevokedKey { .id = at(index), .publicKey = **publicKey });
    }

    if (auto const broken = BrokenRosterRule(state); broken.has_value())
        return std::unexpected(MalformedWireFrame(*broken));
    return state;
}

namespace
{
    /// A member as the roster sees it: who, where it is dialled, which seat, which key --
    /// and none of the scheduler endpoint's bookkeeping, which changes as members lead.
    using RosterMemberFacts =
        std::tuple<std::string const&, std::string const&, MemberSeat, std::optional<Ed25519PublicKey> const&>;

    /// Whether @p before and @p after hold different rosters: the members as the roster sees
    /// them, the principals and the revoked keys.
    /// @param before The state before a command.
    /// @param after The state after it.
    /// @return True when the roster changed.
    [[nodiscard]] bool RosterDiffers(ClusterState const& before, ClusterState const& after)
    {
        auto const facts = [](ClusterMember const& member) {
            return RosterMemberFacts { member.id, member.raftEndpoint, member.seat, member.publicKey };
        };
        return !std::ranges::equal(before.members, after.members, {}, facts, facts) || before.principals != after.principals
               || before.revokedKeys != after.revokedKeys;
    }

    /// Apply @p command to @p state, as `Apply` does before it asks whether the roster moved.
    /// @param state The state.
    /// @param command The committed command.
    void ApplyChange(ClusterState& state, Command const& command)
    {
        switch (command.kind)
        {
            // One arm for both, because they are one verb recording two seats: the seat is
            // the only thing that differs, and `MemberSeatTable` is where it is read from.
            // A member re-admitted through the other verb changes seat and nothing else it
            // did not also restate -- which is how an operator promotes and demotes.
            case CommandKind::AddMember:
            case CommandKind::AddLearner: {
                // Dropped when the rules a key obeys no longer hold (#178): an id that is a
                // principal, or a key that is revoked or somebody else's. `ValidateAgainst`
                // refuses all three before the append; this is the second proposal judged
                // against the same state, committed after the first changed it.
                if (IsPrincipal(state, command.key))
                    return;
                if (command.publicKey.has_value()
                    && StandingOf(state, command.key, *command.publicKey) != KeyStanding::Available)
                    return;

                // Update in place when the id is already known. One verb for "join" and
                // "moved" because they are one intention, and removing first would leave a
                // window in which the cluster has agreed the node does not exist.
                auto const it = std::ranges::find(state.members, command.key, &ClusterMember::id);

                // The endpoint is replaced wholesale and its HISTORY is not, which is what
                // lets a report say *cleared* rather than *never announced* after a re-admit
                // (#1340). Derived here from what the state already records, so no command
                // carries it. Only a removal forgets it: a forget is a positive act, and an
                // id admitted from absence has announced nothing yet.
                auto const announcedBefore =
                    it != state.members.end() && it->schedulerEndpointHistory == SchedulerEndpointHistory::Announced;
                auto const admitted =
                    ClusterMember { .id = command.key,
                                    .raftEndpoint = command.value,
                                    .schedulerEndpoint = command.schedulerEndpoint,
                                    .schedulerEndpointHistory = announcedBefore || !command.schedulerEndpoint.empty()
                                                                    ? SchedulerEndpointHistory::Announced
                                                                    : SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = SeatAdmittedBy(command.kind).value_or(MemberSeat::Voter),
                                    // No opinion keeps what is recorded: a machine that moves
                                    // keeps its identity, and only `Forget` takes one away.
                                    .publicKey = command.publicKey.or_else(
                                        [&] { return it != state.members.end() ? it->publicKey : std::nullopt; }) };
                // Admitting a member at a host re-admits that host: a forget it carries is
                // over (#1309).
                EraseHost(state.forgotten, HostOfEndpoint(command.value));
                if (it != state.members.end())
                {
                    // Wholesale, both endpoints. A record is re-proposed only when it has
                    // changed, and a node that moved moved both of its ports -- so keeping
                    // a scheduler endpoint the command did not repeat would redirect
                    // clients at an address that member no longer answers.
                    *it = admitted;
                    return;
                }
                state.members.push_back(admitted);
                SortByKey(state.members, &ClusterMember::id);
                return;
            }
            case CommandKind::Forget: {
                // Whatever record carries the id -- a member or a principal, never both, which
                // `BrokenRosterRule` holds a decoded state to -- goes, and the key it held is
                // revoked with it (#1555). Derived HERE, from the record being removed, so a key
                // replaced between the proposal and the commit is the one revoked.
                auto revoked = std::vector<Ed25519PublicKey> {};
                if (auto const it = std::ranges::find(state.members, command.key, &ClusterMember::id);
                    it != state.members.end())
                {
                    // A forget is a positive act, so it leaves a TOMBSTONE for the member's host
                    // (#1309): a node whose own `--fleet-member` list still names that machine is
                    // then refused it, rather than serving a decommissioned member until every
                    // list is edited. Never for loopback, which is always this machine's own and a
                    // tombstone could never narrow -- the key is what reaches a member there.
                    auto host = std::string { HostOfEndpoint(it->raftEndpoint) };
                    if (it->publicKey.has_value())
                        revoked.push_back(*it->publicKey);
                    state.members.erase(it);
                    if (!IsLoopbackHost(host))
                        InsertHost(state.forgotten, host);
                }
                else if (auto const principal = std::ranges::find(state.principals, command.key, &ClusterPrincipal::id);
                         principal != state.principals.end())
                {
                    revoked.push_back(principal->publicKey);
                    state.principals.erase(principal);
                }

                // And the key the proposing leader held live for the id (`PrepareForget`), which
                // reaches a member the state records without one or not at all -- one a
                // `--raft-peer` line typed with its key. Never a key another id now holds: that
                // one was admitted since, and revoking it would forget a machine nobody named.
                if (command.publicKey.has_value() && !state.HolderOf(*command.publicKey).has_value())
                    revoked.push_back(*command.publicKey);

                // **Never dropped once there is a key to revoke**: every admitting verb is dropped
                // at commit when its preconditions have stopped holding, and a revocation that
                // could be lost is removal failing OPEN. A key already revoked is not recorded
                // twice.
                for (auto const& key: revoked)
                    if (!state.IsRevoked(key))
                        state.revokedKeys.push_back(RevokedKey { .id = command.key, .publicKey = key });
                std::ranges::sort(
                    state.revokedKeys, {}, [](RevokedKey const& entry) { return std::tie(entry.id, entry.publicKey); });
                return;
            }

            case CommandKind::AdmitClient: {
                auto const host = HostOfEndpoint(command.key);
                InsertHost(state.clients, host);
                EraseHost(state.forgotten, host);
                return;
            }

            case CommandKind::ForgetClient: {
                auto const host = HostOfEndpoint(command.key);
                EraseHost(state.clients, host);
                InsertHost(state.forgotten, host);
                return;
            }

            case CommandKind::AdmitPrincipal: {
                // Dropped rather than applied when any of `ValidateAgainst`'s rules has stopped
                // holding since it was judged -- above all a forget committed first, whose
                // revocation this must never undo.
                if (!command.publicKey.has_value() || !command.role.has_value() || IsMember(state, command.key)
                    || StandingOf(state, command.key, *command.publicKey) != KeyStanding::Available)
                    return;

                auto const admitted =
                    ClusterPrincipal { .id = command.key, .publicKey = *command.publicKey, .role = *command.role };
                auto const it = std::ranges::find(state.principals, command.key, &ClusterPrincipal::id);
                if (it != state.principals.end())
                {
                    *it = admitted;
                    return;
                }
                state.principals.push_back(admitted);
                SortByKey(state.principals, &ClusterPrincipal::id);
                return;
            }

            case CommandKind::SetSetting: {
                auto const it = std::ranges::find(state.settings, command.key, &Setting::name);
                if (it != state.settings.end())
                {
                    it->value = command.value;
                    return;
                }
                state.settings.push_back(Setting { .name = command.key, .value = command.value });
                SortByKey(state.settings, &Setting::name);
                return;
            }

            // Not a verb -- it is the enum's own count, which is what sizes the table in
            // `Validate`. Named rather than swept up by a `default`, because a `default`
            // is what would let a verb added later reach this switch unhandled and be
            // applied as nothing at all, silently.
            case CommandKind::Last:
                break;
        }
    }
} // namespace

void Apply(ClusterState& state, Command const& command)
{
    // The roster version is DERIVED here rather than carried, so every voter applying the
    // same log reaches the same number for the same roster (`ClusterState::rosterVersion`).
    // Asked of the projection after the fact rather than of each verb, because a verb that
    // changes the roster only sometimes -- a re-admission restating what is recorded, a
    // revocation of a key nobody holds -- must not move it, and a per-arm bump is one arm
    // away from moving it on a no-op.
    auto const before = state;
    ApplyChange(state, command);
    if (RosterDiffers(before, state))
        ++state.rosterVersion;
}

std::expected<std::chrono::milliseconds, std::string> ParseLeaseLifetime(std::string_view value)
{
    namespace Wire = CompileCacheWire;

    // The grammar every other length an operator types is read in (#1402), whole: a
    // number and a unit, nothing trailing. A bare number is refused by name -- this
    // setting read `1200000` as milliseconds, and a value committed that way is now
    // unreadable, which `SchedulerService::AgreedLeaseLifetime` degrades on and says so.
    auto const parsed = ParseDuration(value);
    if (!parsed.has_value())
        return std::unexpected(std::format("{}: {}", LeaseLifetimeSetting, DescribeDurationFault(parsed.error(), value)));

    auto const asked = *parsed;

    // The relation `CompileCacheWire`'s static_assert can no longer cover, asked here
    // because here is where a value first exists. An idle bound at or above the total
    // means a healthy worker's own reactor jitter outlives the job, so the fleet reads
    // as stopped -- and the client would give up on silence before the lease it is
    // waiting on could possibly expire, which makes the total bound nothing.
    if (asked <= Wire::DefaultCompileIdleTimeout)
        return std::unexpected(std::format("{} must exceed the {} a client tolerates in silence, or a healthy worker's own "
                                           "jitter reads as a dead one; asked for {}",
                                           LeaseLifetimeSetting,
                                           FormatDuration(Wire::DefaultCompileIdleTimeout),
                                           FormatDuration(asked)));

    // REFUSED, never clamped: see `SettingSpec::refuse`. The ceiling's own reasons are
    // on `MaxCompileLeaseLifetime` and are about replay and about how long a member may
    // hold a compile socket, neither of which an operator can be expected to derive.
    if (asked > Wire::MaxCompileLeaseLifetime)
        return std::unexpected(
            std::format("{} may be at most {}, since it bounds the window a captured grant is replayable in "
                        "across a worker restart; asked for {}",
                        LeaseLifetimeSetting,
                        FormatDuration(Wire::MaxCompileLeaseLifetime),
                        FormatDuration(asked)));

    return asked;
}

std::optional<std::string> RefuseLeaseLifetime(std::string_view value)
{
    // A thin adapter, so the table column and every reader ask ONE function. The two
    // could otherwise agree about the bounds and disagree about the parse -- which is
    // the shape the acceptance/retention rule refuses for the same reason: it is the
    // comparison, not the constant, that is easy to get wrong twice.
    auto parsed = ParseLeaseLifetime(value);
    if (parsed.has_value())
        return std::nullopt;
    return std::move(parsed).error();
}

namespace
{
    /// What `AddMember` and `AddLearner` record; all three become a `ClusterMember`.
    ///
    /// The rows restate the strings `Apply` copies, which is a residual worth naming:
    /// the completeness check below proves one row per VERB, not one entry per field,
    /// so a fourth string added to `Command` and copied by `Apply` would get neither an
    /// entry here nor a compile error. That is the same residual
    /// `RegistrationTextFields` records about `WorkerRegistration`, and the reason both
    /// are tables rather than checks written out.
    constexpr std::array<TextField<Command>, 3> AddMemberText { {
        { .name = "a member id", .project = [](Command const& c) -> std::string_view { return c.key; } },
        { .name = "a member's consensus endpoint", .project = [](Command const& c) -> std::string_view { return c.value; } },
        { .name = "a member's scheduler endpoint",
          .project = [](Command const& c) -> std::string_view { return c.schedulerEndpoint; } },
    } };

    /// What `SetSetting` records that is not already decided by a lookup.
    ///
    /// The name is absent because `FindSetting` settles it: a key this build does not
    /// know is refused whatever its bytes are, so a spelling that is not text cannot
    /// reach the state through that door either.
    constexpr std::array<TextField<Command>, 1> SetSettingText { {
        { .name = "a cluster setting's value", .project = [](Command const& c) -> std::string_view { return c.value; } },
    } };

    /// What `AdmitClient` and `ForgetClient` record: the host, as an entry in `clients`
    /// or in `forgotten`, which every renderer of the state prints.
    constexpr std::array<TextField<Command>, 1> ClientHostText { {
        { .name = "a client host", .project = [](Command const& c) -> std::string_view { return c.key; } },
    } };

    /// What `AdmitPrincipal` records: the id, which every renderer of the state prints.
    constexpr std::array<TextField<Command>, 1> PrincipalText { {
        { .name = "a principal id", .project = [](Command const& c) -> std::string_view { return c.key; } },
    } };

    /// Whether a verb carries one of `Command`'s optional fields (#178).
    ///
    /// **Private: never transmitted or persisted.** Three answers, because the two fields
    /// are three different facts across the verbs: a member's key is an opinion a proposal
    /// may not have, a principal's is what the verb acts on, and every other verb has no use
    /// for one -- where it is a field somebody misunderstood.
    enum class FieldUse : std::uint8_t
    {
        Refused,  ///< The verb has no use for it; carrying one is refused.
        Optional, ///< The verb reads it when present and does without it when not.
        Required, ///< The verb acts on it; one without it is refused.
    };

    /// The shape of one verb: which strings it records, and which optional fields it takes.
    struct CommandShapeRow
    {
        CommandKind kind;                           ///< The verb this row describes.
        std::string_view noun;                      ///< What a refusal calls a command of this verb.
        std::span<TextField<Command> const> fields; ///< What it must be able to name.
        FieldUse publicKey;                         ///< Whether it takes `Command::publicKey`.
        FieldUse role;                              ///< Whether it takes `Command::role`.
    };

    /// One row per `CommandKind`, in enumerator order.
    ///
    /// `Forget`'s text row is empty **by name** rather than by an omission somebody
    /// might tidy up: `Validate` states why, and what belongs here is that an empty row is
    /// a decision and looks like one.
    constexpr EnumTable<CommandKind, CommandShapeRow> CommandShapes { {
        { .kind = CommandKind::AddMember,
          .noun = "a member admission",
          .fields = AddMemberText,
          .publicKey = FieldUse::Optional,
          .role = FieldUse::Refused },
        { .kind = CommandKind::Forget,
          .noun = "a forget",
          .fields = {},
          .publicKey = FieldUse::Optional,
          .role = FieldUse::Refused },
        { .kind = CommandKind::SetSetting,
          .noun = "a setting",
          .fields = SetSettingText,
          .publicKey = FieldUse::Refused,
          .role = FieldUse::Refused },
        { .kind = CommandKind::AdmitClient,
          .noun = "a client command",
          .fields = ClientHostText,
          .publicKey = FieldUse::Refused,
          .role = FieldUse::Refused },
        { .kind = CommandKind::ForgetClient,
          .noun = "a client command",
          .fields = ClientHostText,
          .publicKey = FieldUse::Refused,
          .role = FieldUse::Refused },
        { .kind = CommandKind::AddLearner,
          .noun = "a member admission",
          .fields = AddMemberText,
          .publicKey = FieldUse::Optional,
          .role = FieldUse::Refused },
        { .kind = CommandKind::AdmitPrincipal,
          .noun = "a principal admission",
          .fields = PrincipalText,
          .publicKey = FieldUse::Required,
          .role = FieldUse::Required },
    } };

    static_assert(RowsInEnumeratorOrder(CommandShapes, &CommandShapeRow::kind),
                  "CommandShapes must hold one row per CommandKind, in enumerator order");

    /// Refuse an optional field @p use says the verb may not carry, or must.
    /// @param row The verb's shape.
    /// @param use How the verb uses the field.
    /// @param present Whether the command carries it.
    /// @param what The field, as a refusal names it.
    /// @return Why the command is refused, or nothing.
    [[nodiscard]] std::expected<void, ConsensusError> RefuseFieldUse(CommandShapeRow const& row,
                                                                     FieldUse use,
                                                                     bool present,
                                                                     std::string_view what)
    {
        if (use == FieldUse::Refused && present)
            return std::unexpected(InvalidConfiguration(std::format("{} carries no {}", row.noun, what)));
        if (use == FieldUse::Required && !present)
            return std::unexpected(InvalidConfiguration(std::format("{} must name a {}", row.noun, what)));
        return {};
    }

} // namespace

std::expected<void, ConsensusError> Validate(Command const& command)
{
    // The verb first, because nothing else can be judged without it -- and because a
    // `Command` can be built by a decoder, so indexing the table below is not the way
    // to find out that this build has no row for it.
    auto const verb = static_cast<std::size_t>(command.kind);
    if (verb >= EnumeratorCount<CommandKind>)
        return std::unexpected(InvalidConfiguration("unknown command"));

    if (command.key.empty())
        return std::unexpected(InvalidConfiguration("a cluster command names nothing"));

    // Before the per-verb rules rather than inside them, because the answer is
    // already per-verb: the table is indexed by the verb, and `Forget`'s row is
    // deliberately empty.
    auto const& shape = CommandShapes[verb];
    if (auto const field = FirstFieldNotText(command, shape.fields); field.has_value())
        return std::unexpected(InvalidConfiguration(NotTextRefusal(*field)));

    // The two optional fields (#178), by the same table: which verbs take a key and a role
    // is a column, so a verb added later states it rather than inheriting a default.
    if (auto refused = RefuseFieldUse(shape, shape.publicKey, command.publicKey.has_value(), "public key");
        !refused.has_value())
        return refused;
    if (auto refused = RefuseFieldUse(shape, shape.role, command.role.has_value(), "principal role"); !refused.has_value())
        return refused;

    switch (command.kind)
    {
        case CommandKind::AddMember:
        case CommandKind::AddLearner:
            // An endpoint is required, and this is the check that closes the recorded
            // residual: a member the cluster agreed to admit but cannot reach is worse
            // than one it refused, because the fleet counts it towards quorum and
            // routes to it.
            if (command.value.empty())
                return std::unexpected(InvalidConfiguration("a member must be admitted with an endpoint"));
            return {};

        case CommandKind::Forget:
            if (!command.schedulerEndpoint.empty())
                return std::unexpected(InvalidConfiguration("a forget carries no scheduler endpoint"));
            return {};

        case CommandKind::SetSetting:
            if (!command.schedulerEndpoint.empty())
                return std::unexpected(InvalidConfiguration("a setting carries no scheduler endpoint"));
            // Asked BEFORE the lookup: a key this cluster refuses gets its own answer
            // rather than the one a typo gets. Both are a null `FindSetting`, and
            // *no such cluster setting* reads as a misspelling or as a node too old,
            // which sends an operator to upgrade a machine over a decision. The
            // `static_assert` beside `RefusedSettingTable` is what makes the ORDER
            // here irrelevant rather than load-bearing -- a refusal must not become
            // escapable by a row arriving later and shadowing it.
            if (auto const* const refused = FindRefusedSetting(command.key); refused != nullptr)
                return std::unexpected(
                    InvalidConfiguration(std::format("{} is not a replicated setting: {}", command.key, refused->reason)));

            // Refused HERE rather than ignored at each applier. A key nobody knows
            // would otherwise be replicated to every node, snapshotted, carried across
            // restarts and do nothing -- with the only symptom being that the thing
            // the operator configured did not happen.
            {
                auto const* const spec = FindSetting(command.key);
                if (spec == nullptr)
                    return std::unexpected(InvalidConfiguration("no such cluster setting: " + command.key));

                // The VALUE, once the key is settled. Same argument one level down: a value
                // this build cannot act on would otherwise be replicated, snapshotted and
                // carried across restarts while the thing the operator configured did not
                // happen. A row with no `refuse` says this build constrains nothing here,
                // which is a claim rather than an omission -- see `SettingSpec::refuse`.
                if (spec->refuse != nullptr)
                    if (auto reason = spec->refuse(command.value); reason.has_value())
                        return std::unexpected(InvalidConfiguration(*std::move(reason)));
                return {};
            }

        case CommandKind::AdmitClient:
        case CommandKind::ForgetClient: {
            if (!command.value.empty() || !command.schedulerEndpoint.empty())
                return std::unexpected(InvalidConfiguration("a client command carries a host and nothing else"));
            auto const host = HostOfEndpoint(command.key);
            if (host.empty())
                return std::unexpected(InvalidConfiguration("a client command must name a host"));
            // A caller on a node's own machine is admitted to that node whatever any
            // list says (`ClusterMembership::Classify`), so an ADMIT about loopback
            // would be accepted, replicated and snapshotted while deciding nothing.
            //
            // A FORGET about loopback is the opposite and is the reason this refusal
            // must not be relaxed on the strength of the sentence above: since #1309 a
            // tombstone OUTRANKS every admission route, so an entry naming loopback
            // would refuse the local builds a node exists to serve, on every surface at
            // once. `Distributed::ForgottenVerdicts` guards it a second time, because a
            // rule enforced only here is one a later route can reach around -- and the
            // consequence is invisible from this end.
            if (IsLoopbackHost(host))
                return std::unexpected(InvalidConfiguration(
                    std::format("{} is loopback, which every node always admits from its own machine", host)));
            return {};
        }

        // A principal is admitted by its key alone (#178): it has no consensus endpoint,
        // because consensus never dials it, and no scheduler endpoint, because it never
        // leads. Either one carried is a request for a member, sent through the wrong verb.
        case CommandKind::AdmitPrincipal:
            if (!command.value.empty() || !command.schedulerEndpoint.empty())
                return std::unexpected(
                    InvalidConfiguration(std::format("{} carries an id and a key and nothing else", shape.noun)));
            return {};

        // The count rather than a verb; falls out to the refusal below.
        case CommandKind::Last:
            break;
    }

    return std::unexpected(InvalidConfiguration("unknown command"));
}

std::expected<void, ConsensusError> ValidateAgainst(ClusterState const& state, Command const& command)
{
    if (auto allowed = Validate(command); !allowed.has_value())
        return allowed;

    // The key the command would make live, and whose -- the two admitting verbs' shared
    // question. A member admission with no key has none to ask about.
    auto const refuseKey = [&state, &command]() -> std::expected<void, ConsensusError> {
        if (!command.publicKey.has_value())
            return {};
        auto const& key = *command.publicKey;
        switch (StandingOf(state, command.key, key))
        {
            case KeyStanding::Available:
                return {};
            case KeyStanding::Revoked: {
                // The label the revocation recorded, so the refusal says whose key this was --
                // which is usually the first thing an operator reading it did not know.
                auto const revoked = std::ranges::find(state.revokedKeys, key, &RevokedKey::publicKey);
                return std::unexpected(KeyRevoked(std::format(
                    "{} was revoked{}; a revoked key is never admitted again -- the machine must mint a new identity "
                    "(a fresh --cluster-dir) and be admitted under that",
                    FormatEd25519PublicKey(key),
                    revoked != state.revokedKeys.end() ? std::format(" (it was {}'s)", revoked->id) : std::string {})));
            }
            case KeyStanding::HeldElsewhere:
                return std::unexpected(InvalidConfiguration(
                    std::format("{} is already {}'s key; one key proves one identity, so it cannot also be {}'s",
                                FormatEd25519PublicKey(key),
                                state.HolderOf(key).value_or(std::string {}),
                                command.key)));
        }
        return {};
    };

    switch (command.kind)
    {
        case CommandKind::AddMember:
        case CommandKind::AddLearner:
            // One id, one list: a principal admitted by key is not also a member, and moving
            // one into consensus is a decision this verb must not make on the side.
            if (IsPrincipal(state, command.key))
                return std::unexpected(InvalidConfiguration(
                    std::format("{} is a principal, admitted by its key; it cannot also be a member", command.key)));
            return refuseKey();

        case CommandKind::AdmitPrincipal:
            if (IsMember(state, command.key))
                return std::unexpected(InvalidConfiguration(
                    std::format("{} is a member of this cluster; a principal is a machine that is not", command.key)));
            return refuseKey();

        case CommandKind::Forget:
            // The key the proposer holds live for the id must not be ANOTHER id's, or the
            // forget would revoke a machine nobody named. Held by this id, or by nobody -- a
            // member a `--raft-peer` line typed, recorded nowhere -- is the key it means.
            if (command.publicKey.has_value())
                if (auto const holder = state.HolderOf(*command.publicKey); holder.has_value() && *holder != command.key)
                    return std::unexpected(InvalidConfiguration(
                        std::format("{} is {}'s key, not {}'s; a forget revokes only the key of the machine it names",
                                    FormatEd25519PublicKey(*command.publicKey),
                                    *holder,
                                    command.key)));
            return {};

        case CommandKind::SetSetting:
        case CommandKind::AdmitClient:
        case CommandKind::ForgetClient:
            return {};

        // `Validate` refused it above; named rather than swept up by a `default`, for the
        // reason `Apply` names it.
        case CommandKind::Last:
            break;
    }
    return std::unexpected(InvalidConfiguration("unknown command"));
}

} // namespace FastCache::Cluster
