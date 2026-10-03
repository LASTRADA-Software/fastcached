// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostNaming.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <tests/HostNamingFakes.hpp>

using namespace FastCache;

TEST_CASE("A DNS suffix is what follows the host label and absent without a domain", "[platform][formation][naming]")
{
    CHECK(DnsSuffixOf("office-a.corp.example") == "corp.example");
    CHECK(DnsSuffixOf("office-a").empty());
    CHECK(DnsSuffixOf("office-a.").empty()); // a trailing root dot names no domain
    CHECK(DnsSuffixOf("office-a.corp.example.") == "corp.example");
    CHECK(DnsSuffixOf("").empty());
}

TEST_CASE("A canonical name is this machine's FQDN only when it names this machine", "[platform][formation][naming]")
{
    // The resolver's answer, when it extends this machine's own first label.
    CHECK(FullyQualifiedNameFrom("office-a", "office-a.corp.example") == "office-a.corp.example");
    CHECK(FullyQualifiedNameFrom("office-a", "OFFICE-A.corp.example.") == "OFFICE-A.corp.example");
    CHECK(FullyQualifiedNameFrom("office-a.corp.example", "office-a.corp.example") == "office-a.corp.example");
    CHECK(FullyQualifiedNameFrom("office-a", "office-a") == "office-a");

    // Anything else keeps the host name: an /etc/hosts that lists `localhost` first on the host's
    // own line would otherwise have every peer dial itself.
    CHECK(FullyQualifiedNameFrom("office-a", "localhost") == "office-a");
    CHECK(FullyQualifiedNameFrom("office-a", "localhost.localdomain") == "office-a");
    CHECK(FullyQualifiedNameFrom("office-ab", "office-a.corp.example") == "office-ab"); // a label, not a prefix
    CHECK(FullyQualifiedNameFrom("office-a", "office-ab.corp.example") == "office-a");
    CHECK(FullyQualifiedNameFrom("office-a", "") == "office-a");
    CHECK(FullyQualifiedNameFrom("office-a", ".") == "office-a");
    CHECK(FullyQualifiedNameFrom("", "office-a.corp.example").empty());
}

TEST_CASE("A placeholder domain is refused as this machine's name, by name", "[platform][formation][naming]")
{
    // `localdomain` is what an /etc/hosts line carries for a machine with no domain -- WSL answers
    // `DarkLeon.localdomain` on the machine this was written on. Only this machine resolves it, so
    // advertising it would be a confident wrong signal: every peer's dial fails with nothing at
    // either end saying why. The bare host name may or may not resolve elsewhere, which is the
    // vague right answer, and the declined name is kept so a log line can say what was refused.
    auto const wsl = JudgeHostNaming("DarkLeon.localdomain", "localdomain");
    CHECK(wsl.fqdn == "DarkLeon");
    CHECK(wsl.suffix.empty()); // and no SRV query against `_fastcache._tcp.localdomain`
    CHECK(wsl.declined == "DarkLeon.localdomain");

    // ASCII case and a root dot do not hide it, and `localhost` is the same placeholder.
    CHECK(JudgeHostNaming("office-a.LocalDomain.", "LocalDomain").fqdn == "office-a");
    CHECK(JudgeHostNaming("office-a.localhost", "localhost").declined == "office-a.localhost");

    // A real domain passes untouched, and so does a bare name -- neither declines anything.
    auto const real = JudgeHostNaming("office-a.corp.example", "corp.example");
    CHECK(real.fqdn == "office-a.corp.example");
    CHECK(real.suffix == "corp.example");
    CHECK(real.declined.empty());
    auto const bare = JudgeHostNaming("office-a", "");
    CHECK(bare.fqdn == "office-a");
    CHECK(bare.declined.empty());

    // A domain that merely ENDS in the word is a domain: only the whole suffix is a placeholder.
    CHECK(JudgeHostNaming("office-a.notlocaldomain.example", "notlocaldomain.example").declined.empty());

    // RHEL's unset default declines to exactly `localhost` -- a name every machine resolves to
    // itself, which the node's start then withholds (`NamesOnlyThisMachine`) rather than offers.
    auto const rhel = JudgeHostNaming("localhost.localdomain", "localdomain");
    CHECK(rhel.fqdn == "localhost");
    CHECK(rhel.declined == "localhost.localdomain");
    CHECK(NamesOnlyThisMachine(rhel.fqdn));
}

TEST_CASE("The scripted naming answers what it was built with", "[platform][formation][naming]")
{
    auto const naming = Testing::ScriptedHostNaming { "office-a.corp.example", "corp.example" };
    IHostNaming const& seam = naming;
    CHECK(seam.FullyQualifiedName() == "office-a.corp.example");
    CHECK(seam.PrimaryDnsSuffix() == "corp.example");
    CHECK(seam.DeclinedName().empty());
}

TEST_CASE("The system naming answers a non-empty name on this machine", "[platform][formation][naming][smoke]")
{
    // The real seam, once: an interface with only a fake behind it is an interface nobody has checked.
    auto const naming = MakeSystemHostNaming();
    CHECK_FALSE(naming->FullyQualifiedName().empty());
    CHECK(naming->FullyQualifiedName().starts_with(QueryHostFacts().hostName.substr(0, 1)));

    // The two answers are one reading: the suffix, when there is one, is the FQDN's own.
    auto const fqdn = naming->FullyQualifiedName();
    auto const suffix = naming->PrimaryDnsSuffix();
    INFO("fqdn=" << fqdn << " suffix=" << suffix);
    CHECK_FALSE(fqdn.ends_with('.'));
    if (!suffix.empty())
        CHECK(fqdn.ends_with("." + suffix));
}
