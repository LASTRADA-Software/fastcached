// SPDX-License-Identifier: Apache-2.0
#include "TicketCredentials.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// An outcome of @p kind carrying @p value, every other field at its default.
/// @param kind How the exchange ended.
/// @param value What it served.
/// @return The outcome.
[[nodiscard]] CacheOutcome OutcomeOf(CacheOutcomeKind kind, std::vector<std::byte> value = {})
{
    auto outcome = CacheOutcome {};
    outcome.kind = kind;
    outcome.value = std::move(value);
    return outcome;
}

/// A refusal with @p code.
/// @param code Why.
/// @return The outcome.
[[nodiscard]] CacheOutcome RefusalOf(Wire::ErrorCode code)
{
    auto outcome = OutcomeOf(CacheOutcomeKind::Rejected);
    outcome.code = code;
    return outcome;
}

/// Records every exchange and answers MINT-TICKET with a ticket naming its audience.
class RecordingExchange final: public IEndpointExchange
{
  public:
    struct Seen
    {
        std::string endpoint;
        std::uint8_t op;
        Credential credential;
    };

    /// How a MINT-TICKET is answered.
    enum class Mint : std::uint8_t
    {
        Ticket,
        Refused,
        Unreachable,
        Empty,
    };

    [[nodiscard]] CacheOutcome Exchange(std::string_view hostPort,
                                        std::vector<std::byte> frame,
                                        Credential const& credential,
                                        ExchangeBudget /*budget*/) override
    {
        auto const header = Wire::DecodeRequestHeader(frame);
        REQUIRE(header.has_value());
        auto const op = Unwrap(header).opRaw;
        seen.push_back(Seen { .endpoint = std::string { hostPort }, .op = op, .credential = credential });
        if (op != static_cast<std::uint8_t>(Wire::Op::MintTicket))
            return refuseCommands ? RefusalOf(Wire::ErrorCode::NotAMember) : OutcomeOf(CacheOutcomeKind::Hit);
        switch (mint)
        {
            case Mint::Ticket:
                break;
            case Mint::Refused:
                return RefusalOf(Wire::ErrorCode::NotAMember);
            case Mint::Unreachable:
                return OutcomeOf(CacheOutcomeKind::Transport);
            case Mint::Empty:
                return OutcomeOf(CacheOutcomeKind::Hit);
        }
        auto const audience = Wire::DecodeMintTicketPayload(std::span { frame }.subspan(Wire::RequestHeaderSize));
        REQUIRE(audience.has_value());
        auto const ticket = "ticket-for:" + Unwrap(audience);
        auto const bytes = Wire::AsBytes(ticket);
        return OutcomeOf(CacheOutcomeKind::Hit, { bytes.begin(), bytes.end() });
    }

    std::vector<Seen> seen;
    Mint mint { Mint::Ticket };
    bool refuseCommands { false }; ///< Answer every command but a mint `NotAMember`.
};

/// A launcher's credentials over a recording exchange, minting from this machine's node.
struct Minting
{
    explicit Minting(std::optional<std::string> mintFrom = std::string { "127.0.0.1:6674" },
                     Credential password = {},
                     std::string passwordFor = {}):
        tickets { raw,
                  std::move(mintFrom),
                  std::move(password),
                  std::move(passwordFor),
                  ExchangeBudget {},
                  [this](std::string_view line) { said.emplace_back(line); } }
    {
    }

    RecordingExchange raw;
    std::vector<std::string> said;
    TicketCredentials tickets;
};

} // namespace

TEST_CASE("Which credential an exchange presents is decided by where it goes", "[cc][ticket]")
{
    struct Row
    {
        char const* audience;
        char const* passwordFor;
        CredentialChoice expected;
    };
    auto const rows = std::to_array<Row>({
        { .audience = "office.corp:6674", .passwordFor = "", .expected = CredentialChoice::Ticket },
        { .audience = "10.0.0.7:6676", .passwordFor = "", .expected = CredentialChoice::Ticket },
        { .audience = "127.0.0.1:6674", .passwordFor = "", .expected = CredentialChoice::None },
        { .audience = "[::1]:6674", .passwordFor = "", .expected = CredentialChoice::None },
        // Every audience the MINTER refuses presents nothing, so no mint is asked for and refused:
        // the loopback name, in any case, and the wildcards.
        { .audience = "localhost:6674", .passwordFor = "", .expected = CredentialChoice::None },
        { .audience = "LOCALHOST:6674", .passwordFor = "", .expected = CredentialChoice::None },
        { .audience = "0.0.0.0:6674", .passwordFor = "", .expected = CredentialChoice::None },
        { .audience = "[::]:6674", .passwordFor = "", .expected = CredentialChoice::None },
        // And a name is not loopback however it begins.
        { .audience = "127.cache.example.com:6674", .passwordFor = "", .expected = CredentialChoice::Ticket },
        { .audience = "not an endpoint", .passwordFor = "", .expected = CredentialChoice::None },
        // The token is the cache's: the cache gets it, and every node still gets a ticket.
        { .audience = "cache.corp:6674", .passwordFor = "cache.corp:6674", .expected = CredentialChoice::Password },
        { .audience = "office.corp:6674", .passwordFor = "cache.corp:6674", .expected = CredentialChoice::Ticket },
        { .audience = "127.0.0.1:6674", .passwordFor = "127.0.0.1:6674", .expected = CredentialChoice::Password },
    });
    for (auto const& row: rows)
    {
        INFO(row.audience << " password for " << row.passwordFor);
        CHECK(ChooseCredential(row.audience, row.passwordFor) == row.expected);
    }
}

TEST_CASE("Every remote exchange gets a ticket of its own, minted for that audience on this machine", "[cc][ticket]")
{
    Minting fix;
    auto const lease = fix.tickets.For("office.corp:6674");
    auto const compile = fix.tickets.For("10.0.0.7:6676");
    auto const release = fix.tickets.For("office.corp:6674");

    REQUIRE(fix.raw.seen.size() == 3);
    for (auto const& seen: fix.raw.seen)
    {
        CHECK(seen.endpoint == "127.0.0.1:6674");
        CHECK(seen.op == static_cast<std::uint8_t>(Wire::Op::MintTicket));
        CHECK_FALSE(seen.credential.Configured()); // the mint itself is admitted as this machine
    }
    CHECK(lease.kind == Wire::AuthKind::MachineTicket);
    CHECK(lease.username.empty());
    CHECK(lease.secret.View() == "ticket-for:office.corp:6674");
    CHECK(compile.secret.View() == "ticket-for:10.0.0.7:6676");
    CHECK(release.secret.View() == "ticket-for:office.corp:6674"); // minted again: one ticket per exchange
    CHECK(fix.said.empty());
    CHECK_FALSE(fix.tickets.Present("office.corp:6674").missing.has_value());
}

TEST_CASE("A loopback exchange presents nothing and mints nothing", "[cc][ticket]")
{
    Minting fix;
    CHECK_FALSE(fix.tickets.For("127.0.0.1:6674").Configured());
    CHECK_FALSE(fix.tickets.For("[::1]:6675").Configured());
    CHECK_FALSE(fix.tickets.For("localhost:6674").Configured()); // FASTCACHE_ADDR=localhost:6674
    CHECK(fix.raw.seen.empty());
    CHECK(fix.said.empty());
}

TEST_CASE("The password goes to the endpoint it was configured for, and every other machine gets a ticket", "[cc][ticket]")
{
    Minting fix { std::string { "127.0.0.1:6674" },
                  Credential { .username = "bob", .secret = "s3cret" },
                  "cache.corp:6674" };

    auto const cache = fix.tickets.For("cache.corp:6674");
    CHECK(cache.kind == Wire::AuthKind::Password);
    CHECK(cache.username == "bob");
    CHECK(cache.secret.View() == "s3cret");
    CHECK(fix.raw.seen.empty()); // no mint for the cache

    auto const node = fix.tickets.For("office.corp:6674");
    CHECK(node.kind == Wire::AuthKind::MachineTicket);
    CHECK(node.secret.View() == "ticket-for:office.corp:6674");
}

TEST_CASE("A mint that fails leaves THAT exchange unauthenticated, names why, and says it once", "[cc][ticket]")
{
    // Never a silent fall to no ticket: each way a mint fails has its own fixed reason, carried with
    // the exchange it failed for and said once through the sink with the detail.
    struct Row
    {
        char const* what;
        RecordingExchange::Mint mint;
        std::optional<std::string> mintFrom;
        MintFailure expected;
    };
    auto const rows = std::to_array<Row>({
        { .what = "refused",
          .mint = RecordingExchange::Mint::Refused,
          .mintFrom = std::string { "127.0.0.1:6674" },
          .expected = MintFailure::Refused },
        { .what = "not answering",
          .mint = RecordingExchange::Mint::Unreachable,
          .mintFrom = std::string { "127.0.0.1:6674" },
          .expected = MintFailure::Unreachable },
        { .what = "answering with no ticket",
          .mint = RecordingExchange::Mint::Empty,
          .mintFrom = std::string { "127.0.0.1:6674" },
          .expected = MintFailure::NoTicket },
        { .what = "no source to ask",
          .mint = RecordingExchange::Mint::Ticket,
          .mintFrom = std::nullopt,
          .expected = MintFailure::NoSource },
    });
    for (auto const& row: rows)
    {
        INFO(row.what);
        Minting fix { row.mintFrom };
        fix.raw.mint = row.mint;
        for (auto const* audience: { "office.corp:6674", "10.0.0.7:6676" })
        {
            auto const presented = fix.tickets.Present(audience);
            CHECK_FALSE(presented.credential.Configured());
            CHECK(presented.missing == std::optional { row.expected });
        }
        REQUIRE(fix.said.size() == 1);
        CHECK(fix.said.front().contains(ReasonFor(row.expected)));
    }

    // Every failure is spelled, and a different one each.
    auto spelled = std::vector<std::string_view> {};
    for (auto const& row: MintFailureTable)
    {
        CHECK_FALSE(row.reason.empty());
        CHECK_FALSE(std::ranges::contains(spelled, row.reason));
        spelled.push_back(row.reason);
    }
}

TEST_CASE("A failed mint explains only its own exchange, and a later successful one explains nothing", "[cc][ticket]")
{
    // A failure remembered for the process blamed every later refusal on it: the FETCH's mint fails,
    // the LEASE's succeeds and is refused for its own reason, and the record said "no machine ticket".
    Minting fix;
    fix.raw.mint = RecordingExchange::Mint::Unreachable;
    CHECK(fix.tickets.Present("office.corp:6674").missing == std::optional { MintFailure::Unreachable });
    fix.raw.mint = RecordingExchange::Mint::Ticket;
    auto const later = fix.tickets.Present("office.corp:6674");
    CHECK(later.credential.Configured());
    CHECK_FALSE(later.missing.has_value());

    // And what the record says about each: the refusal the missing ticket caused names it, the
    // refusal answered to the valid ticket keeps its own words.
    auto const refused = RefusalOf(Wire::ErrorCode::NotAMember);
    CHECK(RecordedReason(refused, MintFailure::Unreachable) == ReasonFor(MintFailure::Unreachable));
    CHECK(RecordedReason(refused, later.missing) == DescribeOutcome(refused));
}

TEST_CASE("The recorded reason names a missing ticket only for a refusal a ticket would have answered",
          "[cc][ticket][stats]")
{
    // The ONE decision main.cpp asks at the dispatch, the FETCH and the STORE, as a pure function of
    // the outcome and what that exchange presented.
    SECTION("a cache exchange")
    {
        struct Row
        {
            char const* what;
            CacheOutcome outcome;
            std::optional<MintFailure> missing;
            bool namesTheMint;
        };
        auto const rows = std::to_array<Row>({
            { .what = "not a member, no ticket",
              .outcome = RefusalOf(Wire::ErrorCode::NotAMember),
              .missing = MintFailure::Refused,
              .namesTheMint = true },
            { .what = "ticket refused, no ticket",
              .outcome = RefusalOf(Wire::ErrorCode::TicketRefused),
              .missing = MintFailure::Unreachable,
              .namesTheMint = true },
            { .what = "not a member, a valid ticket",
              .outcome = RefusalOf(Wire::ErrorCode::NotAMember),
              .missing = std::nullopt,
              .namesTheMint = false },
            { .what = "a refusal no ticket answers",
              .outcome = RefusalOf(Wire::ErrorCode::NoWorker),
              .missing = MintFailure::Refused,
              .namesTheMint = false },
            { .what = "a transport failure",
              .outcome = OutcomeOf(CacheOutcomeKind::Transport),
              .missing = MintFailure::Refused,
              .namesTheMint = false },
            { .what = "a miss",
              .outcome = OutcomeOf(CacheOutcomeKind::Miss),
              .missing = MintFailure::Refused,
              .namesTheMint = false },
        });
        for (auto const& row: rows)
        {
            INFO(row.what);
            auto const expected =
                row.namesTheMint ? std::string { ReasonFor(Unwrap(row.missing)) } : DescribeOutcome(row.outcome);
            CHECK(RecordedReason(row.outcome, row.missing) == expected);
        }
    }

    SECTION("a dispatch")
    {
        // Read at the site the result NAMES, and nowhere else: the compile at the worker declined,
        // the release at the scheduler that followed it did not.
        auto const compile = ExchangeSite { .endpoint = "worker:6676", .opcode = std::to_underlying(Wire::Op::Compile) };
        auto const release = ExchangeSite { .endpoint = "sched:6675", .opcode = std::to_underlying(Wire::Op::Release) };
        auto withoutTicket = RefusalRecord {};
        withoutTicket.Note(compile, MintFailure::Unreachable);
        auto releaseWithout = RefusalRecord {};
        releaseWithout.Note(compile, std::nullopt);
        releaseWithout.Note(release, MintFailure::Unreachable);

        auto declined = DispatchResult {};
        declined.status = DispatchStatus::Declined;
        declined.decline = DeclineCause::NotPermitted;
        declined.declinedAt = compile;
        auto const plain = RecordingFor(declined.status, declined.decline);
        CHECK(RecordedReason(declined, withoutTicket).reason == ReasonFor(MintFailure::Unreachable));
        CHECK(RecordedReason(declined, withoutTicket).outcome == plain.outcome);
        CHECK(RecordedReason(declined, releaseWithout).reason == plain.reason);
        CHECK(RecordedReason(declined, RefusalRecord {}).reason == plain.reason);

        // A result naming no site blames no mint, whatever was filed.
        auto unnamed = declined;
        unnamed.declinedAt.reset();
        CHECK(RecordedReason(unnamed, withoutTicket).reason == plain.reason);

        declined.decline = DeclineCause::NoToolchain;
        CHECK(RecordedReason(declined, withoutTicket).reason == RecordingFor(declined.status, declined.decline).reason);

        auto unavailable = DispatchResult {};
        unavailable.status = DispatchStatus::Unavailable;
        CHECK(RecordedReason(unavailable, withoutTicket).reason
              == RecordingFor(unavailable.status, unavailable.decline).reason);
    }
}

TEST_CASE("The decorator files, per refused exchange site, whether that exchange presented a ticket", "[cc][ticket]")
{
    Minting fix;
    CredentialedExchange credentialed { fix.raw, fix.tickets };
    auto const fetchAt = [](std::string endpoint) {
        return ExchangeSite { .endpoint = std::move(endpoint), .opcode = std::to_underlying(Wire::Op::Fetch) };
    };
    auto const storeAt = [](std::string endpoint) {
        return ExchangeSite { .endpoint = std::move(endpoint), .opcode = std::to_underlying(Wire::Op::Store) };
    };
    fix.raw.refuseCommands = true;

    // Refused with no ticket: filed under that exchange's failed mint.
    fix.raw.mint = RecordingExchange::Mint::Unreachable;
    std::ignore = credentialed.Exchange("office.corp:6674", Wire::EncodeFetch("k"), Credential {}, ExchangeBudget {});
    CHECK(credentialed.Refusals().MissingAt(fetchAt("office.corp:6674")) == std::optional { MintFailure::Unreachable });

    // A later exchange at the same site that is SERVED leaves the filed refusal as it was.
    fix.raw.refuseCommands = false;
    fix.raw.mint = RecordingExchange::Mint::Ticket;
    std::ignore = credentialed.Exchange("office.corp:6674", Wire::EncodeFetch("k"), Credential {}, ExchangeBudget {});
    CHECK(credentialed.Refusals().MissingAt(fetchAt("office.corp:6674")) == std::optional { MintFailure::Unreachable });

    // A refusal answered to a VALID ticket at ANOTHER endpoint is filed there, and touches nothing
    // filed elsewhere.
    fix.raw.refuseCommands = true;
    std::ignore = credentialed.Exchange("10.0.0.7:6676", Wire::EncodeFetch("k"), Credential {}, ExchangeBudget {});
    CHECK_FALSE(credentialed.Refusals().MissingAt(fetchAt("10.0.0.7:6676")).has_value());
    CHECK(credentialed.Refusals().MissingAt(fetchAt("office.corp:6674")) == std::optional { MintFailure::Unreachable });

    // Nor does another VERB at the same endpoint: one address answers several exchanges.
    fix.raw.mint = RecordingExchange::Mint::Refused;
    std::ignore = credentialed.Exchange("10.0.0.7:6676",
                                        Wire::EncodeStore(Wire::StoreRequest {
                                            .key = "k", .prefetchGroup = {}, .srcRoot = {}, .buildTree = {}, .value = {} }),
                                        Credential {},
                                        ExchangeBudget {});
    CHECK(credentialed.Refusals().MissingAt(storeAt("10.0.0.7:6676")) == std::optional { MintFailure::Refused });
    CHECK_FALSE(credentialed.Refusals().MissingAt(fetchAt("10.0.0.7:6676")).has_value());

    // And a later refusal at the SAME site replaces what was filed there.
    fix.raw.mint = RecordingExchange::Mint::Ticket;
    std::ignore = credentialed.Exchange("office.corp:6674", Wire::EncodeFetch("k"), Credential {}, ExchangeBudget {});
    CHECK_FALSE(credentialed.Refusals().MissingAt(fetchAt("office.corp:6674")).has_value());
}

TEST_CASE("An exchange through the credentialed decorator presents a ticket for the endpoint it dials", "[cc][ticket]")
{
    Minting fix;
    CredentialedExchange credentialed { fix.raw, fix.tickets };
    std::ignore = credentialed.Exchange(
        "10.0.0.7:6676", Wire::EncodeFetch("k"), Credential { .username = {}, .secret = "caller's" }, ExchangeBudget {});
    REQUIRE(fix.raw.seen.size() == 2);
    CHECK(fix.raw.seen[0].op == static_cast<std::uint8_t>(Wire::Op::MintTicket));
    CHECK(fix.raw.seen[1].endpoint == "10.0.0.7:6676");
    CHECK(fix.raw.seen[1].credential.kind == Wire::AuthKind::MachineTicket);
    CHECK(fix.raw.seen[1].credential.secret.View() == "ticket-for:10.0.0.7:6676");
}

TEST_CASE("The ticket source is this machine's node, at the port the cache address names", "[cc][ticket]")
{
    CHECK(TicketSourceFor("127.0.0.1:6674") == std::optional<std::string> { "127.0.0.1:6674" });
    CHECK(TicketSourceFor("[::1]:7000") == std::optional<std::string> { "[::1]:7000" });
    // A remote cache keeps its PORT: never a default.
    CHECK(TicketSourceFor("cache.corp:6674") == std::optional<std::string> { "127.0.0.1:6674" });
    CHECK(TicketSourceFor("cache.corp:7000") == std::optional<std::string> { "127.0.0.1:7000" });
    CHECK(TicketSourceFor("[2001:db8::5]:7001") == std::optional<std::string> { "127.0.0.1:7001" });
    // A NAME is never the source, however it begins or whatever it resolves to: the source is a
    // loopback LITERAL at the cache address's port.
    CHECK(TicketSourceFor("127.cache.example.com:6674") == std::optional<std::string> { "127.0.0.1:6674" });
    CHECK(TicketSourceFor("localhost:7000") == std::optional<std::string> { "127.0.0.1:7000" });
    // A loopback LITERAL cache address is kept: another address in 127/8, or IPv6's.
    CHECK(TicketSourceFor("127.0.0.2:6674") == std::optional<std::string> { "127.0.0.2:6674" });
    // No endpoint, no port, no source -- and a mint then fails by that name.
    CHECK_FALSE(TicketSourceFor("").has_value());
    CHECK_FALSE(TicketSourceFor("not an endpoint").has_value());
}

TEST_CASE("Every mint goes to this machine's node over loopback, whatever the scheduler is", "[cc][ticket]")
{
    // The scheduler is where a LEASE goes, never where a ticket comes from: the node mints only for
    // its own machine. Each scheduler shape -- on this machine, elsewhere, not configured -- beside
    // each cache shape, and every mint dial lands on this machine at the cache address's port.
    auto const schedulers = std::to_array<std::string_view>({ "127.0.0.1:6675", "sched.corp:6675", "" });
    auto const caches = std::to_array<std::string_view>({ "127.0.0.1:6674", "cache.corp:7000" });
    for (auto const cacheAddr: caches)
        for (auto const scheduler: schedulers)
        {
            INFO("cache " << cacheAddr << ", scheduler " << scheduler);
            auto const source = TicketSourceFor(cacheAddr);
            REQUIRE(source.has_value());
            Minting fix { source };
            if (!scheduler.empty())
                std::ignore = fix.tickets.For(scheduler);
            std::ignore = fix.tickets.For("10.0.0.7:6676");
            std::ignore = fix.tickets.For(cacheAddr);

            REQUIRE_FALSE(fix.raw.seen.empty());
            auto const cachePort = ParseDialEndpoint(cacheAddr);
            REQUIRE(cachePort.has_value());
            for (auto const& seen: fix.raw.seen)
            {
                CHECK(seen.endpoint == Unwrap(source));
                auto const endpoint = ParseDialEndpoint(seen.endpoint);
                REQUIRE(endpoint.has_value());
                CHECK(IsLoopbackHost(Unwrap(endpoint).first));
                CHECK(Unwrap(endpoint).second == Unwrap(cachePort).second);
            }
        }
}
