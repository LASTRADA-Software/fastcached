// SPDX-License-Identifier: Apache-2.0
//
// What a re-survey owes the scheduler when a toolchain goes away
// ([#573](https://github.com/LASTRADA-Software/fastcached/issues/573)).
//
// `AdoptRegistrars` exists as a free function because clang-tidy refused it as a
// lambda inside `WorkerBody` -- cognitive complexity 66 against a threshold of 60 --
// and the analyser was right for a reason beyond arithmetic: `main.cpp` is in no test
// target (#909), so the rule *replacing the served set retires what left it* could
// only be checked by reading, at two call sites that must not diverge. This file is
// what the extraction bought.
#include "NodeAnnounce.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include <tests/ScriptedSocket.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{
namespace Wire = FastCache::CompileCacheWire;

/// A registrar for @p fingerprint, optionally already accepted by a scheduler.
///
/// The id is what a withdrawal NAMES, so whether it is set is the whole of the
/// second clause under test rather than incidental setup.
/// @param notice Where an unchecked credential would be reported.
/// @param fingerprint The toolchain it announces.
/// @return The registrar, never registered.
[[nodiscard]] Cc::WorkerRegistrar Registrar(Cc::CredentialNotice& notice, std::string fingerprint)
{
    return Cc::WorkerRegistrar { notice, std::move(fingerprint), "10.0.0.2:6677",
                                 1U,     Wire::CodecList {},     Wire::CapacityFields {} };
}

/// The fingerprints of a registrar list, in order, so a case can assert WHICH
/// survived rather than how many.
/// @param registrars The list to read.
/// @return One name per entry.
[[nodiscard]] std::vector<std::string> NamesOf(std::vector<Cc::WorkerRegistrar> const& registrars)
{
    std::vector<std::string> names;
    names.reserve(registrars.size());
    for (auto const& registrar: registrars)
        names.push_back(registrar.Fingerprint());
    return names;
}

/// A framed REGISTER reply accepting a worker under @p workerId.
/// @param workerId The id to assign.
/// @return The reply bytes.
[[nodiscard]] std::vector<std::byte> RegisterOk(std::string_view workerId)
{
    auto const payload =
        Wire::EncodeRegisterReply({ .workerId = std::string { workerId }, .clusterId = "fleet-a", .epoch = 1 });
    return Wire::EncodeReply(Wire::Status::Ok, payload);
}

/// A served set with the given fingerprints, values unused by the rule under test.
/// @param fingerprints What this node serves now.
/// @return Something answering `contains`.
[[nodiscard]] std::map<std::string, int> Served(std::vector<std::string> const& fingerprints)
{
    std::map<std::string, int> served;
    for (auto const& fingerprint: fingerprints)
        served.emplace(fingerprint, 0);
    return served;
}
} // namespace

TEST_CASE("Adopting a served set retires only the registrations that left it", "[node][announce]")
{
    // The registrars a re-survey drops carry the scheduler-issued `WorkerId` a
    // withdrawal names, and rebuilding the list destroys them. They are moved aside
    // instead.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));
    current.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "gcc-14"));
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), Served({ "gcc-14", "clang-20" }), current, withdrawals);

    // Nothing is retired, because neither registrar was ever accepted -- an empty
    // `WorkerId()` means there is no id to name it with and nothing on the other end.
    // This is the clause that makes the case below say something: without it, a
    // version that retired every departing registrar would pass on the names alone.
    CHECK(withdrawals.empty());

    // Replaced rather than merged, which is the same rule `ReplaceToolchains` follows
    // one line above it in production.
    CHECK(NamesOf(current) == std::vector<std::string> { "gcc-14", "clang-20" });
}

TEST_CASE("Adopting a served set retires a registration the scheduler accepted", "[node][announce]")
{
    // The positive direction, and the one the three cases around it cannot reach: they
    // all assert `withdrawals.empty()`, so on their own they pass equally well against
    // a function that retires NOTHING -- which is the version this whole change exists
    // to replace. A registrar only carries the `WorkerId` a withdrawal names once a
    // scheduler has accepted it, so the case has to register one for real.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));
    current.push_back(Registrar(notice, "clang-20"));

    Testing::ScriptedSocket scheduler { Testing::Replies({
        RegisterOk("w-gcc"),
        RegisterOk("w-clang"),
    }) };
    REQUIRE(current[0].Register(scheduler).has_value());
    REQUIRE(current[1].Register(scheduler).has_value());
    REQUIRE(current[0].WorkerId() == "w-gcc");

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), Served({ "clang-20" }), current, withdrawals);

    // Exactly the one that left, carrying the id the scheduler issued -- which is the
    // only thing `Op::Withdraw` can name, and the thing rebuilding the list destroys.
    REQUIRE(withdrawals.size() == 1);
    CHECK(withdrawals.front().Fingerprint() == "gcc-13");
    CHECK(withdrawals.front().WorkerId() == "w-gcc");

    // And the sibling is untouched: still registered, still holding its own id.
    CHECK(NamesOf(current) == std::vector<std::string> { "clang-20" });
}

TEST_CASE("Adopting a served set leaves the toolchains it still carries alone", "[node][announce]")
{
    // The control, and the direction that is expensive to get wrong: a node serving
    // several toolchains re-surveys routinely, and retiring a registration it still
    // serves would take that machine out of the fleet for a fingerprint it can
    // honour -- silently, since the entry simply stops being heartbeated.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> rebuilt;
    rebuilt.push_back(Registrar(notice, "clang-20"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars(std::move(rebuilt), Served({ "clang-20" }), current, withdrawals);

    CHECK(withdrawals.empty());
    CHECK(NamesOf(current) == std::vector<std::string> { "clang-20" });
}

TEST_CASE("Adopting an empty served set retires nothing that was never registered", "[node][announce]")
{
    // A machine that loses every toolchain keeps running and keeps saying nothing,
    // rather than exiting -- the compiler may come back with the next package. What
    // it must not do is invent withdrawals for entries no scheduler ever accepted.
    auto notice = Cc::CredentialNotice::Silent();

    std::vector<Cc::WorkerRegistrar> current;
    current.push_back(Registrar(notice, "gcc-13"));

    std::vector<Cc::WorkerRegistrar> withdrawals;
    AdoptRegistrars({}, Served({}), current, withdrawals);

    CHECK(current.empty());
    CHECK(withdrawals.empty());
}
