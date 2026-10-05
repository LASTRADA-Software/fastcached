// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Firewall.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FirewallFakes.hpp>
#include <tests/Unwrap.hpp>

using FastCache::FirewallLocalPort;
using FastCache::FirewallPortKind;
using FastCache::FirewallProtocol;
using FastCache::FirewallRule;
using FastCache::Testing::RecordingFirewall;
using FastCache::Testing::Unwrap;

namespace
{

/// An absolute path on this host: a rule's program must be one.
#if defined(_WIN32)
constexpr auto ProbeProgram = std::string_view { "C:/Program Files/fastcached/bin/probe.exe" };
#else
constexpr auto ProbeProgram = std::string_view { "/opt/fastcached/bin/probe" };
#endif

/// A rule of @p group on TCP @p port, as a planner would build it.
[[nodiscard]] FirewallRule RuleOn(std::string const& group, std::uint16_t port)
{
    auto const localPort = FirewallLocalPort { .kind = FirewallPortKind::Fixed, .number = port };
    return FirewallRule { .name = FastCache::FirewallRuleName("Probe", "node", FirewallProtocol::Tcp, localPort),
                          .group = group,
                          .program = ProbeProgram,
                          .serviceName = "Probe",
                          .protocol = FirewallProtocol::Tcp,
                          .localPort = localPort,
                          .remoteAddresses = {},
                          .bindHost = "0.0.0.0" };
}

/// The variable that opts a run into reading the machine's real firewall policy.
constexpr auto LiveReadVariable = std::string_view { "FASTCACHED_FIREWALL_LIVE_READ" };

} // namespace

TEST_CASE("A firewall rule and its group are named after the service and the surface", "[platform][firewall]")
{
    CHECK(FastCache::FirewallGroupFor("FastCacheCompileNode") == "fastcached: FastCacheCompileNode");
    CHECK(FastCache::FirewallRuleName("FastCacheCompileNode",
                                      "discovery-beacon",
                                      FirewallProtocol::Udp,
                                      FirewallLocalPort { .kind = FirewallPortKind::Fixed, .number = 6681 })
          == "FastCacheCompileNode discovery-beacon udp/6681");
    CHECK(FastCache::FirewallProtocolRowOf(FirewallProtocol::Tcp).ianaNumber == 6);
    CHECK(FastCache::FirewallProtocolRowOf(FirewallProtocol::Udp).ianaNumber == 17);
}

TEST_CASE("A rule admitting any local port says so in its name and gives the firewall its every-port spelling",
          "[platform][firewall]")
{
    // "any" in the name rather than a 0: a rule list reading `udp/0` looks like a port that
    // admits nothing, which is the opposite of what the rule does.
    auto const any = FirewallLocalPort { .kind = FirewallPortKind::Any, .number = 0 };
    CHECK(FastCache::FirewallRuleName("FastCacheCompileNode", "discovery-reply", FirewallProtocol::Udp, any)
          == "FastCacheCompileNode discovery-reply udp/any");

    // What `INetFwRule::put_LocalPorts` is given: `*` is the Windows Firewall's every-port value
    // (a new UDP rule reads it back before any port is set, and "any" is refused as invalid
    // data), while a fixed port is its number.
    CHECK(FastCache::FirewallPortKindRowOf(FirewallPortKind::Any).firewallText(0) == "*");
    CHECK(FastCache::FirewallPortKindRowOf(FirewallPortKind::Fixed).firewallText(6681) == "6681");
}

TEST_CASE("A firewall rule name's comparison key folds ASCII case and nothing else", "[platform][firewall]")
{
    CHECK(FastCache::FirewallNameKey("Probe NODE tcp/6674") == "probe node tcp/6674");
    CHECK(FastCache::FirewallNameKey("probe node tcp/6674") == "probe node tcp/6674");
    // Bytes outside ASCII pass through untouched: folding them is the Windows comparison's job.
    CHECK(FastCache::FirewallNameKey("\xC3\x9C-Regel") == "\xC3\x9C-regel");
}

TEST_CASE("A firewall scope is an IPv4 or IPv6 address or prefix and a refusal names what is wrong with it",
          "[platform][firewall]")
{
    // One row per input, so the grammar is read as a table. `mentions` is the phrase the refusal
    // must contain -- WHICH refusal, not merely that one happened.
    struct ScopeCase
    {
        std::string_view text;
        bool accepted;
        std::string_view mentions;
    };
    constexpr auto Cases = std::to_array<ScopeCase>({
        { .text = "10.0.0.0/8", .accepted = true, .mentions = {} },
        { .text = "192.168.1.7", .accepted = true, .mentions = {} },
        { .text = "10.1.2.3/32", .accepted = true, .mentions = {} },
        { .text = "0.0.0.0/8", .accepted = true, .mentions = {} },
        { .text = "fd00::/8", .accepted = true, .mentions = {} },
        { .text = "2001:db8::1", .accepted = true, .mentions = {} },
        { .text = "::1/128", .accepted = true, .mentions = {} },
        { .text = "::ffff:192.0.2.1", .accepted = true, .mentions = {} },
        { .text = "1:2:3:4::/64", .accepted = true, .mentions = {} },
        // /0 is every address. Allowing any address is spelled by OMITTING the flag, so a /0 is an
        // operator who meant something narrower and typed the wrong length (--fleet-open's rule:
        // "everyone" is never an accident).
        { .text = "0.0.0.0/0", .accepted = false, .mentions = "omit --firewall-allow" },
        { .text = "::/0", .accepted = false, .mentions = "omit --firewall-allow" },
        { .text = "10.0.0.0/00", .accepted = false, .mentions = "omit --firewall-allow" },
        { .text = "10.0.0.0/33", .accepted = false, .mentions = "1 to 32" },
        { .text = "fd00::/129", .accepted = false, .mentions = "1 to 128" },
        { .text = "10.0.0.0/", .accepted = false, .mentions = "1 to 32" },
        { .text = "10.0.0.0/x", .accepted = false, .mentions = "1 to 32" },
        // A prefix with host bits set names one network while spelling another address: which did
        // the operator mean? Both readings are offered, the network in canonical text.
        { .text = "10.0.0.5/24", .accepted = false, .mentions = "did you mean 10.0.0.0/24 or 10.0.0.5/32" },
        { .text = "192.168.1.130/25", .accepted = false, .mentions = "did you mean 192.168.1.128/25 or 192.168.1.130/32" },
        { .text = "1:2:3:4:5:6:7:8/64", .accepted = false, .mentions = "did you mean 1:2:3:4::/64 or 1:2:3:4:5:6:7:8/128" },
        { .text = "2001:db8:0:0:1::1/48",
          .accepted = false,
          .mentions = "did you mean 2001:db8::/48 or 2001:db8:0:0:1::1/128" },
        { .text = "1:0:0:2:0:0:3:5/127",
          .accepted = false,
          .mentions = "did you mean 1::2:0:0:3:4/127 or 1:0:0:2:0:0:3:5/128" },
        { .text = "::ffff:192.0.2.1/96",
          .accepted = false,
          .mentions = "did you mean ::ffff:0:0/96 or ::ffff:192.0.2.1/128" },
        { .text = "300.1.1.1", .accepted = false, .mentions = "IPv4 address is four numbers" },
        { .text = "10.0.0", .accepted = false, .mentions = "IPv4 address is four numbers" },
        { .text = "1.2.3.4.5", .accepted = false, .mentions = "IPv4 address is four numbers" },
        { .text = "office.example", .accepted = false, .mentions = "IPv4 address is four numbers" },
        // Some address readers take a leading zero as octal, so `010` would be 8 to them and 10 to
        // the operator: a rule admitting somebody else.
        { .text = "010.0.0.1", .accepted = false, .mentions = "none with a leading zero" },
        { .text = "::ffff:10.0.0.01", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "1::2::3", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "1:2:3:4:5:6:7:8:9", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "1:2:3:4:5:6:7::8", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "12345::", .accepted = false, .mentions = "IPv6 address is" },
        { .text = ":::", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "fe80::1%eth0", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "::ffff:300.0.0.1", .accepted = false, .mentions = "IPv6 address is" },
        { .text = "", .accepted = false, .mentions = "empty" },
    });

    for (auto const& row: Cases)
    {
        INFO("scope: '" << row.text << "'");
        auto const parsed = FastCache::ParseFirewallScope(row.text);
        CHECK(parsed.has_value() == row.accepted);
        if (row.accepted && parsed.has_value())
            CHECK(Unwrap(parsed) == row.text);
        if (!row.accepted && !parsed.has_value())
        {
            // The parser cannot know which flag reached it, so it names none; `ApplyOneOption`
            // stamps the row's own spelling.
            CHECK(parsed.error().field.empty());
            CHECK(parsed.error().source.empty());
            CHECK(parsed.error().context.contains(row.mentions));
        }
    }
}

TEST_CASE("Applying a service's rules replaces its group and leaves every other group alone", "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 1111), RuleOn("fastcached: Other", 2222) };

    auto const wanted = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674), RuleOn("fastcached: Probe", 6680) };
    auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
    REQUIRE(outcome.has_value());
    CHECK(outcome->removed == 1);
    CHECK(outcome->added == 2);
    CHECK(firewall.InGroup("fastcached: Probe") == wanted);
    CHECK(firewall.InGroup("fastcached: Other").size() == 1);

    // A repair or an upgrade runs the install again. Replaced, never appended.
    REQUIRE(FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted).has_value());
    CHECK(firewall.InGroup("fastcached: Probe") == wanted);
}

TEST_CASE("A rule filed under another group is refused before anything is touched", "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 1111) };
    auto const stray = std::vector<FirewallRule> { RuleOn("fastcached: Other", 6674) };

    auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", stray);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().contains("fastcached: Other"));
    CHECK(firewall.adds == 0);
    CHECK(firewall.InGroup("fastcached: Probe").size() == 1);
}

TEST_CASE("A rule the firewall would refuse is refused before anything is touched", "[platform][firewall]")
{
    // The new rules share the old ones' group and names, so they cannot be added before the old
    // are removed. Every rule is therefore checked first: a refusal after the removal would leave
    // the service with fewer rules than it had. One row per fault, each spoiling the SECOND rule
    // so a check that looked only at the first would let it through.
    struct FaultCase
    {
        std::string_view fault;
        void (*spoil)(std::vector<FirewallRule>&);
        std::string_view mentions;
    };
    constexpr auto Cases = std::to_array<FaultCase>({
        { .fault = "no name",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].name.clear(); },
          .mentions = "has no name" },
        { .fault = "no service",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].serviceName.clear(); },
          .mentions = "names no service" },
        { .fault = "relative program",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].program = "bin/probe.exe"; },
          .mentions = "is not an absolute path" },
        { .fault = "port 0",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].localPort.number = 0; },
          .mentions = "its port is 0" },
        // Any-port with a number is two answers to one question; the firewall is not asked to pick.
        { .fault = "any port naming a number",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].localPort.kind = FirewallPortKind::Any; },
          .mentions = "it admits any local port and also names port 6680" },
        // Any-port is UDP's alone; every TCP surface is Fixed, and the batch here is TCP.
        { .fault = "any port on tcp",
          .spoil =
              [](std::vector<FirewallRule>& rules) {
                  rules[1].localPort = FirewallLocalPort { .kind = FirewallPortKind::Any, .number = 0 };
              },
          .mentions = "it admits any local port on tcp" },
        { .fault = "bad scope",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].remoteAddresses = { "10.0.0.0/8", "10.0.0.5/24" }; },
          .mentions = "did you mean 10.0.0.0/24 or 10.0.0.5/32" },
        { .fault = "duplicate name",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].name = rules[0].name; },
          .mentions = "rules 'Probe node tcp/6674' and 'Probe node tcp/6674' have the same name" },
        // The firewall may not tell names apart by case, so two that differ only in case collide.
        { .fault = "duplicate name differing only in case",
          .spoil = [](std::vector<FirewallRule>& rules) { rules[1].name = "PROBE NODE TCP/6674"; },
          .mentions = "have the same name ignoring case" },
    });

    auto const batch = [] {
        auto rules = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674), RuleOn("fastcached: Probe", 6680) };
        rules[1].remoteAddresses = { "10.0.0.0/8", "fd00::/8" };
        return rules;
    };

    // The control: the unspoiled batch applies, so every refusal below is the spoiled field's.
    RecordingFirewall control;
    REQUIRE(FastCache::ApplyServiceFirewall(control, "fastcached: Probe", batch()).has_value());
    CHECK(control.adds == 2);

    for (auto const& row: Cases)
    {
        INFO("fault: " << row.fault);
        RecordingFirewall firewall;
        firewall.rules = { RuleOn("fastcached: Probe", 1111) };
        auto wanted = batch();
        row.spoil(wanted);

        // CHECK rather than REQUIRE, so a check that lets one fault through names every row it misses.
        auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
        CHECK_FALSE(outcome.has_value());
        if (outcome.has_value())
            continue;
        CHECK(outcome.error().contains(row.mentions));
        CHECK(outcome.error().contains("the firewall was not changed"));
        CHECK(firewall.adds == 0);
        CHECK(firewall.InGroup("fastcached: Probe") == std::vector<FirewallRule> { RuleOn("fastcached: Probe", 1111) });
    }
}

TEST_CASE("A refused add says how many rules landed before it and how many were removed", "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 1111) };
    firewall.refuseAddAt = 1;
    auto const wanted = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674), RuleOn("fastcached: Probe", 6680) };

    auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().contains("1 of 2"));
    CHECK(outcome.error().contains("scripted refusal"));
    CHECK(outcome.error().contains("the 1 rule(s) the group held before had already been removed"));
}

TEST_CASE("Removing a service's rules removes its group only", "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 1), RuleOn("fastcached: Probe", 2), RuleOn("fastcached: Other", 3) };
    auto const removed = FastCache::RemoveServiceFirewall(firewall, "fastcached: Probe");
    REQUIRE(removed.has_value());
    CHECK(Unwrap(removed) == 2);
    CHECK(firewall.rules.size() == 1);
}

TEST_CASE("A firewall that cannot empty the group refuses the apply and the removal before adding anything",
          "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 1111) };
    firewall.refuseRemove = "scripted removal refusal";
    auto const wanted = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674) };

    auto const applied = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().contains("scripted removal refusal"));
    CHECK(firewall.adds == 0);

    auto const removed = FastCache::RemoveServiceFirewall(firewall, "fastcached: Probe");
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error().contains("scripted removal refusal"));
    CHECK(firewall.InGroup("fastcached: Probe") == std::vector<FirewallRule> { RuleOn("fastcached: Probe", 1111) });
}

TEST_CASE("A name a rule outside the group carries refuses the apply before anything changes", "[platform][firewall]")
{
    // The firewall removes by NAME. A planned rule named like an operator's own rule would make the
    // NEXT removal take either, and a group rule named like one makes THIS removal do so: both
    // refuse, in the removal's own walk, before anything is removed or added.
    auto const wanted = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674), RuleOn("fastcached: Probe", 6680) };
    auto theirs = RuleOn("Operator rules", 6680);

    SECTION("a planned name")
    {
        RecordingFirewall firewall;
        firewall.rules = { RuleOn("fastcached: Probe", 1111), theirs };
        auto const before = firewall.rules;

        auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().contains("our rule 'Probe node tcp/6680' (group 'fastcached: Probe')"));
        CHECK(outcome.error().contains("the rule 'Probe node tcp/6680' outside it"));
        CHECK(firewall.adds == 0);
        CHECK(firewall.rules == before);
    }

    SECTION("a planned name differing only in case")
    {
        RecordingFirewall firewall;
        theirs.name = "PROBE NODE TCP/6680";
        firewall.rules = { RuleOn("fastcached: Probe", 1111), theirs };
        auto const before = firewall.rules;

        auto const outcome = FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted);
        REQUIRE_FALSE(outcome.has_value());
        // Both spellings, each in its place: ours as this project spells it, theirs as the other
        // rule does -- the sentence every implementation builds through FirewallNameCollision.
        CHECK(outcome.error().contains("our rule 'Probe node tcp/6680' (group 'fastcached: Probe')"));
        CHECK(outcome.error().contains("the rule 'PROBE NODE TCP/6680' outside it"));
        CHECK(firewall.adds == 0);
        CHECK(firewall.rules == before);
    }

    SECTION("a name the group already holds")
    {
        RecordingFirewall firewall;
        firewall.rules = { RuleOn("fastcached: Probe", 6680), theirs };
        auto const before = firewall.rules;

        auto const removed = FastCache::RemoveServiceFirewall(firewall, "fastcached: Probe");
        REQUIRE_FALSE(removed.has_value());
        CHECK(removed.error().contains("our rule 'Probe node tcp/6680' (group 'fastcached: Probe')"));
        CHECK(firewall.rules == before);
    }

    SECTION("the control: no shared name, and both go through")
    {
        RecordingFirewall firewall;
        theirs.name = "Operator's own rule";
        firewall.rules = { RuleOn("fastcached: Probe", 6680), theirs };

        REQUIRE(FastCache::ApplyServiceFirewall(firewall, "fastcached: Probe", wanted).has_value());
        CHECK(firewall.adds == 2);
        REQUIRE(FastCache::RemoveServiceFirewall(firewall, "fastcached: Probe").has_value());
        CHECK(firewall.rules == std::vector<FirewallRule> { theirs });
    }
}

TEST_CASE("Only Windows has a firewall this project manages", "[platform][firewall]")
{
#if defined(_WIN32)
    CHECK(FastCache::MakeSystemFirewall() != nullptr);
#else
    CHECK(FastCache::MakeSystemFirewall() == nullptr);
#endif
}

TEST_CASE("The system firewall's rules can be listed when a run opts in", "[platform][firewall]")
{
    // The only case that reaches the machine's REAL firewall, and it only READS: every rule is
    // enumerated and compared against a group no service is registered under, so the removal
    // pass has nothing to remove. Opt-in all the same, because the policy is machine-wide state
    // and a unit run must not depend on it; the fakes carry every decision above.
    if (FastCache::ReadEnvironmentVariable(LiveReadVariable) != "1")
        SKIP("reads the machine's firewall policy; set FASTCACHED_FIREWALL_LIVE_READ=1 to run it");
    auto const firewall = FastCache::MakeSystemFirewall();
    if (firewall == nullptr)
        SKIP("this platform has no firewall this project manages");

    auto const removed = firewall->RemoveGroup("fastcached: read-only probe, never a service's group", {});
    INFO("refusal: " << (removed.has_value() ? std::string {} : removed.error()));
    REQUIRE(removed.has_value());
    CHECK(Unwrap(removed) == 0);

    // The listing walks the same rules the same way, and changes nothing by construction.
    auto const listed = firewall->NamesInGroup("fastcached: read-only probe, never a service's group");
    INFO("refusal: " << (listed.has_value() ? std::string {} : listed.error()));
    REQUIRE(listed.has_value());
    CHECK(Unwrap(listed).empty());
}

TEST_CASE("An install says which rules it opened, and a loopback-only one clears what an earlier install opened",
          "[platform][firewall]")
{
    RecordingFirewall firewall;
    auto const opened = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674) };

    auto const first = FastCache::RegistrationFirewallNote(&firewall, "Probe", opened);
    CHECK(first.contains("allowed inbound Probe node tcp/6674"));

    // The same service re-registered with every surface on loopback: nothing faces the network,
    // so no rule is needed and the ones the first registration opened are gone.
    auto const second = FastCache::RegistrationFirewallNote(&firewall, "Probe", {});
    CHECK(second.contains("no rule needed"));
    CHECK(firewall.InGroup("fastcached: Probe").empty());

    firewall.rules = opened;
    CHECK(FastCache::RemovalFirewallNote(&firewall, "Probe").contains("removed 1 rule(s)"));
    CHECK(firewall.rules.empty());
}

TEST_CASE("Where no firewall is managed, an install says so only when something faces the network", "[platform][firewall]")
{
    auto const opened = std::vector<FirewallRule> { RuleOn("fastcached: Probe", 6674) };
    CHECK(FastCache::RegistrationFirewallNote(nullptr, "Probe", opened).contains("not managed"));
    CHECK(FastCache::RegistrationFirewallNote(nullptr, "Probe", {}).empty());
    CHECK(FastCache::RemovalFirewallNote(nullptr, "Probe").empty());
}

TEST_CASE("A removal the firewall refuses names every rule it left in place and how to remove them", "[platform][firewall]")
{
    // The operator's own rule carries one of our names, so the firewall refuses to remove by name.
    auto theirs = RuleOn("Operator rules", 6680);
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 6674), RuleOn("fastcached: Probe", 6680), theirs };

    auto const note = FastCache::RemovalFirewallNote(&firewall, "Probe");
    INFO(note);
    CHECK(note.contains("the firewall rules of 'fastcached: Probe' were not removed"));
    // WHICH refusal: the shared name, in the one sentence both implementations give.
    CHECK(note.contains("have the same name ignoring case"));
    CHECK(note.contains("still in place: Probe node tcp/6674, Probe node tcp/6680"));
    CHECK(note.contains("Remove-NetFirewallRule -Group 'fastcached: Probe'"));
    CHECK(note.contains("run --uninstall-service again"));
    CHECK(firewall.InGroup("fastcached: Probe").size() == 2);
    CHECK(std::ranges::contains(firewall.rules, theirs));

    // Where the firewall will not even list them, the note says so rather than naming none.
    firewall.refuseList = "listing refused";
    auto const unlisted = FastCache::RemovalFirewallNote(&firewall, "Probe");
    INFO(unlisted);
    CHECK(unlisted.contains("could not be listed (listing refused)"));
    CHECK(unlisted.contains("every one may still be in place"));
    CHECK(unlisted.contains("Remove-NetFirewallRule -Group 'fastcached: Probe'"));
    CHECK_FALSE(unlisted.contains("still in place:"));
}

TEST_CASE("A rule opened for the name localhost says why, and one bound to an address says nothing more",
          "[platform][firewall]")
{
    auto named = RuleOn("fastcached: Probe", 6674);
    named.bindHost = "LocalHost";
    auto const addressed = RuleOn("fastcached: Probe", 6680);

    RecordingFirewall firewall;
    auto const note =
        FastCache::RegistrationFirewallNote(&firewall, "Probe", std::vector<FirewallRule> { named, addressed });
    INFO(note);
    CHECK(note.contains("note: Probe node tcp/6674 is opened for a surface bound to 'LocalHost'"));
    CHECK_FALSE(note.contains("Probe node tcp/6680 is opened for"));

    // The same line where no firewall is managed: the rule is still one the operator must write.
    auto const unmanaged = FastCache::RegistrationFirewallNote(nullptr, "Probe", std::vector<FirewallRule> { named });
    CHECK(unmanaged.contains("not managed"));
    CHECK(unmanaged.contains("is opened for a surface bound to 'LocalHost'"));
}

TEST_CASE("A removal that stops part-way names the rules that remain, and not the ones it took", "[platform][firewall]")
{
    RecordingFirewall firewall;
    firewall.rules = { RuleOn("fastcached: Probe", 6674),
                       RuleOn("fastcached: Probe", 6680),
                       RuleOn("fastcached: Probe", 6690) };
    firewall.refuseRemoveAt = 1;

    auto const note = FastCache::RemovalFirewallNote(&firewall, "Probe");
    INFO(note);
    CHECK(note.contains("scripted refusal after 1 of 3 removed"));
    CHECK(note.contains("still in place: Probe node tcp/6680, Probe node tcp/6690"));
    CHECK_FALSE(note.contains("tcp/6674"));
    CHECK(note.contains("Remove-NetFirewallRule -Group 'fastcached: Probe'"));
    CHECK(firewall.InGroup("fastcached: Probe").size() == 2);
}
