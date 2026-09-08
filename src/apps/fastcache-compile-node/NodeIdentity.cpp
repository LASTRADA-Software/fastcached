// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace FastCache::Node
{

namespace
{
    /// Lowercase hex, written out rather than formatted per byte.
    constexpr std::string_view HexDigits = "0123456789abcdef";

    /// Trim the trailing newline a text file is written with, and any spacing.
    ///
    /// A recorded identity is one line, and the writer below ends it with `\n` so the
    /// file is readable with `cat`. An operator who edits it deliberately may leave a
    /// CR, a trailing space or a blank line; none of those is a different identity, and
    /// treating them as one would make an admitted member unreachable over whitespace.
    ///
    /// Only the ENDS, never the middle: an identity with an interior space is a value
    /// this build did not mint and did not accept as `--node-id`, and silently
    /// rewriting it would be the repair a renderer must never make.
    /// @param text What the file held.
    /// @return The same text without leading or trailing whitespace.
    [[nodiscard]] std::string_view Trimmed(std::string_view text) noexcept
    {
        auto const spacing = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        while (!text.empty() && spacing(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && spacing(text.back()))
            text.remove_suffix(1);
        return text;
    }

    /// Read the identity file, if there is one.
    ///
    /// Sized from the stream and read in one call, deliberately NOT through
    /// `std::istreambuf_iterator`. GCC 14 at `-O3` inlines that iterator's `sbumpc`
    /// far enough to see a possibly-null `streambuf` and reports
    /// `-Werror=null-dereference` inside `<streambuf>` itself -- a false positive this
    /// project cannot silence, warnings being errors and the rule being to fix them at
    /// the source. **Invisible on MSVC and under `clang-debug`**: only a `gcc-release`
    /// build sees it, which is why the iterator form survived every local run here and
    /// died on the reference build.
    ///
    /// The same defect reached `NodeCredential_test.cpp` on another branch in the same
    /// week, and the idiom is written out a third time in `fastcache-cc/FileBytes.hpp`
    /// and once more in `Config/DefaultConfigPath_test.cpp`. Four authors of one read
    /// is what [#1029](https://github.com/LASTRADA-Software/fastcached/issues/1029) is
    /// for -- a SCAN, since a comment stating the rule reaches only the files that
    /// already obey it. Not consolidated here: this file has no business owning a
    /// shared file-reading seam, and reaching into `FastCache::Cc` for one would put a
    /// cross-app include in the node's identity code to save nine lines.
    /// @param path The file.
    /// @return Its contents, or nothing when it does not exist or cannot be read.
    [[nodiscard]] std::optional<std::string> ReadIdentityFile(std::filesystem::path const& path)
    {
        auto stream = std::ifstream { path, std::ios::binary | std::ios::ate };
        if (!stream.is_open())
            return std::nullopt;

        auto const size = stream.tellg();
        if (size < 0)
            return std::nullopt;
        stream.seekg(0, std::ios::beg);

        std::string text(static_cast<std::size_t>(size), '\0');
        if (!text.empty() && !stream.read(text.data(), size))
            return std::nullopt;
        return text;
    }

    /// Write @p id into @p path, so that a crash cannot leave half of one.
    ///
    /// Through a temporary and a rename, which is not ceremony here: a torn identity
    /// file is refused by `ResolveNodeIdentity` rather than re-minted -- that is the
    /// safe direction, and it means a partially-written file would stop this node
    /// starting until somebody deleted it by hand. The window is small and the cost of
    /// landing in it is a machine that will not boot.
    /// @param path Where the identity belongs.
    /// @param id What to record.
    /// @return Nothing, or why it could not be written.
    [[nodiscard]] std::expected<void, std::string> WriteIdentityFile(std::filesystem::path const& path, std::string_view id)
    {
        auto const temporary = std::filesystem::path { path }.concat(".new");
        {
            auto stream = std::ofstream { temporary, std::ios::binary | std::ios::trunc };
            if (!stream.is_open())
                return std::unexpected { std::format("cannot write {}", temporary.string()) };
            stream << id << '\n';
            stream.flush();
            if (!stream.good())
                return std::unexpected { std::format("cannot write {}", temporary.string()) };
        }

        auto failure = std::error_code {};
        std::filesystem::rename(temporary, path, failure);
        if (failure)
            return std::unexpected { std::format(
                "cannot record the node identity in {}: {}", path.string(), failure.message()) };
        return {};
    }

    /// What a node says at startup about where its identity came from.
    ///
    /// `EnumTable` so the extent comes from the enum and every row's position is checked
    /// at compile time: a fourth origin added without a sentence would otherwise render
    /// as an empty parenthesis, which is the state this enum exists to stop -- "kept the
    /// identity it had" and "invented one" read identically without one.
    constexpr auto OriginSentences = EnumTable<NodeIdentityOrigin, std::string_view> {
        "from --node-id, recorded",
        "recorded",
        "newly minted; this node has not been admitted to any cluster yet",
    };
    static_assert(std::ranges::none_of(OriginSentences, [](std::string_view text) { return text.empty(); }),
                  "every origin needs a sentence; an empty one reads as no answer at all");
} // namespace

std::string_view DescribeNodeIdentityOrigin(NodeIdentityOrigin origin) noexcept
{
    return OriginSentences[static_cast<std::size_t>(origin)];
}

std::filesystem::path NodeStateDirectory(NodeConfig const& cfg)
{
    if (!cfg.clusterDir.empty())
        return cfg.clusterDir;
    return std::filesystem::path { "fastcache-cluster" };
}

IdentityNeed NodeIdentityNeed(NodeConfig const& cfg) noexcept
{
    // Consensus first, because everything else here is a refinement of it: a node that
    // opens no consensus port has no identity to keep and no cluster to be admitted to.
    if (!RunsConsensus(cfg))
        return IdentityNeed::None;

    // The verbs that answer and exit. Each of them would otherwise CREATE a state
    // directory as a side effect of being asked a question -- `--print-surfaces` says
    // in its own comment that it opens nothing and changes nothing, a cluster verb is a
    // client dialling somebody else's scheduler, and removing a registration is the
    // recovery an operator reaches for when the configuration is already wrong.
    if (cfg.printSurfaces || cfg.uninstallService || cfg.cluster.action != ClusterAction::None)
        return IdentityNeed::None;

    // What is left is a node that will run consensus, or an `--install-service` that
    // registers a command line which will. The second is why this is not simply "am I
    // about to start": a registration replays its command line forever, so the value
    // baked into it has to be the one this machine will actually answer to -- otherwise
    // a re-image resolves a different id under a registration nobody edited, and the
    // cluster counts a member that no longer exists beside a stranger nobody admitted.
    return IdentityNeed::Mint;
}

std::string MintNodeId(IRandomSource& random)
{
    // Two draws rather than one, because a `std::uint64_t` is 64 bits and this is 128.
    // Written as hex rather than as a UUID: nothing here parses it, it appears in Raft
    // messages, in discovery beacons and on a dashboard, and a shape that looks like a
    // UUID invites somebody to read structure into bits that have none.
    auto const draw = [&random] {
        return random.UniformInRange(0, std::numeric_limits<std::uint64_t>::max());
    };

    std::string id;
    id.reserve(MintedNodeIdLength);
    for (auto const half: std::array { draw(), draw() })
        for (auto const shift: std::views::iota(std::size_t { 0 }, std::size_t { 16 }))
            id.push_back(HexDigits[(half >> (60U - (4U * shift))) & 0xFU]);
    return id;
}

std::expected<NodeIdentity, std::string> ResolveNodeIdentity(std::filesystem::path const& stateDirectory,
                                                             std::string_view configured,
                                                             IRandomSource& random)
{
    auto const path = stateDirectory / NodeIdentityFileName;

    // The recorded value is read FIRST even when `--node-id` was typed, because the
    // interesting case is the two disagreeing: an operator who renames a member has
    // changed the thing the cluster admitted, and this is the one moment anybody can
    // be told so. The flag still wins -- it is an override, and refusing it would
    // leave a wrongly-recorded id unfixable except by deleting a file.
    auto const recorded = ReadIdentityFile(path);
    if (recorded.has_value())
    {
        auto const trimmed = Trimmed(*recorded);

        // Refused, never re-minted. A file that is empty or is not text is an identity
        // this build cannot read, and re-minting over it would replace an id the
        // cluster may already have admitted -- silently, on a machine whose only
        // symptom is that a member it used to be is now a member it is not.
        if (trimmed.empty())
            return std::unexpected { std::format("{} is empty: it should hold this node's identity, and an empty one "
                                                 "cannot be told from an identity this node has lost. Delete it to "
                                                 "mint a new identity, which the cluster must then admit",
                                                 path.string()) };
        if (!IsValidUtf8(trimmed))
            return std::unexpected { std::format("{} does not hold text: this node's identity travels in Raft "
                                                 "messages, in discovery beacons and onto a dashboard, and every one "
                                                 "of those reads it back out as text",
                                                 path.string()) };

        if (configured.empty() || configured == trimmed)
            return NodeIdentity { .id = std::string { trimmed },
                                  .origin =
                                      configured.empty() ? NodeIdentityOrigin::Recorded : NodeIdentityOrigin::Configured };
    }

    auto const minted = configured.empty() ? MintNodeId(random) : std::string { configured };

    auto failure = std::error_code {};
    std::filesystem::create_directories(stateDirectory, failure);
    if (failure)
        return std::unexpected { std::format("cannot create {}: {}", stateDirectory.string(), failure.message()) };

    if (auto const written = WriteIdentityFile(path, minted); !written.has_value())
        return std::unexpected { written.error() };

    return NodeIdentity { .id = minted,
                          .origin = configured.empty() ? NodeIdentityOrigin::Minted : NodeIdentityOrigin::Configured };
}

void ApplyNodeIdentity(NodeConfig& cfg, NodeIdentity const& identity)
{
    // An identity with no id is an invocation that needed none -- a node running no
    // consensus. Answered here rather than at each call site so the reload path can
    // call this unconditionally: a branch inside that lambda is a branch that has to be
    // right in a file no test target builds.
    if (identity.id.empty())
        return;

    cfg.nodeId = identity.id;

    // Nothing to synthesise: either the operator named this node's own `--raft-peer`
    // -- which they can only have done for an id they typed -- or they gave no
    // `--raft-self`, and the startup table has already refused a consensus node that
    // names itself neither way.
    if (cfg.raftSelf.empty() || ClusterSelfMember(cfg) != nullptr)
        return;

    // Through `RaftSelfEndpoint`, which the startup rule that refuses a CONTRADICTING
    // `--raft-peer` also asks: written out here as well, the two would disagree about
    // IPv6 bracketing and that rule would refuse every reload of a node it had just
    // accepted at startup.
    auto const endpoint = RaftSelfEndpoint(cfg);
    if (endpoint.empty())
        return;

    // `schedulerEndpoint` empty, which is what this node knows about itself here: the
    // scheduler port it will ANNOUNCE is the one its listener actually binds, and that
    // is not known until it has bound. `ConsensusTier::Start` fills it in when this
    // node announces its own record, exactly as it does for a typed `--raft-peer`.
    cfg.raftPeers.push_back(Cluster::ClusterMember { .id = identity.id, .raftEndpoint = endpoint, .schedulerEndpoint = {} });
}

} // namespace FastCache::Node
