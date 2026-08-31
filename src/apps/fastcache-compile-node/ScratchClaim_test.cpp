// SPDX-License-Identifier: Apache-2.0
#include "CompileJob.hpp"
#include "ScratchClaim.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <latch>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Node;
using FastCache::Testing::ScratchDirectory;

namespace
{

/// A claimant driven by a script, so the paths that matter are reachable.
///
/// The two the production claimant cannot be asked for without spawning processes
/// are exactly the two worth testing: a root held by a foreign LIVE owner, and one
/// left behind by a foreign DEAD owner. A guard whose refusal path is only ever
/// exercised by an end-to-end run is a guard nobody can regression-test.
class ScriptedClaimant final: public IScratchClaimant
{
  public:
    /// One scripted answer.
    struct Answer
    {
        bool granted { true };                                      ///< Whether a claim comes back.
        bool reclaimed { false };                                   ///< Whether it had leftovers.
        ScratchClaimRefusal refusal { ScratchClaimRefusal::InUse }; ///< Used when not granted.
    };

    /// @param answers What to return, in order.
    explicit ScriptedClaimant(std::vector<Answer> answers):
        _answers { std::move(answers) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<IScratchClaim>, ScratchClaimRefusal> Claim(std::filesystem::path const& base,
                                                                                           std::size_t /*maxRoots*/) override
    {
        REQUIRE(_next < _answers.size());
        auto const answer = _answers.at(_next++);
        if (!answer.granted)
            return std::unexpected(answer.refusal);
        return std::make_unique<Claim_>(base / "node-0", answer.reclaimed);
    }

  private:
    class Claim_ final: public IScratchClaim
    {
      public:
        Claim_(std::filesystem::path root, bool reclaimed):
            _root { std::move(root) },
            _reclaimed { reclaimed }
        {
        }
        [[nodiscard]] std::filesystem::path const& Root() const noexcept override
        {
            return _root;
        }
        [[nodiscard]] bool Reclaimed() const noexcept override
        {
            return _reclaimed;
        }

      private:
        std::filesystem::path _root;
        bool _reclaimed;
    };

    std::vector<Answer> _answers;
    std::size_t _next { 0 };
};

/// Write a file into @p dir, so a later claim has something to reclaim.
/// @param dir Where to write.
/// @param name The file name.
void Litter(std::filesystem::path const& dir, std::string const& name)
{
    std::filesystem::create_directories(dir);
    std::ofstream { dir / name } << "left behind";
}

} // namespace

TEST_CASE("A scratch root is claimed exclusively, and a second claimant gets another", "[node][scratch-claim]")
{
    // The whole point of #279: two nodes on one host must not derive one root. The
    // claimants here are two objects rather than two processes, which is the harder
    // case -- `flock` is per open file DESCRIPTION, so this also pins that the guard
    // is not an `fcntl` lock, which is per PROCESS and would let both succeed.
    ScratchDirectory base { "fc-scratchclaim" };
    auto const first = MakeLockFileScratchClaimant();
    auto const second = MakeLockFileScratchClaimant();

    auto a = first->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(a.has_value());
    auto b = second->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(b.has_value());

    CHECK((*a)->Root() != (*b)->Root());
    CHECK(std::filesystem::is_directory((*a)->Root()));
    CHECK(std::filesystem::is_directory((*b)->Root()));
}

TEST_CASE("Releasing a claim frees the root for the next claimant", "[node][scratch-claim]")
{
    // The claim lives exactly as long as the object. This is what makes a restarted
    // node reuse its own root rather than marching up the numbers forever.
    ScratchDirectory base { "fc-scratchclaim-release" };
    auto const claimant = MakeLockFileScratchClaimant();

    std::filesystem::path taken;
    {
        auto held = claimant->Claim(base.Path(), DefaultMaxScratchRoots);
        REQUIRE(held.has_value());
        taken = (*held)->Root();
    } // released

    auto again = claimant->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(again.has_value());
    CHECK((*again)->Root() == taken);
}

TEST_CASE("A root left behind by a dead owner is reclaimed, not refused", "[node][scratch-claim]")
{
    // `WorkerServer`'s abandoned-drain path is `std::_Exit` (#239), which bypasses
    // every destructor -- so a node that took that path leaves its job directories
    // on disk. Its LOCK, though, is released by the OS however it died. So a root
    // whose lock is free and whose contents are not is reclaimable by definition,
    // and no staleness has to be inferred from a pid or a timestamp.
    ScratchDirectory base { "fc-scratchclaim-dead" };
    Litter(base.Path() / "node-0", "job-1-leftovers.o");

    auto const claimant = MakeLockFileScratchClaimant();
    auto held = claimant->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(held.has_value());

    CHECK((*held)->Root() == base.Path() / "node-0");
    CHECK((*held)->Reclaimed());
    CHECK(std::filesystem::is_directory((*held)->Root()));
    CHECK(std::filesystem::is_empty((*held)->Root()));
}

TEST_CASE("A fresh root is not reported as reclaimed", "[node][scratch-claim]")
{
    // Otherwise the counter would rise on every first start and mean nothing.
    ScratchDirectory base { "fc-scratchclaim-fresh" };
    auto const claimant = MakeLockFileScratchClaimant();
    auto held = claimant->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(held.has_value());
    CHECK_FALSE((*held)->Reclaimed());
}

TEST_CASE("The claim file lives BESIDE the root, so emptying the root cannot destroy it", "[node][scratch-claim]")
{
    // Load-bearing twice. Reclaiming empties the root, and so does ordinary success:
    // `CompileJobRunner`'s `ScratchGuard` removes each job directory beneath it. A
    // lock file inside would be destroyed by normal operation. Worse on POSIX --
    // deleting an `flock`'d file is legal, and a second process may then create a
    // fresh file at the same path and lock THAT, giving two live owners of one root.
    ScratchDirectory base { "fc-scratchclaim-beside" };
    auto const claimant = MakeLockFileScratchClaimant();
    auto held = claimant->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(held.has_value());

    auto const root = (*held)->Root();
    auto expected = root;
    expected += ".claim";
    CHECK(std::filesystem::exists(expected));
    CHECK_FALSE(std::filesystem::exists(root / ".claim"));

    // And emptying the root leaves the claim untouched.
    std::filesystem::remove_all(root);
    CHECK(std::filesystem::exists(expected));
}

TEST_CASE("Every root being held is refused by name, and differently from being unable to make one", "[node][scratch-claim]")
{
    // Two refusals rather than one "no scratch", because they send an operator to
    // different places: every root held is a machine running more nodes than it has
    // roots for, while being unable to create one is a disk or a permission.
    ScratchDirectory base { "fc-scratchclaim-exhausted" };
    auto const holder = MakeLockFileScratchClaimant();

    // Hold the only candidate, then ask for one with the same ceiling.
    auto held = holder->Claim(base.Path(), 1);
    REQUIRE(held.has_value());

    auto const contender = MakeLockFileScratchClaimant();
    auto refused = contender->Claim(base.Path(), 1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ScratchClaimRefusal::InUse);
    CHECK(DescribeScratchClaimRefusal(refused.error()).name == "scratch-roots-exhausted");
    CHECK(DescribeScratchClaimRefusal(ScratchClaimRefusal::Unavailable).name == "scratch-unavailable");

    // Each row says what to actually do, which is the half a bare name leaves out.
    CHECK_FALSE(DescribeScratchClaimRefusal(ScratchClaimRefusal::InUse).remedy.empty());
    CHECK_FALSE(DescribeScratchClaimRefusal(ScratchClaimRefusal::Unavailable).remedy.empty());
}

TEST_CASE("A claimant walks past roots that are held", "[node][scratch-claim]")
{
    // Three nodes on one machine is unusual but legal, and each must get its own.
    ScratchDirectory base { "fc-scratchclaim-walk" };
    constexpr std::size_t Nodes = 3;

    // The claims are held for the whole case, because a released one is immediately
    // reusable -- which is the property the case above pins and would make this one
    // pass for the wrong reason.
    std::vector<std::unique_ptr<IScratchClaimant>> claimants;
    std::vector<std::unique_ptr<IScratchClaim>> alive;
    std::vector<std::filesystem::path> roots;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Nodes))
    {
        claimants.push_back(MakeLockFileScratchClaimant());
        auto held = claimants.back()->Claim(base.Path(), DefaultMaxScratchRoots);
        REQUIRE(held.has_value());
        roots.push_back((*held)->Root());
        alive.push_back(std::move(*held));
    }
    std::ranges::sort(roots);
    CHECK(std::ranges::adjacent_find(roots) == roots.end());
    CHECK(roots.size() == Nodes);
}

TEST_CASE("The seam lets a caller rehearse a live owner and a dead one", "[node][scratch-claim]")
{
    // What the injected interface is FOR: neither of these is reachable against the
    // real filesystem without a second process, and both are the interesting cases.
    ScratchDirectory base { "fc-scratchclaim-scripted" };

    SECTION("a foreign live owner is a named refusal")
    {
        ScriptedClaimant claimant { { { .granted = false, .reclaimed = false, .refusal = ScratchClaimRefusal::InUse } } };
        auto refused = claimant.Claim(base.Path(), DefaultMaxScratchRoots);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == ScratchClaimRefusal::InUse);
    }

    SECTION("a foreign dead owner is reclaimed and reported")
    {
        ScriptedClaimant claimant { { { .granted = true, .reclaimed = true, .refusal = ScratchClaimRefusal::InUse } } };
        auto held = claimant.Claim(base.Path(), DefaultMaxScratchRoots);
        REQUIRE(held.has_value());
        CHECK((*held)->Reclaimed());
    }

    SECTION("a filesystem that cannot lock is Unavailable, never an unclaimed root")
    {
        ScriptedClaimant claimant {
            { { .granted = false, .reclaimed = false, .refusal = ScratchClaimRefusal::Unavailable } }
        };
        auto refused = claimant.Claim(base.Path(), DefaultMaxScratchRoots);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == ScratchClaimRefusal::Unavailable);
    }
}

namespace
{
/// A process runner that echoes each invocation's own SOURCE back as its object,
/// and does not return until both jobs are inside it at once.
///
/// Both halves are the test. The echo makes "this job got the other's bytes"
/// visible, where a canned object would look identical either way; the barrier
/// makes the overlap a fact rather than a hope. Without it one job finishes before
/// the other starts, nothing is contended, and the case passes having tested
/// nothing -- which is how the defect survived in the first place.
class OverlappingEchoRunner final: public FastCache::Cc::IProcessRunner
{
  public:
    static constexpr std::size_t Overlap = 2;

    FastCache::Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }

    FastCache::Cc::CompileRun RunCaptureSplit(std::span<std::string const> argv) override
    {
        std::string source;
        std::string object;
        for (auto const& arg: argv)
        {
            if (arg.starts_with("-o") && arg.size() > 2)
                object = arg.substr(2);
            else if (arg.ends_with(".cpp"))
                source = arg;
        }

        {
            auto guard = std::unique_lock { _mutex };
            ++_inside;
            _changed.notify_all();
            // Bounded, and it says what it waited for: the other job reaching the
            // compiler. Unbounded here would hang the suite rather than fail it.
            (void) _changed.wait_for(guard, std::chrono::seconds { 5 }, [this] { return _inside >= Overlap; });
        }

        std::ifstream in { source, std::ios::binary | std::ios::ate };
        auto const size = in.tellg();
        std::string text;
        if (size > 0)
        {
            text.resize(static_cast<std::size_t>(size));
            in.seekg(0, std::ios::beg);
            in.read(text.data(), size);
        }
        std::ofstream { object, std::ios::binary } << text;
        return FastCache::Cc::CompileRun { .exitCode = 0, .out = {}, .err = {} };
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _inside { 0 };
};
} // namespace

TEST_CASE("Two nodes compiling at once, each on its own claimed root, keep their own objects", "[node][scratch-claim]")
{
    // #279's acceptance criterion at unit level, wired the way `main` wires it: a
    // claim per node, and the job runner handed the root that claim covers.
    //
    // The two runners are two node PROCESSES: each has its own job counter starting
    // at 1, so both derive `job-1` beneath their own root. Before the claim those
    // roots were the same directory and this collided every time -- deterministically,
    // because there is no lost update to lose. It is the claim, not the numbering,
    // that separates them.
    ScratchDirectory base { "fc-scratchclaim-compile" };

    auto const claimantA = MakeLockFileScratchClaimant();
    auto const claimantB = MakeLockFileScratchClaimant();
    auto rootA = claimantA->Claim(base.Path(), DefaultMaxScratchRoots);
    auto rootB = claimantB->Claim(base.Path(), DefaultMaxScratchRoots);
    REQUIRE(rootA.has_value());
    REQUIRE(rootB.has_value());
    REQUIRE((*rootA)->Root() != (*rootB)->Root());

    OverlappingEchoRunner runner;
    FastCache::Cc::CompileJobRunner first {
        runner, (*rootA)->Root(), { { "gcc-13", "/opt/real/g++" } }, FastCache::Cc::ToolchainSurvey::Completed()
    };
    FastCache::Cc::CompileJobRunner second {
        runner, (*rootB)->Root(), { { "gcc-13", "/opt/real/g++" } }, FastCache::Cc::ToolchainSurvey::Completed()
    };
    std::array<FastCache::Cc::CompileJobRunner*, OverlappingEchoRunner::Overlap> const runners { &first, &second };

    // Same source NAME, different source TEXT: two machines compiling their own
    // `main.cpp` against one host.
    std::array<std::string, OverlappingEchoRunner::Overlap> const texts { "int a = 1;", "long b = 2;" };

    auto const asText = [](std::vector<std::byte> const& bytes) {
        std::string out;
        out.reserve(bytes.size());
        for (auto const byte: bytes)
            out.push_back(static_cast<char>(byte));
        return out;
    };

    std::array<std::expected<FastCache::Cc::CompileOutcome, FastCache::Cc::JobError>, OverlappingEchoRunner::Overlap>
        results {};
    std::latch start { static_cast<std::ptrdiff_t>(OverlappingEchoRunner::Overlap) };
    {
        std::vector<std::jthread> threads;
        for (auto const index: std::views::iota(std::size_t { 0 }, OverlappingEchoRunner::Overlap))
            threads.emplace_back([&, index] {
                auto job = FastCache::Cc::CompileJob {
                    .fingerprint = "gcc-13", .args = { "-O2" }, .preprocessed = texts.at(index), .sourceName = "a.cpp"
                };
                start.arrive_and_wait();
                // No Catch2 macro on this thread: the assertion macros are not
                // thread-safe, so the answer is carried back and checked below.
                results.at(index) = runners.at(index)->Run(job);
            });
    } // joined

    for (auto const index: std::views::iota(std::size_t { 0 }, OverlappingEchoRunner::Overlap))
    {
        INFO("node " << index);
        REQUIRE(results.at(index).has_value());
        REQUIRE(asText(results.at(index)->object) == texts.at(index));
    }
}
