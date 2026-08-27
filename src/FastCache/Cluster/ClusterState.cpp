// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <utility>

namespace FastCache::Cluster
{

std::optional<ClusterMember> ParseMemberSpec(std::string_view spec)
{
    auto const split = spec.find('=');
    if (split == std::string_view::npos)
        return std::nullopt;

    auto const id = spec.substr(0, split);
    auto const endpoint = spec.substr(split + 1);
    if (id.empty() || endpoint.empty())
        return std::nullopt;

    // Both halves of the question a dialer asks. A split alone is not enough:
    // `10.0.0.4:0` splits and names no port anybody can connect to, so a member
    // accepted on that basis is one the cluster counts and never reaches.
    auto const parts = SplitHostPort(endpoint);
    if (!parts.has_value() || !ParseTcpPort(parts->second).has_value())
        return std::nullopt;

    // Every field named, including the one this token cannot carry. A member's
    // scheduler endpoint is a port peers never connect to, so nothing an operator
    // types about a PEER could supply it -- the node announces its own. Saying so
    // with `{}` rather than leaving it out is what keeps a field added to the
    // middle of the struct from becoming a silent zero here.
    return ClusterMember { .id = std::string { id }, .raftEndpoint = std::string { endpoint }, .schedulerEndpoint = {} };
}

namespace
{
    /// Wire tag in front of every encoded command and state.
    ///
    /// Versioned for the reason every other format here is: a node recovering a
    /// snapshot written by a build that arranged the fields differently must refuse
    /// it rather than read it as this build's arrangement. A snapshot is the one
    /// thing that outlives a process, so a format that could not say which one wrote
    /// it would make an upgrade a silent corruption.
    constexpr std::uint8_t StateVersion = 2;

    /// Fields one member occupies in an encoded state: id, Raft, scheduler.
    ///
    /// Named because the decoder's arithmetic is otherwise three unexplained
    /// threes, and getting one of them wrong reads every member's scheduler
    /// endpoint as the next member's id -- which decodes, and produces a state
    /// nothing would report as wrong.
    constexpr std::size_t MemberFields = 3;

    /// Fields one setting occupies: name, value.
    constexpr std::size_t SettingFields = 2;

    /// Keep a sorted-by-key vector's ordering after an insertion.
    /// @param entries The vector to sort.
    /// @param key How to read the ordering key from an entry.
    template <typename T, typename Key>
    void SortByKey(std::vector<T>& entries, Key key)
    {
        std::ranges::sort(entries, {}, key);
    }
} // namespace

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

std::vector<std::byte> Encode(Command const& command)
{
    auto const header = std::array { static_cast<std::byte>(StateVersion), static_cast<std::byte>(command.kind) };
    return WireFields::Encode({ std::span<std::byte const> { header },
                                WireFields::AsBytes(command.key),
                                WireFields::AsBytes(command.value),
                                WireFields::AsBytes(command.schedulerEndpoint) });
}

std::optional<Command> DecodeCommand(std::span<std::byte const> payload)
{
    auto const fields = WireFields::SplitExactly(payload, 4);
    if (!fields.has_value())
        return std::nullopt;

    auto const header = (*fields)[0];
    if (header.size() != 2 || static_cast<std::uint8_t>(header[0]) != StateVersion)
        return std::nullopt;

    // The verb is checked against the enum here rather than cast and switched on
    // later: a byte this build does not know is a peer speaking a vocabulary it
    // lacks, and applying it as whichever enumerator it happens to alias would
    // change the cluster's state in a way nobody wrote down.
    //
    // Bounded by the enum's own count rather than by its last enumerator BY NAME.
    // The two agree today and stop agreeing the moment a verb is appended: the
    // name-anchored form then refuses the new verb, on a peer that understands it
    // perfectly, and says nothing about why.
    auto const kindRaw = static_cast<std::size_t>(header[1]);
    if (kindRaw >= EnumeratorCount<CommandKind>)
        return std::nullopt;

    return Command { .kind = static_cast<CommandKind>(kindRaw),
                     .key = std::string { WireFields::AsStringView((*fields)[1]) },
                     .value = std::string { WireFields::AsStringView((*fields)[2]) },
                     .schedulerEndpoint = std::string { WireFields::AsStringView((*fields)[3]) } };
}

std::vector<std::byte> Encode(ClusterState const& state)
{
    // Two counted groups rather than a nested record per entry, because a snapshot is
    // read only by this same code: what the nesting buys elsewhere -- a peer stepping
    // over a group it does not understand -- has no reader here, and the version byte
    // already refuses a layout this build did not write.
    std::vector<std::span<std::byte const>> fields;
    auto const header = std::array { static_cast<std::byte>(StateVersion) };
    auto const memberCount = WireFields::ToBigEndian<std::uint32_t>(static_cast<std::uint32_t>(state.members.size()));

    fields.emplace_back(header);
    fields.emplace_back(memberCount);
    for (auto const& member: state.members)
    {
        fields.push_back(WireFields::AsBytes(member.id));
        fields.push_back(WireFields::AsBytes(member.raftEndpoint));
        fields.push_back(WireFields::AsBytes(member.schedulerEndpoint));
    }
    for (auto const& setting: state.settings)
    {
        fields.push_back(WireFields::AsBytes(setting.name));
        fields.push_back(WireFields::AsBytes(setting.value));
    }
    return WireFields::Encode(WireFields::FieldList { fields });
}

std::optional<ClusterState> DecodeState(std::span<std::byte const> bytes)
{
    auto const fields = WireFields::SplitAll(bytes);
    if (!fields.has_value() || fields->size() < 2)
        return std::nullopt;

    if ((*fields)[0].size() != 1 || static_cast<std::uint8_t>((*fields)[0][0]) != StateVersion)
        return std::nullopt;

    auto const memberCount = WireFields::FromBigEndian<std::uint32_t>((*fields)[1]);
    if (!memberCount.has_value())
        return std::nullopt;

    // Members first as triples, settings after as pairs, and BOTH shapes are checked
    // against what actually arrived. A truncated snapshot must be refused rather than
    // read as a member with an empty endpoint -- that member would be replicated
    // onward as an address nobody can dial -- and a declared member count larger than
    // the fields present is the same fault stated by the other end.
    auto const rest = fields->size() - 2;
    auto const memberSpan = static_cast<std::size_t>(*memberCount) * MemberFields;
    if (memberSpan > rest || (rest - memberSpan) % SettingFields != 0)
        return std::nullopt;

    auto const at = [&](std::size_t index) {
        return std::string { WireFields::AsStringView((*fields)[2 + index]) };
    };

    ClusterState state;
    state.members.reserve(*memberCount);
    state.settings.reserve((rest - memberSpan) / SettingFields);
    for (std::size_t index = 0; index < memberSpan; index += MemberFields)
        state.members.push_back(
            ClusterMember { .id = at(index), .raftEndpoint = at(index + 1), .schedulerEndpoint = at(index + 2) });
    for (std::size_t index = memberSpan; index < rest; index += SettingFields)
        state.settings.push_back(Setting { .name = at(index), .value = at(index + 1) });
    return state;
}

void Apply(ClusterState& state, Command const& command)
{
    switch (command.kind)
    {
        case CommandKind::AddMember: {
            // Update in place when the id is already known. One verb for "join" and
            // "moved" because they are one intention, and removing first would leave a
            // window in which the cluster has agreed the node does not exist.
            auto const admitted = ClusterMember { .id = command.key,
                                                  .raftEndpoint = command.value,
                                                  .schedulerEndpoint = command.schedulerEndpoint };
            auto const it = std::ranges::find(state.members, command.key, &ClusterMember::id);
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
        case CommandKind::RemoveMember:
            std::erase_if(state.members, [&](auto const& member) { return member.id == command.key; });
            return;

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

namespace
{
    /// What `AddMember` records; all three become a `ClusterMember`.
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

    /// Which strings each verb records, in one place.
    struct CommandTextRow
    {
        CommandKind kind;                           ///< The verb this row describes.
        std::span<TextField<Command> const> fields; ///< What it must be able to name.
    };

    /// One row per `CommandKind`, in enumerator order.
    ///
    /// `RemoveMember`'s row is empty **by name** rather than by an omission somebody
    /// might tidy up. `Validate` states why; what belongs here is that an empty row
    /// is a decision and looks like one.
    constexpr EnumTable<CommandKind, CommandTextRow> CommandTextFields { {
        { .kind = CommandKind::AddMember, .fields = AddMemberText },
        { .kind = CommandKind::RemoveMember, .fields = {} },
        { .kind = CommandKind::SetSetting, .fields = SetSettingText },
    } };

    static_assert(RowsInEnumeratorOrder(CommandTextFields, &CommandTextRow::kind),
                  "CommandTextFields must hold one row per CommandKind, in enumerator order");

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
    // already per-verb: the table is indexed by the verb, and `RemoveMember`'s row is
    // deliberately empty.
    if (auto const field = FirstFieldNotText(command, CommandTextFields[verb].fields); field.has_value())
        return std::unexpected(InvalidConfiguration(NotTextRefusal(*field)));

    switch (command.kind)
    {
        case CommandKind::AddMember:
            // An endpoint is required, and this is the check that closes the recorded
            // residual: a member the cluster agreed to admit but cannot reach is worse
            // than one it refused, because the fleet counts it towards quorum and
            // routes to it.
            if (command.value.empty())
                return std::unexpected(InvalidConfiguration("a member must be admitted with an endpoint"));
            return {};

        case CommandKind::RemoveMember:
            if (!command.schedulerEndpoint.empty())
                return std::unexpected(InvalidConfiguration("a removal carries no scheduler endpoint"));
            return {};

        case CommandKind::SetSetting:
            if (!command.schedulerEndpoint.empty())
                return std::unexpected(InvalidConfiguration("a setting carries no scheduler endpoint"));
            // Refused HERE rather than ignored at each applier. A key nobody knows
            // would otherwise be replicated to every node, snapshotted, carried across
            // restarts and do nothing -- with the only symptom being that the thing
            // the operator configured did not happen.
            if (FindSetting(command.key) == nullptr)
                return std::unexpected(InvalidConfiguration("no such cluster setting: " + command.key));
            return {};

        // The count rather than a verb; falls out to the refusal below.
        case CommandKind::Last:
            break;
    }

    return std::unexpected(InvalidConfiguration("unknown command"));
}

} // namespace FastCache::Cluster
