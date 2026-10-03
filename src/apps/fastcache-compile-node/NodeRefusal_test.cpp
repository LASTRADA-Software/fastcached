// SPDX-License-Identifier: Apache-2.0
#include "NodeRefusal.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using FastCache::ProcessExit;
using FastCache::Node::NodeRefusalCause;

namespace
{

/// One cause, and the exit it must end a start with.
struct CauseVerdict
{
    NodeRefusalCause cause; ///< The cause.
    ProcessExit exit;       ///< How the process must end.
    std::string_view why;   ///< Why the next start does or does not reach the same verdict.
};

/// Every cause, written out here rather than read back from the table, so a row whose exit is
/// changed -- or a cause added without a decision -- fails by name.
constexpr auto CauseVerdicts = std::to_array<CauseVerdict>({
    { .cause = NodeRefusalCause::EarlierRule,
      .exit = ProcessExit::Refused,
      .why = "a rule of the configuration refuses it again" },
    { .cause = NodeRefusalCause::HandedOverListeners,
      .exit = ProcessExit::Refused,
      .why = "the socket unit hands over the same listeners, and --advertise is the same" },
    { .cause = NodeRefusalCause::KeptRoster,
      .exit = ProcessExit::Refused,
      .why = "a kept roster read and found unusable or foreign is read the same at the next start" },
    { .cause = NodeRefusalCause::CredentialFile,
      .exit = ProcessExit::Refused,
      .why = "a credential file read and found empty is read the same at the next start" },
    { .cause = NodeRefusalCause::LeaseValidation,
      .exit = ProcessExit::Refused,
      .why = "a socket-activated worker that admits peers is the same configuration next time" },
    { .cause = NodeRefusalCause::ConsensusState,
      .exit = ProcessExit::Refused,
      .why = "a Raft state this build cannot read is still unreadable to it" },
    { .cause = NodeRefusalCause::KeptRosterIo,
      .exit = ProcessExit::Failed,
      .why = "a kept roster that could not be read may be readable at the next start" },
    { .cause = NodeRefusalCause::CredentialIo,
      .exit = ProcessExit::Failed,
      .why = "a credential file that could not be opened or read may be at the next start" },
    { .cause = NodeRefusalCause::Listener,
      .exit = ProcessExit::Failed,
      .why = "a port held by a process that was exiting is free at the next start" },
    { .cause = NodeRefusalCause::CacheStore,
      .exit = ProcessExit::Failed,
      .why = "a store held by another process, or a directory on a volume not yet mounted, may not be" },
    { .cause = NodeRefusalCause::ConsensusStore,
      .exit = ProcessExit::Failed,
      .why = "a Raft store held by another process may not be at the next start" },
    { .cause = NodeRefusalCause::TlsMaterial,
      .exit = ProcessExit::Failed,
      .why = "a certificate read mid-rotation is whole at the next start" },
    { .cause = NodeRefusalCause::ScratchRoot,
      .exit = ProcessExit::Failed,
      .why = "a scratch root held by an exiting worker is free at the next start" },
    { .cause = NodeRefusalCause::ToolchainSurvey,
      .exit = ProcessExit::Failed,
      .why = "a compiler installed since is found by the next survey" },
    { .cause = NodeRefusalCause::BuildDefect,
      .exit = ProcessExit::Refused,
      .why = "the same build wires the same components the same way at the next start" },
});

/// The node's `main.cpp`.
/// @return Its bytes; the case has failed when it cannot be read.
[[nodiscard]] std::string NodeMain()
{
    auto const path =
        std::filesystem::path { FASTCACHED_SOURCE_DIR } / "src" / "apps" / "fastcache-compile-node" / "main.cpp";
    REQUIRE(std::filesystem::exists(path));
    // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
    std::ifstream in { path, std::ios::binary };
    REQUIRE(in);
    std::ostringstream contents;
    contents << in.rdbuf();
    return std::move(contents).str();
}

/// How often @p needle occurs in @p text.
/// @param text Where to look.
/// @param needle What to count; never empty.
/// @return The number of non-overlapping occurrences.
[[nodiscard]] std::size_t Occurrences(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto at = text.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = text.find(needle, at + needle.size());
    }
    return count;
}

} // namespace

TEST_CASE("A node's start gives up as a refusal only when the next start reaches the same verdict", "[node][startup][exit]")
{
    for (auto const cause: FastCache::Enumerators<NodeRefusalCause>())
        CHECK(std::ranges::count(CauseVerdicts, cause, &CauseVerdict::cause) == 1);

    for (auto const& verdict: CauseVerdicts)
    {
        INFO(static_cast<int>(verdict.cause) << ": " << verdict.why);
        CHECK(FastCache::Node::ExitOf(verdict.cause) == verdict.exit);
        CHECK(FastCache::Node::ExitCodeFor(verdict.cause) == FastCache::ExitCodeOf(verdict.exit));
    }
}

TEST_CASE("The node's body gives up with the cause its refusal carries, never one of its own", "[node][startup][exit]")
{
    // `main` is in no test target, so what holds it to the table is its text: every refusal from a
    // tier, a surface, the state directory or a handed-over socket ends with that refusal's own
    // cause -- eleven of them -- and the worker's late ending with its own. None ends as
    // `StartStage::Serving` any more, which was one answer for all of them.
    auto const text = NodeMain();
    CHECK(Occurrences(text, ".error().cause);") == 11);
    CHECK(Occurrences(text, "return ExitCodeFor(") == 13);
    CHECK(Occurrences(text, "->Ending()") == 1);
    CHECK(Occurrences(text, "StartStage::Serving") == 0);
    CHECK(Occurrences(text, "EndedInRefusal") == 0);
}
