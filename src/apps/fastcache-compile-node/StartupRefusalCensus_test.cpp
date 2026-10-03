// SPDX-License-Identifier: Apache-2.0
//
// Every refusal a node's tiers can return, and why each one is not a row of the startup table.
//
// `--print-surfaces`, `--install-service` and a start judge a command line through
// `StartupPolicyRejection` before any tier exists. A refusal that depends on nothing but the
// parsed configuration and still lives only in a tier is a line the worksheet and the install
// accept and every start of the registered service then refuses, into a log nobody reads -- the
// shape found twice in one survey (TLS on a build without it, and one id typed twice as a
// `--raft-peer`). So every `return std::unexpected` in the files below is a row here saying which
// kind it is, and a new one that is not is a failure to be classified rather than a default.
//
// A `Belt` row's claim -- that an earlier refusal answers its site first -- is checked too: it
// names where that refusal lives and the text it is spelled with, and the census finds that text
// inside that function. A claim nothing verifies is how a new tier refusal escapes by being
// labelled `Belt`. What is checked is that the earlier refusal EXISTS where the row says, not that
// it refuses every configuration the tier's does.
//
// And each start-time refusal says how the process ends: a statement that builds a
// `NodeRefusal` names its `NodeRefusalCause`, and its row names the same one, so the audit of
// which refusals a retry can change is written HERE, per site, and `NodeRefusal_test` holds each
// cause to its exit. A row naming no cause is a site whose statement names none: a helper whose
// text a typed caller relays under a cause of its own row, or a relay of a refusal already typed.
//
// **Blind spot, and the direction it fails in: OPEN.** Only refusals spelled
// `return std::unexpected` in the scanned files are counted. One returned any other way -- an
// `std::expected` built into a variable first, a `co_return`, an error carried out through an
// out-parameter, or a file the glob does not reach -- is simply not seen, and this passes.

#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

/// Why a tier's refusal is not (only) a row of the startup table.
///
/// Private to this census: never transmitted or persisted.
enum class RefusalKind : std::uint8_t
{
    Belt,       ///< An earlier refusal -- the startup table, or `main` before any tier -- answers it first.
    NeedsIo,    ///< It depends on what the filesystem, the network or the host holds.
    Relay,      ///< It forwards an error another site produced, and that site is classified.
    NotAtStart, ///< Not a refusal of a start: a one-shot verb, or a request at run time.
    Defect,     ///< How this build wired the node: no configuration or host reaches it, and a test pins that none does.
};

/// The earlier refusal a `Belt` row says answers its site first: a file, and the function the
/// refusal's text must be inside.
///
/// Private to this census: never transmitted or persisted.
enum class Earlier : std::uint8_t
{
    None,            ///< Not a `Belt` row.
    StartupTable,    ///< `StartupPolicyRejection`, which judges a start before any tier exists.
    MainBeforeTiers, ///< `main`, before it builds a tier.
    OptionTable,     ///< A value parser of `NodeOptions()`, which refuses the command line itself.
    Last,
};

/// Where one `Earlier` lives.
struct EarlierRow
{
    Earlier earlier;        ///< Which.
    std::string_view file;  ///< The file, under the node's source directory; empty for `None`.
    std::string_view opens; ///< The line that opens the function; the body runs to the next `}` in column 0.
};

constexpr auto EarlierRows = FastCache::EnumTable<Earlier, EarlierRow> { {
    { .earlier = Earlier::None, .file = "", .opens = "" },
    { .earlier = Earlier::StartupTable,
      .file = "NodeConfig.cpp",
      .opens = "std::optional<std::string> StartupPolicyRejection(NodeConfig const& cfg)" },
    { .earlier = Earlier::MainBeforeTiers, .file = "main.cpp", .opens = "int main(int argc, char** argv)" },
    { .earlier = Earlier::OptionTable,
      .file = "NodeConfig.cpp",
      .opens = "std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept" },
} };
static_assert(FastCache::RowsInEnumeratorOrder(EarlierRows, [](EarlierRow const& row) { return row.earlier; }),
              "every Earlier needs a row, at its own index");

/// One classified refusal site, or several sharing one spelling.
struct CensusRow
{
    std::string_view file;             ///< The file, under the node's source directory.
    std::string_view anchor;           ///< Text in the refusal's statement that identifies it.
    std::size_t count;                 ///< How many sites in @p file the anchor identifies.
    RefusalKind kind;                  ///< Why it is where it is.
    std::string_view what;             ///< What answers it first, or what it depends on.
    Earlier earlier { Earlier::None }; ///< `Belt` only: where the refusal that answers first lives.
    std::string_view answeredBy {};    ///< `Belt` only: text that refusal is spelled with there.
    std::string_view cause {};         ///< The `NodeRefusalCause` its statement spells, or empty.
};

constexpr auto Census = std::to_array<CensusRow>({
    { .file = "CacheTier.cpp",
      .anchor = "\"cannot create {}: {}\"",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "creating the --cache-dir" },
    { .file = "CacheTier.cpp",
      .anchor = "options.path.string(), spec.inUse",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the store's exclusive claim" },
    { .file = "CacheTier.cpp",
      .anchor = "options.path.string(), opened.error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "opening the store file" },
    { .file = "CacheTier.cpp",
      .anchor = "std::move(disk.error())",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "the disk tier's open, above" },
    { .file = "CacheTier.cpp",
      .anchor = "\"--cache-dir {}\", storage.error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "the storage build, above",
      .cause = "CacheStore" },
    { .file = "CacheTier.cpp",
      .anchor = "--migrate-cache needs --cache-dir",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "the --migrate-cache verb" },
    { .file = "CacheTier.cpp",
      .anchor = "std::move(line)",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "the --migrate-cache verb's conversion" },
    { .file = "ConsensusTier.cpp",
      .anchor = "ConsensusNeedsNodeIdRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "main resolves this node's id before any tier",
      .earlier = Earlier::MainBeforeTiers,
      .answeredBy = "AdoptNodeIdentity(cfg, cliOnly, identityRandom, logger, publicKey)" },
    { .file = "ConsensusTier.cpp",
      .anchor = "ConsensusNamesNoDialAddressRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's own row",
      .earlier = Earlier::StartupTable,
      .answeredBy = "ConsensusNamesNoDialAddressRefusal" },
    { .file = "ConsensusTier.cpp",
      .anchor = "std::move(self).error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "ConsensusSelfMemberOf, above",
      .cause = "EarlierRule" },
    { .file = "ConsensusTier.cpp",
      .anchor = "ConsensusNeedsIdentityKeyRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "main resolves or refuses the identity key before any tier",
      .earlier = Earlier::MainBeforeTiers,
      .answeredBy = "ExitCodeFor(StageOf(identityKey.error().fault))",
      .cause = "EarlierRule" },
    { .file = "ConsensusTier.cpp",
      .anchor = "resolved.error()",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's surface grammar walk",
      .earlier = Earlier::StartupTable,
      .answeredBy = "for (auto const& surface: NodeSurfaceTable())",
      .cause = "EarlierRule" },
    { .file = "ConsensusTier.cpp",
      .anchor = "\"cannot open {}: {}{}\"",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "opening the Raft store",
      .cause = "ConsensusStore" },
    { .file = "ConsensusTier.cpp",
      .anchor = "std::move(started).error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "Launch, below" },
    { .file = "ConsensusTier.cpp",
      .anchor = "is not bound: {}",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the listener a supervisor handed over",
      .cause = "HandedOverListeners" },
    { .file = "ConsensusTier.cpp",
      .anchor = "cannot be matched to {}",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the listener a supervisor handed over",
      .cause = "HandedOverListeners" },
    { .file = "ConsensusTier.cpp",
      .anchor = "is bound to {}, and the",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the listener a supervisor handed over",
      .cause = "HandedOverListeners" },
    { .file = "ConsensusTier.cpp",
      .anchor = "and this node's mode binds no",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "a listener a supervisor handed over for a mode that binds no consensus port",
      .cause = "HandedOverListeners" },
    { .file = "ConsensusTier.cpp",
      .anchor = "cannot serve the listener handed to consensus",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "adopting the listener a supervisor handed over",
      .cause = "Listener" },
    { .file = "ConsensusTier.cpp",
      .anchor = "std::move(judged).error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the consensus port",
      .cause = "Listener" },
    { .file = "ConsensusTier.cpp",
      .anchor = "BindToleranceUnsupported",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the consensus port",
      .cause = "Listener" },
    { .file = "ConsensusTier.cpp",
      .anchor = "is not a dialable endpoint for",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the members the formation record in the state directory names",
      .cause = "EarlierRule" },
    { .file = "ConsensusTier.cpp",
      .anchor = "cannot recover consensus state",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "reading the Raft store",
      .cause = "ConsensusStore" },
    { .file = "ConsensusTier.cpp",
      .anchor = "node.error().context",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the bootstrap voters the formation record in the state directory names",
      .cause = "EarlierRule" },
    { .file = "ConsensusTier.cpp",
      .anchor = "UnreadableConsensusStateRefusal",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "what the Raft store holds",
      .cause = "ConsensusState" },
    { .file = "ConsensusTier.cpp",
      .anchor = "prepared.error()",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "a proposal at run time" },
    { .file = "ConsensusTier.cpp",
      .anchor = "allowed.error()",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "a proposal at run time" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "{ std::format(\"--discovery={} is not",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's surface grammar walk",
      .earlier = Earlier::StartupTable,
      .answeredBy = "for (auto const& surface: NodeSurfaceTable())" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "std::move(destinations).error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "BeaconDestinationsFor, above",
      .cause = "EarlierRule" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "resolved {} endpoint(s)",
      .count = 1,
      .kind = RefusalKind::Defect,
      .what = "the discovery row's resolver",
      .cause = "BuildDefect" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "DiscoveryAnnouncesOnlyThisMachineRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's own row",
      .earlier = Earlier::StartupTable,
      .answeredBy = "DiscoveryAnnouncesOnlyThisMachineRefusal",
      .cause = "EarlierRule" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "<address>:<port>\", cfg.discoveryAddress))",
      .count = 2,
      .kind = RefusalKind::Belt,
      .what = "the startup table's surface grammar walk",
      .earlier = Earlier::StartupTable,
      .answeredBy = "for (auto const& surface: NodeSurfaceTable())",
      .cause = "EarlierRule" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "std::move(judged).error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the discovery port",
      .cause = "Listener" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "BindToleranceUnsupported",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the discovery port",
      .cause = "Listener" },
    { .file = "DiscoveryTier.cpp",
      .anchor = "DiscoveryNeedsConsensusRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's own row",
      .earlier = Earlier::StartupTable,
      .answeredBy = "DiscoveryNeedsConsensusRefusal",
      .cause = "EarlierRule" },
    { .file = "SchedulerTier.cpp",
      .anchor = "SchedulerNeedsIdentityKeyRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "main resolves or refuses the identity key before any tier",
      .earlier = Earlier::MainBeforeTiers,
      .answeredBy = "ExitCodeFor(StageOf(identityKey.error().fault))",
      .cause = "EarlierRule" },
    { .file = "SharedCacheTier.cpp",
      .anchor = "std::move(opened.error())",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "the shared tier, opened at run time once the fleet names this machine" },
    { .file = "WorkerTier.cpp",
      .anchor = "row.name, row.remedy",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "claiming a scratch root" },
    { .file = "WorkerTier.cpp",
      .anchor = "this worker has nowhere to register",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's closed-consensus row: a worker registers where its formation says",
      .earlier = Earlier::StartupTable,
      .answeredBy = "WorkerConsensusClosed(c)",
      .cause = "EarlierRule" },
    { .file = "WorkerTier.cpp",
      .anchor = "a malformed --toolchain was named",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "--toolchain's value parser",
      .earlier = Earlier::OptionTable,
      .answeredBy = "AppendFrom<&NodeConfig::toolchains, ParseToolchain>()",
      .cause = "EarlierRule" },
    { .file = "WorkerTier.cpp",
      .anchor = "std::move(claim).error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "the scratch claim, above",
      .cause = "ScratchRoot" },
    { .file = "WorkerTier.cpp",
      .anchor = "std::move(validator).error()",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "MakeWorkerLeaseValidator",
      .cause = "LeaseValidation" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "\"cannot read '{}'\"",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "reading a secret file",
      .cause = "CredentialIo" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "could not read all of",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "reading a secret file",
      .cause = "CredentialIo" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "is empty",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "what a secret file holds",
      .cause = "CredentialFile" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "FleetTextRefusal::UnknownSelector",
      .count = 2,
      .kind = RefusalKind::NotAtStart,
      .what = "a fleet text request" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "--dashboard-token-file {}",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "reading a secret file, above" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "created.error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "the TLS material on disk, or making it",
      .cause = "TlsMaterial" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "TlsUnavailableRefusal",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's TLS-in-this-build row",
      .earlier = Earlier::StartupTable,
      .answeredBy = "TlsUnavailableRefusal",
      .cause = "EarlierRule" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "--admin-listen {}",
      .count = 1,
      .kind = RefusalKind::Relay,
      .what = "AdminEndpoint::Start, below",
      .cause = "Listener" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "resolved.error()",
      .count = 1,
      .kind = RefusalKind::Belt,
      .what = "the startup table's surface grammar walk",
      .earlier = Earlier::StartupTable,
      .answeredBy = "for (auto const& surface: NodeSurfaceTable())" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "std::move(judged).error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the admin port" },
    { .file = "AdminEndpoint.cpp",
      .anchor = "BindToleranceUnsupported",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the admin port" },
    { .file = "NodeRoster.cpp",
      .anchor = "NodeRefusalCause::KeptRosterIo",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "reading the kept roster",
      .cause = "KeptRosterIo" },
    { .file = "NodeRoster.cpp",
      .anchor = "NodeRefusalCause::KeptRoster, std::move(loaded).error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "what the kept roster holds",
      .cause = "KeptRoster" },
    { .file = "NodeRoster.cpp",
      .anchor = "RosterlessWorkerRefusal",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "whether a kept roster exists; the table refuses the rest",
      .cause = "KeptRoster" },
    { .file = "WorkerLease.cpp",
      .anchor = "a socket-activated worker that admits peers",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "whether the listener was inherited" },
    { .file = "WorkerLease.cpp",
      .anchor = "ReloadWidensUncheckedWorkerRefusal",
      .count = 1,
      .kind = RefusalKind::NotAtStart,
      .what = "a reload that would widen admission on a worker that verifies no lease" },
    { .file = "NodeFrameSurface.cpp",
      .anchor = "started.error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the node port" },
    { .file = "NodeFrameSurface.cpp",
      .anchor = "std::move(judged).error()",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the node port",
      .cause = "Listener" },
    { .file = "NodeFrameSurface.cpp",
      .anchor = "wires its surfaces",
      .count = 1,
      .kind = RefusalKind::Defect,
      .what = "whether main routed every family every built node serves",
      .cause = "BuildDefect" },
    { .file = "NodeFrameSurface.cpp",
      .anchor = "BindToleranceUnsupported",
      .count = 1,
      .kind = RefusalKind::NeedsIo,
      .what = "binding the node port",
      .cause = "Listener" },
});

/// The files named besides the `*Tier.cpp` glob: the ones a start reaches before or between tiers.
constexpr auto NamedFiles = std::to_array<std::string_view>({
    "AdminEndpoint.cpp",
    "NodeRoster.cpp",
    "WorkerLease.cpp",
    "NodeFrameSurface.cpp",
});

constexpr std::string_view RefusalSpelling = "return std::unexpected";

/// One scanned file.
struct SourceFile
{
    std::string name; ///< Its file name.
    std::string text; ///< Its text, comment lines blanked.
};

/// @p text with whole-line `//` comments blanked: a comment naming a refusal is not one.
/// @param text A source file.
/// @return The same text, comment-only lines emptied.
[[nodiscard]] std::string WithoutCommentLines(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (auto const line: std::views::split(text, '\n'))
    {
        std::string_view const view { line.begin(), line.end() };
        auto const first = view.find_first_not_of(" \t");
        if (first == std::string_view::npos || !view.substr(first).starts_with("//"))
            out.append(view);
        out.push_back('\n');
    }
    return out;
}

/// The statement each refusal site opens: from the spelling to the first `;` that ends a line.
/// @param text A scanned file's text.
/// @return One entry per site, in order.
[[nodiscard]] std::vector<std::string_view> RefusalStatements(std::string_view text)
{
    std::vector<std::string_view> statements;
    auto at = text.find(RefusalSpelling);
    while (at != std::string_view::npos)
    {
        auto end = text.find(";\n", at);
        if (end == std::string_view::npos)
            end = text.size();
        statements.push_back(text.substr(at, end - at));
        at = text.find(RefusalSpelling, at + RefusalSpelling.size());
    }
    return statements;
}

/// What the census found wrong: each entry a sentence naming the site or the row.
/// @param files The scanned files.
/// @param rows The classification.
/// @return Empty when every site matches exactly one row and every row its count.
[[nodiscard]] std::vector<std::string> CensusFindings(std::span<SourceFile const> files, std::span<CensusRow const> rows)
{
    std::vector<std::string> findings;
    std::vector<std::size_t> matched(rows.size(), 0);
    for (auto const& file: files)
    {
        for (auto const statement: RefusalStatements(file.text))
        {
            std::vector<std::size_t> owners;
            for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
                if (rows[index].file == file.name && statement.contains(rows[index].anchor))
                    owners.push_back(index);
            if (owners.size() == 1)
                ++matched[owners.front()];
            else
                findings.push_back(std::format("{}: a refusal matching {} rows: {}",
                                               file.name,
                                               owners.size(),
                                               statement.substr(0, std::min<std::size_t>(statement.size(), 160))));
        }
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, rows.size()))
        if (matched[index] != rows[index].count)
            findings.push_back(std::format("row {} '{}': identifies {} site(s), the row says {}",
                                           rows[index].file,
                                           rows[index].anchor,
                                           matched[index],
                                           rows[index].count));
    return findings;
}

/// What is wrong with the causes @p rows name: each entry a sentence naming the site or the row.
/// @param files The scanned files.
/// @param rows The classification.
/// @return Empty when every site's statement spells exactly the cause its row names, or none when
///         the row names none.
[[nodiscard]] std::vector<std::string> CauseFindings(std::span<SourceFile const> files, std::span<CensusRow const> rows)
{
    constexpr std::string_view Spelling = "NodeRefusalCause::";
    std::vector<std::string> findings;
    for (auto const& file: files)
        for (auto const statement: RefusalStatements(file.text))
            for (auto const& row: rows)
            {
                if (row.file != file.name || !statement.contains(row.anchor))
                    continue;
                auto const spelled = std::format("{}{}", Spelling, row.cause);
                if (row.cause.empty() ? statement.contains(Spelling) : !statement.contains(spelled))
                    findings.push_back(std::format("{} '{}': the row names cause '{}', and the statement is: {}",
                                                   file.name,
                                                   row.anchor,
                                                   row.cause,
                                                   statement.substr(0, std::min<std::size_t>(statement.size(), 160))));
            }
    return findings;
}

/// One of the node's source files, comment lines blanked.
/// @param name The file name, under the node's source directory.
/// @return Its text; the case has failed when it is missing.
[[nodiscard]] std::string NodeSource(std::string_view name)
{
    auto const path = std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node" / name;
    INFO(path.string());
    REQUIRE(std::filesystem::exists(path));
    // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
    std::ifstream in { path, std::ios::binary };
    REQUIRE(in);
    std::ostringstream contents;
    contents << in.rdbuf();
    return WithoutCommentLines(std::move(contents).str());
}

/// What is wrong with the `Belt` claims in @p rows: each entry a sentence naming the row.
/// @param rows The classification.
/// @param source Reads one of the node's source files, comment lines blanked.
/// @return Empty when every `Belt` row names an earlier refusal whose text is where it says, and
///         no other row names one.
template <typename ReadSource>
[[nodiscard]] std::vector<std::string> BeltFindings(std::span<CensusRow const> rows, ReadSource const& source)
{
    std::vector<std::string> findings;
    for (auto const& row: rows)
    {
        auto const belt = row.kind == RefusalKind::Belt;
        if (!belt)
        {
            if (row.earlier != Earlier::None || !row.answeredBy.empty())
                findings.push_back(
                    std::format("row {} '{}': names an earlier refusal and is not Belt", row.file, row.anchor));
            continue;
        }
        if (row.earlier == Earlier::None || row.answeredBy.empty())
        {
            findings.push_back(std::format("row {} '{}': Belt, and names no earlier refusal", row.file, row.anchor));
            continue;
        }
        auto const& at = EarlierRows[static_cast<std::size_t>(row.earlier)];
        auto const text = source(at.file);
        auto const opens = text.find(at.opens);
        if (opens == std::string::npos)
        {
            findings.push_back(std::format("row {} '{}': {} does not open '{}'", row.file, row.anchor, at.file, at.opens));
            continue;
        }
        auto const closes = text.find("\n}\n", opens);
        auto const body = std::string_view { text }.substr(opens, closes == std::string::npos ? text.npos : closes - opens);
        if (!body.contains(row.answeredBy))
            findings.push_back(
                std::format("row {} '{}': '{}' is not in {}'s {}", row.file, row.anchor, row.answeredBy, at.file, at.opens));
    }
    return findings;
}

/// Every file the census scans, read from the tree.
/// @return The files; the case has failed when a named one is missing.
[[nodiscard]] std::vector<SourceFile> ScannedFiles()
{
    auto const directory = std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node";
    std::vector<std::filesystem::path> paths;
    for (auto const& entry: std::filesystem::directory_iterator { directory })
    {
        auto const name = entry.path().filename().string();
        if (name.ends_with("Tier.cpp") && !name.ends_with("_test.cpp"))
            paths.push_back(entry.path());
    }
    for (auto const name: NamedFiles)
        paths.push_back(directory / name);
    std::ranges::sort(paths);

    std::vector<SourceFile> files;
    for (auto const& path: paths)
    {
        INFO(path.string());
        REQUIRE(std::filesystem::exists(path));
        // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
        std::ifstream in { path, std::ios::binary };
        REQUIRE(in);
        std::ostringstream contents;
        contents << in.rdbuf();
        files.push_back(
            SourceFile { .name = path.filename().string(), .text = WithoutCommentLines(std::move(contents).str()) });
    }
    return files;
}

} // namespace

TEST_CASE("Every refusal a node tier returns is classified against the startup table", "[node][startup][census]")
{
    auto const files = ScannedFiles();
    // A glob that stopped matching is a census of nothing agreeing with everything.
    REQUIRE(std::ranges::count_if(files, [](SourceFile const& file) { return file.name.ends_with("Tier.cpp"); }) >= 5);

    auto const findings = CensusFindings(files, Census);
    for (auto const& finding: findings)
        UNSCOPED_INFO(finding);
    CHECK(findings.empty());

    SECTION("a refusal nobody classified is a finding")
    {
        // The planted positive: the real tree plus one unclassified refusal, which must be named.
        auto planted = files;
        planted.front().text.append("    return std::unexpected { std::string { \"planted refusal\" } };\n");
        auto const found = CensusFindings(planted, Census);
        REQUIRE(found.size() == 1);
        CHECK(found.front().contains("planted refusal"));
        CHECK(found.front().contains("matching 0 rows"));
    }

    SECTION("a row whose refusal is gone is a finding")
    {
        // A row describing a site that moved into the startup table, or was deleted, must go too:
        // a stale row is a classification of nothing.
        auto rows = std::vector<CensusRow> { Census.begin(), Census.end() };
        rows.push_back(CensusRow { .file = "CacheTier.cpp",
                                   .anchor = "a refusal that is not there",
                                   .count = 1,
                                   .kind = RefusalKind::Belt,
                                   .what = "nothing" });
        auto const found = CensusFindings(files, rows);
        REQUIRE(found.size() == 1);
        CHECK(found.front().contains("identifies 0 site(s), the row says 1"));
    }

    SECTION("a Belt row's claim is found where it says")
    {
        auto const beltFindings = BeltFindings(Census, NodeSource);
        for (auto const& finding: beltFindings)
            UNSCOPED_INFO(finding);
        CHECK(beltFindings.empty());
        CHECK(std::ranges::count(Census, RefusalKind::Belt, &CensusRow::kind) > 0);

        // The planted positives: a claim naming text its earlier refusal does not hold, and a Belt
        // row naming no earlier refusal at all, are each named.
        // Searched in the copy, a `std::vector`, whose iterator is a class type on every standard
        // library -- `std::array`'s is a raw pointer on one, and the qualifier clang-tidy then asks
        // for does not compile on another.
        auto rows = std::vector<CensusRow> { Census.begin(), Census.end() };
        auto const planted = std::ranges::find(rows, RefusalKind::Belt, &CensusRow::kind);
        REQUIRE(planted != rows.end());
        planted->answeredBy = "text no earlier refusal holds";
        auto const stale = BeltFindings(rows, NodeSource);
        REQUIRE(stale.size() == 1);
        CHECK(stale.front().contains("text no earlier refusal holds"));

        planted->earlier = Earlier::None;
        auto const unnamed = BeltFindings(rows, NodeSource);
        REQUIRE(unnamed.size() == 1);
        CHECK(unnamed.front().contains("names no earlier refusal"));
    }

    SECTION("every refusal spells the cause its row names")
    {
        auto const causeFindings = CauseFindings(files, Census);
        for (auto const& finding: causeFindings)
            UNSCOPED_INFO(finding);
        CHECK(causeFindings.empty());
        CHECK(std::ranges::count_if(Census, [](CensusRow const& row) { return !row.cause.empty(); }) > 0);

        // The planted positives: a row naming a cause its statement does not spell, and a row
        // naming none over a statement that spells one, are each named.
        auto rows = std::vector<CensusRow> { Census.begin(), Census.end() };
        auto const typed = std::ranges::find_if(rows, [](CensusRow const& row) { return !row.cause.empty(); });
        REQUIRE(typed != rows.end());
        typed->cause = "NotACause";
        auto const wrong = CauseFindings(files, rows);
        REQUIRE(wrong.size() == 1);
        CHECK(wrong.front().contains("NotACause"));

        typed->cause = {};
        auto const unnamed = CauseFindings(files, rows);
        REQUIRE(unnamed.size() == 1);
        CHECK(unnamed.front().contains("the row names cause ''"));
    }

    SECTION("a comment naming a refusal is not one")
    {
        auto commented = files;
        commented.front().text = WithoutCommentLines(
            commented.front().text + "    // return std::unexpected { std::string { \"only a comment\" } };\n");
        CHECK(CensusFindings(commented, Census).empty());
    }
}
