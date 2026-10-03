// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/SrvResolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <tests/HostNamingFakes.hpp>
#include <tests/Unwrap.hpp>

#if defined(_WIN32)
    #include <FastCache/Platform/WindowsDnsRecords.hpp>

    #include <array>
    #include <bit>
    #include <cstring>
#endif

using namespace FastCache;
using FastCache::Testing::Unwrap;

namespace
{
/// A DNS message built by hand, so the parser is driven with bytes no resolver was asked for.
class DnsMessage
{
  public:
    /// Where the question's name starts: straight after the header.
    static constexpr std::uint16_t QuestionNameOffset = 12;

    /// Append a big-endian 16-bit field.
    DnsMessage& U16(std::uint16_t value)
    {
        _bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
        _bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        return *this;
    }

    /// Append a big-endian 32-bit field.
    DnsMessage& U32(std::uint32_t value)
    {
        return U16(static_cast<std::uint16_t>(value >> 16U)).U16(static_cast<std::uint16_t>(value & 0xFFFFU));
    }

    /// Append raw bytes.
    DnsMessage& Raw(std::vector<std::uint8_t> const& bytes)
    {
        _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
        return *this;
    }

    /// Append the labels of @p dotted, without the root that would end the name.
    DnsMessage& Labels(std::string_view dotted)
    {
        for (auto const label: std::views::split(dotted, '.'))
        {
            _bytes.push_back(static_cast<std::uint8_t>(std::ranges::distance(label)));
            _bytes.insert(_bytes.end(), label.begin(), label.end());
        }
        return *this;
    }

    /// Append @p dotted as an uncompressed name; empty is the root.
    DnsMessage& Name(std::string_view dotted)
    {
        if (!dotted.empty())
            Labels(dotted);
        _bytes.push_back(0);
        return *this;
    }

    /// Append a compression pointer to @p offset.
    DnsMessage& Pointer(std::uint16_t offset)
    {
        return U16(static_cast<std::uint16_t>(0xC000U | offset));
    }

    /// Append one resource record, owned by the question's name, carrying @p rdata.
    DnsMessage& Record(std::uint16_t type, std::vector<std::uint8_t> const& rdata, std::uint16_t rrClass = 1)
    {
        return Pointer(QuestionNameOffset)
            .U16(type)
            .U16(rrClass)
            .U32(300)
            .U16(static_cast<std::uint16_t>(rdata.size()))
            .Raw(rdata);
    }

    /// @return The message so far.
    [[nodiscard]] std::vector<std::uint8_t> const& Bytes() const
    {
        return _bytes;
    }

  private:
    std::vector<std::uint8_t> _bytes;
};

/// The name every response here answers.
constexpr auto QueryName = std::string_view { "_fastcache._tcp.corp.example" };

/// Where `corp.example` starts inside the question's name: past `_fastcache` and `_tcp`.
constexpr std::uint16_t CorpExampleOffset = DnsMessage::QuestionNameOffset + 11 + 5;

/// Header flags: a recursive answer with RCODE 0, the same flagged TC, and two failures.
constexpr std::uint16_t FlagsNoError = 0x8180;
constexpr std::uint16_t FlagsTruncated = 0x8380;
constexpr std::uint16_t FlagsNxDomain = 0x8183;
constexpr std::uint16_t FlagsServFail = 0x8182;

/// RR types this file writes.
constexpr std::uint16_t TypeA = 1;
constexpr std::uint16_t TypeCname = 5;
constexpr std::uint16_t TypeTxt = 16;
constexpr std::uint16_t TypeSrv = 33;

/// A response header and its one question, with the section counts given.
/// @param flags      The header flags.
/// @param answers    ANCOUNT.
/// @param additional ARCOUNT.
/// @return The message, ready for its records.
/// @param id         The header's id.
DnsMessage Response(std::uint16_t flags, std::uint16_t answers, std::uint16_t additional = 0, std::uint16_t id = 0x1234)
{
    auto message = DnsMessage {};
    message.U16(id).U16(flags).U16(1).U16(answers).U16(0).U16(additional);
    message.Name(QueryName).U16(TypeSrv).U16(1);
    return message;
}

/// @param target The target to name, uncompressed.
/// @return The RDATA of an SRV record for it.
std::vector<std::uint8_t> SrvRdata(SrvTarget const& target)
{
    auto rdata = DnsMessage {};
    rdata.U16(target.priority).U16(target.weight).U16(target.port).Name(target.host);
    return rdata.Bytes();
}

SrvTarget const OfficeA { .host = "office-a.corp.example", .port = 6674, .priority = 10, .weight = 5 };
SrvTarget const OfficeB { .host = "office-b.corp.example", .port = 7000, .priority = 20, .weight = 1 };
} // namespace

TEST_CASE("A resolver status maps to one fault", "[platform][formation][srv]")
{
    CHECK(SrvFaultOf(SrvStatus::NameError) == SrvLookupFault::NoSuchName);
    CHECK(SrvFaultOf(SrvStatus::EmptyAnswer) == SrvLookupFault::NoAnswer);
    CHECK(SrvFaultOf(SrvStatus::Other) == SrvLookupFault::ResolverFailed);
}

TEST_CASE("Every lookup fault has a distinct name a log line can carry", "[platform][formation][srv]")
{
    CHECK(SrvLookupFaultName(SrvLookupFault::NoSuchName) == "no-such-name");
    CHECK(SrvLookupFaultName(SrvLookupFault::NoAnswer) == "no-answer");
    CHECK(SrvLookupFaultName(SrvLookupFault::ResolverFailed) == "resolver-failed");
}

TEST_CASE("An answer offers its targets in order and drops the ones that name no service", "[platform][formation][srv]")
{
    auto const a = SrvTarget { .host = "office-a.corp.example", .port = 6674, .priority = 10, .weight = 5 };
    auto const b = SrvTarget { .host = "office-b.corp.example", .port = 7000, .priority = 20, .weight = 1 };
    auto const none = SrvTarget { .host = ".", .port = 0, .priority = 0, .weight = 0 };
    auto const empty = SrvTarget { .host = "", .port = 6674, .priority = 0, .weight = 0 };

    // The order is the resolver's: sorting is `OrderSeeds`'s decision, not this one's.
    auto const whole = SrvAnswerOf({ b, none, a, empty }, SrvAnswerExtent::Whole);
    REQUIRE(whole.has_value());
    CHECK(Unwrap(whole) == std::vector<SrvTarget> { b, a });

    // A truncated answer that still carried targets offers them.
    auto const partial = SrvAnswerOf({ a }, SrvAnswerExtent::Truncated);
    REQUIRE(partial.has_value());
    CHECK(Unwrap(partial) == std::vector<SrvTarget> { a });
}

TEST_CASE("An answer with nothing usable is NoAnswer when whole and ResolverFailed when truncated",
          "[platform][formation][srv]")
{
    auto const none = SrvTarget { .host = ".", .port = 0, .priority = 0, .weight = 0 };

    // RFC 2782: a sole "." target says the service is decidedly not available -- nothing is here.
    auto const decidedlyAbsent = SrvAnswerOf({ none }, SrvAnswerExtent::Whole);
    REQUIRE_FALSE(decidedlyAbsent.has_value());
    CHECK(decidedlyAbsent.error().fault == SrvLookupFault::NoAnswer);

    auto const emptyWhole = SrvAnswerOf({}, SrvAnswerExtent::Whole);
    REQUIRE_FALSE(emptyWhole.has_value());
    CHECK(emptyWhole.error().fault == SrvLookupFault::NoAnswer);

    // A truncated answer with nothing readable in it did not say "nothing here": a record may
    // exist that did not fit. Reporting NoAnswer would be a confident wrong signal.
    auto const emptyTruncated = SrvAnswerOf({}, SrvAnswerExtent::Truncated);
    REQUIRE_FALSE(emptyTruncated.has_value());
    CHECK(emptyTruncated.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("An answer with no usable target says how many it dropped as undialable", "[platform][formation][srv]")
{
    // `NoAnswer` alone covers an empty answer, RFC 2782's ".", and records nobody can dial -- and
    // only the last is a record an operator published wrong. The counts are what tell them apart,
    // and the "." is counted as what it is, never as undialable.
    auto const dot = SrvTarget { .host = ".", .port = 0, .priority = 0, .weight = 0 };
    auto const broken = SrvTarget { .host = "user@office.corp.example", .port = 6674, .priority = 0, .weight = 0 };
    auto const empty = SrvTarget { .host = "office..corp.example", .port = 6674, .priority = 0, .weight = 0 };

    auto const brokenWhole = SrvAnswerOf({ dot, broken, empty }, SrvAnswerExtent::Whole);
    REQUIRE_FALSE(brokenWhole.has_value());
    CHECK(brokenWhole.error() == SrvLookupFailure { .fault = SrvLookupFault::NoAnswer, .undialable = 2, .notOffered = 1 });
    CHECK(DescribeSrvLookupFailure(brokenWhole.error())
          == "no-answer: 1 root target (the domain says the service is decidedly not offered there), "
             "2 targets dropped as undialable");

    auto const nothing = SrvAnswerOf({}, SrvAnswerExtent::Whole);
    REQUIRE_FALSE(nothing.has_value());
    CHECK(nothing.error() == SrvLookupFailure { .fault = SrvLookupFault::NoAnswer, .undialable = 0, .notOffered = 0 });
    CHECK(DescribeSrvLookupFailure(nothing.error()) == "no-answer");

    // A truncated answer keeps its own fault and still says what it dropped.
    auto const brokenCut = SrvAnswerOf({ broken }, SrvAnswerExtent::Truncated);
    REQUIRE_FALSE(brokenCut.has_value());
    CHECK(brokenCut.error()
          == SrvLookupFailure { .fault = SrvLookupFault::ResolverFailed, .undialable = 1, .notOffered = 0 });
    CHECK(DescribeSrvLookupFailure(brokenCut.error()) == "resolver-failed: 1 target dropped as undialable");

    // And an answer with something usable in it reports no failure at all, however much it dropped.
    auto const office = SrvTarget { .host = "office-a.corp.example", .port = 6674, .priority = 0, .weight = 0 };
    CHECK(SrvAnswerOf({ broken, office }, SrvAnswerExtent::Whole).has_value());
}

TEST_CASE("The scripted resolver answers what it was scripted and counts every question", "[platform][formation][srv]")
{
    auto resolver = Testing::ScriptedSrvResolver {};
    auto const target = SrvTarget { .host = "office-a.corp.example", .port = 6674, .priority = 10, .weight = 5 };
    resolver.Answer("_fastcache._tcp.corp.example", std::vector<SrvTarget> { target });
    resolver.Answer("_fastcache._tcp.empty.example", std::unexpected { SrvFailureOf(SrvLookupFault::NoAnswer) });
    ISrvResolver const& seam = resolver;
    CHECK(resolver.Asked() == 0);

    auto const found = seam.Lookup("_fastcache._tcp.corp.example");
    REQUIRE(found.has_value());
    CHECK(Unwrap(found) == std::vector<SrvTarget> { target });

    auto const empty = seam.Lookup("_fastcache._tcp.empty.example");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().fault == SrvLookupFault::NoAnswer);

    // A name nothing scripted does not exist, as a real resolver would say.
    auto const unknown = seam.Lookup("_fastcache._tcp.elsewhere.example");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().fault == SrvLookupFault::NoSuchName);

    CHECK(resolver.Asked() == 3);
}

TEST_CASE("The system resolver answers a name that cannot exist with a fault and not a target",
          "[platform][formation][srv][smoke]")
{
    auto const resolver = MakeSystemSrvResolver();
    // `.invalid` is reserved (RFC 6761), so this is NXDOMAIN on a host with a resolver and a
    // resolver failure on one without; either is a fault, which is all this case can assert
    // everywhere. The case below is the positive direction.
    auto const answer = resolver->Lookup("_fastcache._tcp.fastcache-nonexistent.invalid");
    CHECK_FALSE(answer.has_value());
}

TEST_CASE("The system resolver finds a real SRV record when one is named", "[platform][formation][srv][smoke]")
{
    // The positive direction, which the `.invalid` case cannot give: a resolver that failed every
    // lookup would pass that one. Opt-in, because it needs a network and a record that exists,
    // and no name this repository could write down is guaranteed to stay both.
    auto const name = ReadEnvironmentVariable("FASTCACHE_TEST_SRV_NAME").value_or(std::string {});
    if (name.empty())
        SKIP("FASTCACHE_TEST_SRV_NAME is not set: name an SRV record that exists (e.g. _imaps._tcp.gmail.com) "
             "to check the system resolver finds one");

    auto const answer = MakeSystemSrvResolver()->Lookup(name);
    if (!answer.has_value() && answer.error().fault == SrvLookupFault::ResolverFailed)
        SKIP("no resolver answered " << name << " (no network?): this run cannot tell that from a broken resolver");

    INFO("name: " << name);
    REQUIRE(answer.has_value());
    auto const& targets = Unwrap(answer);
    REQUIRE_FALSE(targets.empty());
    for (auto const& target: targets)
    {
        INFO("target: " << target.host << ':' << target.port);
        CHECK_FALSE(target.host.empty());
        CHECK_FALSE(target.host.ends_with('.'));
        CHECK(target.port != 0);
    }
}

TEST_CASE("A whole DNS answer yields every SRV target in the order it holds them", "[platform][formation][srv][dns]")
{
    auto const message = Response(FlagsNoError, 2).Record(TypeSrv, SrvRdata(OfficeB)).Record(TypeSrv, SrvRdata(OfficeA));
    auto const targets = SrvTargetsInMessage(message.Bytes());
    REQUIRE(targets.has_value());
    CHECK(Unwrap(targets) == std::vector<SrvTarget> { OfficeB, OfficeA });
}

TEST_CASE("The header decides first: NXDOMAIN, another failure, and an empty answer", "[platform][formation][srv][dns]")
{
    auto const nxdomain = SrvTargetsInMessage(Response(FlagsNxDomain, 0).Bytes());
    REQUIRE_FALSE(nxdomain.has_value());
    CHECK(nxdomain.error().fault == SrvLookupFault::NoSuchName);

    auto const servfail = SrvTargetsInMessage(Response(FlagsServFail, 0).Bytes());
    REQUIRE_FALSE(servfail.has_value());
    CHECK(servfail.error().fault == SrvLookupFault::ResolverFailed);

    auto const empty = SrvTargetsInMessage(Response(FlagsNoError, 0).Bytes());
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().fault == SrvLookupFault::NoAnswer);

    // Shorter than a header is not a message at all.
    auto const header = Response(FlagsNoError, 0).Bytes();
    auto const stub = std::vector<std::uint8_t>(header.begin(), header.begin() + 11);
    auto const tooShort = SrvTargetsInMessage(stub);
    REQUIRE_FALSE(tooShort.has_value());
    CHECK(tooShort.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("A TC answer offers the targets it holds, and holding none is not an empty answer",
          "[platform][formation][srv][dns]")
{
    auto const partial = SrvTargetsInMessage(Response(FlagsTruncated, 1).Record(TypeSrv, SrvRdata(OfficeA)).Bytes());
    REQUIRE(partial.has_value());
    CHECK(Unwrap(partial) == std::vector<SrvTarget> { OfficeA });

    // The same empty answer as NoAnswer above, flagged TC: a record may exist that did not fit.
    auto const none = SrvTargetsInMessage(Response(FlagsTruncated, 0).Bytes());
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("A message cut mid-record keeps the records before the cut and reads nothing past it",
          "[platform][formation][srv][dns]")
{
    auto const whole =
        Response(FlagsNoError, 2).Record(TypeSrv, SrvRdata(OfficeA)).Record(TypeSrv, SrvRdata(OfficeB)).Bytes();
    auto const firstRecordEnd = Response(FlagsNoError, 2).Record(TypeSrv, SrvRdata(OfficeA)).Bytes().size();

    // Cut inside the second record's RDATA, and inside its fixed fields.
    for (auto const cut: { whole.size() - 5, firstRecordEnd + 4 })
    {
        INFO("cut at " << cut << " of " << whole.size());
        auto const message = std::vector<std::uint8_t>(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
        auto const targets = SrvTargetsInMessage(message);
        REQUIRE(targets.has_value());
        CHECK(Unwrap(targets) == std::vector<SrvTarget> { OfficeA });
    }

    // Cut inside the only record: nothing was read, and nothing is not "no answer".
    auto const only = Response(FlagsNoError, 1).Record(TypeSrv, SrvRdata(OfficeA)).Bytes();
    auto const message = std::vector<std::uint8_t>(only.begin(), only.end() - 3);
    auto const none = SrvTargetsInMessage(message);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("An SRV record whose RDATA is no longer than its fixed fields holds no target", "[platform][formation][srv][dns]")
{
    auto const fixedOnly = std::vector<std::uint8_t> { 0, 10, 0, 5, 0x1A, 0x12 };

    auto const beside =
        SrvTargetsInMessage(Response(FlagsNoError, 2).Record(TypeSrv, fixedOnly).Record(TypeSrv, SrvRdata(OfficeB)).Bytes());
    REQUIRE(beside.has_value());
    CHECK(Unwrap(beside) == std::vector<SrvTarget> { OfficeB });

    auto const alone = SrvTargetsInMessage(Response(FlagsNoError, 1).Record(TypeSrv, fixedOnly).Bytes());
    REQUIRE_FALSE(alone.has_value());
    CHECK(alone.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("A compressed target name is expanded from the message it points back into", "[platform][formation][srv][dns]")
{
    auto rdata = DnsMessage {};
    rdata.U16(OfficeA.priority).U16(OfficeA.weight).U16(OfficeA.port).Labels("office-a").Pointer(CorpExampleOffset);
    auto const targets = SrvTargetsInMessage(Response(FlagsNoError, 1).Record(TypeSrv, rdata.Bytes()).Bytes());
    REQUIRE(targets.has_value());
    CHECK(Unwrap(targets) == std::vector<SrvTarget> { OfficeA });
}

TEST_CASE("A CNAME, another type, another class and another section are skipped", "[platform][formation][srv][dns]")
{
    auto cname = DnsMessage {};
    cname.Name("_fastcache._tcp.elsewhere.example");
    // Every skipped record but the A record carries RDATA that WOULD read as an SRV target, so
    // each is skipped by the filter it names and by nothing else.
    auto const message = Response(FlagsNoError, 5, 1)
                             .Record(TypeCname, cname.Bytes())
                             .Record(TypeA, { 192, 0, 2, 1 })
                             .Record(TypeTxt, SrvRdata(OfficeB))
                             .Record(TypeSrv, SrvRdata(OfficeB), 3) // class CH
                             .Record(TypeSrv, SrvRdata(OfficeA))
                             .Record(TypeSrv, SrvRdata(OfficeB)); // the ADDITIONAL section
    auto const targets = SrvTargetsInMessage(message.Bytes());
    REQUIRE(targets.has_value());
    CHECK(Unwrap(targets) == std::vector<SrvTarget> { OfficeA });
}

TEST_CASE("A root target in a DNS answer is RFC 2782's not-here", "[platform][formation][srv][dns]")
{
    auto const root = SrvTarget { .host = "", .port = 0, .priority = 0, .weight = 0 };
    auto const targets = SrvTargetsInMessage(Response(FlagsNoError, 1).Record(TypeSrv, SrvRdata(root)).Bytes());
    REQUIRE_FALSE(targets.has_value());
    CHECK(targets.error() == SrvLookupFailure { .fault = SrvLookupFault::NoAnswer, .undialable = 0, .notOffered = 1 });
}

TEST_CASE("A domain that publishes the root target is not told its record is broken", "[platform][formation][srv]")
{
    // RFC 2782: the root target says the service is decidedly not available at this domain. That
    // is a record published RIGHT, so the line an operator reads must not call it undialable.
    for (auto const* const host: { ".", "" })
    {
        INFO("target '" << host << "'");
        CHECK(IsRootSrvTarget(host));
        auto const answer =
            SrvAnswerOf({ SrvTarget { .host = host, .port = 0, .priority = 0, .weight = 0 } }, SrvAnswerExtent::Whole);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error() == SrvLookupFailure { .fault = SrvLookupFault::NoAnswer, .undialable = 0, .notOffered = 1 });
        CHECK(DescribeSrvLookupFailure(answer.error())
              == "no-answer: 1 root target (the domain says the service is decidedly not offered there)");
        CHECK_FALSE(DescribeSrvLookupFailure(answer.error()).contains("undialable"));
    }

    // A name that merely ends in the root dot, or holds one, is a record published wrong.
    CHECK_FALSE(IsRootSrvTarget("office-a.corp.example."));
    CHECK_FALSE(IsRootSrvTarget(".."));
}

TEST_CASE("A hostile target name is unreadable rather than followed, and an undialable one is dropped",
          "[platform][formation][srv][dns]")
{
    auto const answerFor = [](std::vector<std::uint8_t> const& target) {
        auto rdata = DnsMessage {};
        rdata.U16(10).U16(5).U16(6674).Raw(target);
        return SrvTargetsInMessage(Response(FlagsNoError, 1).Record(TypeSrv, rdata.Bytes()).Bytes());
    };
    // Where the target's first byte sits: header and question, the record's owner pointer and
    // fixed fields, then the SRV fixed fields.
    auto const targetOffset = static_cast<std::uint16_t>(Response(FlagsNoError, 1).Bytes().size() + 2 + 10 + 6);

    auto const self = std::vector<std::uint8_t> { static_cast<std::uint8_t>(0xC0U | (targetOffset >> 8U)),
                                                  static_cast<std::uint8_t>(targetOffset & 0xFFU) };
    auto const pastTheEnd = std::vector<std::uint8_t> { 0xC0, 0xFF };
    auto const dotted = std::vector<std::uint8_t> { 3, 'a', '.', 'b', 0 };
    auto const control = std::vector<std::uint8_t> { 3, 'a', 0x01, 'b', 0 };
    auto const at = std::vector<std::uint8_t> { 3, 'a', '@', 'b', 0 };
    auto const extended = std::vector<std::uint8_t> { 0x41, 'a', 0 };
    auto tooLong = std::vector<std::uint8_t> {};
    for ([[maybe_unused]] auto const label: std::views::iota(0, 5))
    {
        tooLong.push_back(63);
        tooLong.insert(tooLong.end(), 63, static_cast<std::uint8_t>('x'));
    }
    tooLong.push_back(0);

    struct Hostile
    {
        std::string_view what;
        std::vector<std::uint8_t> target;
    };
    // Names that cannot be READ: the record is unreadable, so the answer is truncated.
    for (auto const& [what, target]: { Hostile { .what = "a pointer to itself", .target = self },
                                       Hostile { .what = "a pointer past the end of the message", .target = pastTheEnd },
                                       Hostile { .what = "an extended label type", .target = extended },
                                       Hostile { .what = "a name over 255 bytes", .target = tooLong } })
    {
        INFO(what);
        auto const answer = answerFor(target);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().fault == SrvLookupFault::ResolverFailed);
    }

    // Names that read but no one could DIAL: the whole answer was read and offered nothing.
    for (auto const& [what, target]: { Hostile { .what = "a label holding a dot", .target = dotted },
                                       Hostile { .what = "a label holding a control byte", .target = control },
                                       Hostile { .what = "a label holding an @", .target = at } })
    {
        INFO(what);
        auto const answer = answerFor(target);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().fault == SrvLookupFault::NoAnswer);
    }
}

TEST_CASE("A target name that does not end where its record does is refused", "[platform][formation][srv][dns]")
{
    auto const rdata = SrvRdata(OfficeA);
    auto const recordDeclaring = [&rdata](std::uint16_t declared, std::vector<std::uint8_t> const& trailing) {
        auto message = Response(FlagsNoError, 1);
        message.Pointer(DnsMessage::QuestionNameOffset).U16(TypeSrv).U16(1).U32(300).U16(declared).Raw(rdata).Raw(trailing);
        return SrvTargetsInMessage(message.Bytes());
    };
    auto const exact = static_cast<std::uint16_t>(rdata.size());

    // The control: the same bytes, declared at their own length, are OfficeA.
    auto const control = recordDeclaring(exact, {});
    REQUIRE(control.has_value());
    CHECK(Unwrap(control) == std::vector<SrvTarget> { OfficeA });

    // Declared shorter than the name: the name runs on past the record into bytes it does not own.
    auto const pastTheRecord = recordDeclaring(static_cast<std::uint16_t>(exact - 4), {});
    REQUIRE_FALSE(pastTheRecord.has_value());
    CHECK(pastTheRecord.error().fault == SrvLookupFault::ResolverFailed);

    // Declared longer than the name, with the slack filled: bytes nothing in the record accounts for.
    auto const shortOfTheRecord = recordDeclaring(static_cast<std::uint16_t>(exact + 2), { 0, 0 });
    REQUIRE_FALSE(shortOfTheRecord.has_value());
    CHECK(shortOfTheRecord.error().fault == SrvLookupFault::ResolverFailed);
}

#if defined(_WIN32)
namespace
{
/// One record for a hand-built DNS client list, of @p type, from @p section.
/// @param type    The record type.
/// @param section The section it came from.
/// @return The record, unlinked and carrying no data.
DNS_RECORDW RecordOf(WORD type, DNS_SECTION section)
{
    auto record = DNS_RECORDW {};
    record.wType = type;
    auto flags = DNS_RECORD_FLAGS {};
    flags.Section = static_cast<DWORD>(section);
    record.Flags = std::bit_cast<decltype(record.Flags)>(flags);
    return record;
}

/// An SRV record from @p section naming @p target (which the caller keeps alive).
/// @param section The section it came from.
/// @param target  The target's UTF-16 text.
/// @param port    The target's port.
/// @return The record, unlinked.
DNS_RECORDW SrvRecordOf(DNS_SECTION section, std::wstring& target, WORD port)
{
    auto record = RecordOf(DNS_TYPE_SRV, section);
    auto srv = DNS_SRV_DATAW {};
    srv.pNameTarget = target.data();
    srv.wPriority = 10;
    srv.wWeight = 5;
    srv.wPort = port;
    std::memcpy(&record.Data, &srv, sizeof srv);
    return record;
}

/// A CNAME record from @p section pointing at @p alias (which the caller keeps alive).
///
/// Its name pointer sits where an SRV record's target does, so a filter that stopped asking the
/// type would read @p alias as a target -- which is what makes the CNAME a case at all.
/// @param section The section it came from.
/// @param alias   The name it points at, in UTF-16.
/// @return The record, unlinked.
DNS_RECORDW CnameRecordOf(DNS_SECTION section, std::wstring& alias)
{
    auto record = RecordOf(DNS_TYPE_CNAME, section);
    auto cname = DNS_PTR_DATAW {};
    cname.pNameHost = alias.data();
    std::memcpy(&record.Data, &cname, sizeof cname);
    return record;
}

/// Link @p records in order, as the DNS client hands them back.
/// @param records The records.
/// @return The first.
template <std::size_t N>
DNS_RECORDW const* Linked(std::array<DNS_RECORDW, N>& records)
{
    for (auto const index: std::views::iota(std::size_t { 1 }, N))
        records[index - 1].pNext = &records[index];
    return records.data();
}
} // namespace

TEST_CASE("A DNS client list yields only the SRV records of its answer section", "[platform][formation][srv][dns]")
{
    auto officeA = std::wstring { L"office-a.corp.example" };
    auto officeB = std::wstring { L"office-b.corp.example" };
    auto root = std::wstring { L"." };
    auto alias = std::wstring { L"_fastcache._tcp.elsewhere.example" };
    auto records = std::array {
        CnameRecordOf(DnsSectionAnswer, alias),          // a CNAME the name led through
        SrvRecordOf(DnsSectionAnswer, officeA, 6674),    // the one target
        RecordOf(DNS_TYPE_A, DnsSectionAddtional),       // the target's own address
        SrvRecordOf(DnsSectionAddtional, officeB, 7000), // an SRV record volunteered elsewhere
        SrvRecordOf(DnsSectionAnswer, root, 0),          // RFC 2782's not-here
    };
    auto const targets = SrvTargetsInRecordList(ERROR_SUCCESS, Linked(records));
    REQUIRE(targets.has_value());
    CHECK(Unwrap(targets)
          == std::vector<SrvTarget> {
              SrvTarget { .host = "office-a.corp.example", .port = 6674, .priority = 10, .weight = 5 } });
}

TEST_CASE("A DNS client target that is not valid UTF-16 is unreadable, not empty", "[platform][formation][srv][dns]")
{
    auto broken = std::wstring { L"office-" };
    broken.push_back(static_cast<wchar_t>(0xD800));
    auto officeB = std::wstring { L"office-b.corp.example" };

    auto beside = std::array { SrvRecordOf(DnsSectionAnswer, broken, 6674), SrvRecordOf(DnsSectionAnswer, officeB, 7000) };
    auto const kept = SrvTargetsInRecordList(ERROR_SUCCESS, Linked(beside));
    REQUIRE(kept.has_value());
    CHECK(Unwrap(kept).size() == 1);

    auto alone = std::array { SrvRecordOf(DnsSectionAnswer, broken, 6674) };
    auto const none = SrvTargetsInRecordList(ERROR_SUCCESS, Linked(alone));
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("A DNS client status maps to the fault its name says", "[platform][formation][srv][dns]")
{
    struct Case
    {
        DNS_STATUS status;
        SrvLookupFault fault;
    };
    for (auto const [status, fault]: { Case { .status = DNS_ERROR_RCODE_NAME_ERROR, .fault = SrvLookupFault::NoSuchName },
                                       Case { .status = DNS_INFO_NO_RECORDS, .fault = SrvLookupFault::NoAnswer },
                                       Case { .status = ERROR_TIMEOUT, .fault = SrvLookupFault::ResolverFailed },
                                       Case { .status = ERROR_CANCELLED, .fault = SrvLookupFault::ResolverFailed },
                                       Case { .status = ERROR_SUCCESS, .fault = SrvLookupFault::NoAnswer } })
    {
        INFO("status " << status);
        auto const answer = SrvTargetsInRecordList(status, nullptr);
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().fault == fault);
        CHECK(answer.error().undialable == 0);
        CHECK(answer.error().notOffered == 0);
    }
}
TEST_CASE("A DNS client target no one could dial is dropped, as the parser drops it", "[platform][formation][srv][dns]")
{
    auto undialable = std::wstring { L"user@office.corp.example" };
    auto officeB = std::wstring { L"office-b.corp.example" };

    auto beside =
        std::array { SrvRecordOf(DnsSectionAnswer, undialable, 6674), SrvRecordOf(DnsSectionAnswer, officeB, 7000) };
    auto const kept = SrvTargetsInRecordList(ERROR_SUCCESS, Linked(beside));
    REQUIRE(kept.has_value());
    CHECK(Unwrap(kept)
          == std::vector<SrvTarget> {
              SrvTarget { .host = "office-b.corp.example", .port = 7000, .priority = 10, .weight = 5 } });

    // Read whole and offering nothing: NoAnswer, the same fault the parser gives the same record.
    auto alone = std::array { SrvRecordOf(DnsSectionAnswer, undialable, 6674) };
    auto const none = SrvTargetsInRecordList(ERROR_SUCCESS, Linked(alone));
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().fault == SrvLookupFault::NoAnswer);
}
#endif

TEST_CASE("A forward compression pointer is refused even where it lands on a readable name",
          "[platform][formation][srv][dns]")
{
    // The answer's target is nothing but a pointer, so its RDATA is 8 bytes and the end-of-record
    // check passes either way. It points FORWARD, at the owner name of an additional-section record
    // written after it: in bounds, and a name anyone could dial. So the backward rule is the only
    // thing between this message and a target -- which is what makes the case a pin on that rule
    // rather than on the bounds check a pointer past the message meets first.
    auto message = Response(FlagsNoError, 1, 1);
    auto const answerStart = message.Bytes().size();
    auto const answerBytes = std::size_t { 2 + 10 + 8 }; // owner pointer, fixed fields, RDATA
    auto const forwardName = static_cast<std::uint16_t>(answerStart + answerBytes);
    auto rdata = DnsMessage {};
    rdata.U16(OfficeB.priority).U16(OfficeB.weight).U16(OfficeB.port).Pointer(forwardName);
    message.Record(TypeSrv, rdata.Bytes());
    REQUIRE(message.Bytes().size() == forwardName);
    message.Name(OfficeB.host).U16(TypeA).U16(1).U32(300).U16(4).Raw({ 192, 0, 2, 2 });

    auto const answer = SrvTargetsInMessage(message.Bytes());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("A compression pointer into the header is refused", "[platform][formation][srv][dns]")
{
    // An id of 0x0161 and flags of zero make the header's first three bytes spell the name "a": a
    // one-byte label 'a', then a zero that reads as the root. A pointer to offset 0 would read that
    // as a dialable target. Flags of zero clear QR -- the bit that otherwise keeps the flags from
    // reading as a label at all -- and RCODE, so the message is still read as a NOERROR answer.
    constexpr std::uint16_t HeaderSpellsA = 0x0161;
    auto rdata = DnsMessage {};
    rdata.U16(OfficeA.priority).U16(OfficeA.weight).U16(OfficeA.port).Pointer(0);
    auto const message = Response(0, 1, 0, HeaderSpellsA).Record(TypeSrv, rdata.Bytes());

    auto const answer = SrvTargetsInMessage(message.Bytes());
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().fault == SrvLookupFault::ResolverFailed);
}

TEST_CASE("One predicate decides which SRV targets a node would dial", "[platform][formation][srv][dns]")
{
    auto const label63 = std::string(63, 'x');
    auto const label64 = std::string(64, 'x');
    // 63 + 1 + 63 + 1 + 63 + 1 + 61 = 253 characters; one more is 254.
    auto const name253 = label63 + "." + label63 + "." + label63 + "." + std::string(61, 'x');
    auto const name254 = name253 + "x";
    REQUIRE(name253.size() == 253);

    struct Row
    {
        std::string host;
        bool dialable;
    };
    for (auto const& [host, dialable]: {
             Row { .host = "office-a.corp.example", .dialable = true },
             Row { .host = "office-a", .dialable = true },
             Row { .host = "_fastcache._tcp.corp.example", .dialable = true },
             Row { .host = "A-1.b_2.EXAMPLE", .dialable = true },
             Row { .host = label63 + ".example", .dialable = true },
             Row { .host = name253, .dialable = true },
             Row { .host = "", .dialable = false },
             Row { .host = ".", .dialable = false },
             Row { .host = "office-a.corp.example.", .dialable = false },
             Row { .host = ".office-a", .dialable = false },
             Row { .host = "office..example", .dialable = false },
             Row { .host = "-office.example", .dialable = false },
             Row { .host = "office-.example", .dialable = false },
             Row { .host = "office a.example", .dialable = false },
             Row { .host = "office:6674", .dialable = false },
             Row { .host = "[::1]", .dialable = false },
             Row { .host = "fe80::1%eth0", .dialable = false },
             Row { .host = "user@office.example", .dialable = false },
             Row { .host = "office/a.example", .dialable = false },
             Row { .host = "a\\046b.example", .dialable = false },
             Row { .host = "gr\xC3\xBCn.example", .dialable = false },
             Row { .host = label64 + ".example", .dialable = false },
             Row { .host = name254, .dialable = false },
         })
    {
        INFO("host: " << host);
        CHECK(IsDialableSrvTarget(host) == dialable);
    }
}
