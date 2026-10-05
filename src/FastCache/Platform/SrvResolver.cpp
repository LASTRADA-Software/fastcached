// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/SrvResolver.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

#if defined(_WIN32)
    #include <FastCache/Platform/NarrowText.hpp>
    #include <FastCache/Platform/WindowsDnsRecords.hpp>

    #include <bit>
    #include <condition_variable>
    #include <cstring>
    #include <mutex>

    #include <windows.h>

    #include <windns.h>
#else
    #include <sys/types.h>

    #include <type_traits>

    #include <netdb.h>
    #include <resolv.h>

    #include <arpa/nameser.h>
    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace
{
    /// Which fault a resolver status is.
    struct SrvStatusRow
    {
        SrvStatus status;     ///< What the platform's resolver reported.
        SrvLookupFault fault; ///< What the caller is told.
    };

    /// One fault per status.
    constexpr EnumTable<SrvStatus, SrvStatusRow> SrvStatusTable { {
        { .status = SrvStatus::NameError, .fault = SrvLookupFault::NoSuchName },
        { .status = SrvStatus::EmptyAnswer, .fault = SrvLookupFault::NoAnswer },
        { .status = SrvStatus::Other, .fault = SrvLookupFault::ResolverFailed },
    } };
    static_assert(RowsInEnumeratorOrder(SrvStatusTable, &SrvStatusRow::status),
                  "SrvStatusTable must hold one row per SrvStatus, in enumerator order");

    /// What an answer with no usable target means, by how much of it was read.
    struct SrvAnswerExtentRow
    {
        SrvAnswerExtent extent;      ///< How much of the answer was read.
        SrvLookupFault faultIfEmpty; ///< The fault when nothing usable was found in it.
    };

    /// A whole answer with nothing in it says there is nothing; a truncated one does not.
    constexpr EnumTable<SrvAnswerExtent, SrvAnswerExtentRow> SrvAnswerExtentTable { {
        { .extent = SrvAnswerExtent::Whole, .faultIfEmpty = SrvLookupFault::NoAnswer },
        { .extent = SrvAnswerExtent::Truncated, .faultIfEmpty = SrvLookupFault::ResolverFailed },
    } };
    static_assert(RowsInEnumeratorOrder(SrvAnswerExtentTable, &SrvAnswerExtentRow::extent),
                  "SrvAnswerExtentTable must hold one row per SrvAnswerExtent, in enumerator order");

    /// The longest presentation form a name fitting DNS's 255 wire bytes can have.
    constexpr std::size_t MaxHostNameChars = 253;

    /// The longest label DNS allows.
    constexpr std::size_t MaxLabelChars = 63;

    /// @param character One character of a host name.
    /// @return Whether a host name label may hold it: an ASCII letter or digit, `-` or `_`.
    [[nodiscard]] constexpr bool IsHostNameCharacter(char character) noexcept
    {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
               || (character >= '0' && character <= '9') || character == '-' || character == '_';
    }

    /// @param label One label of a host name.
    /// @return Whether it is a label a host name may have.
    [[nodiscard]] bool IsHostNameLabel(std::string_view label) noexcept
    {
        return !label.empty() && label.size() <= MaxLabelChars && !label.starts_with('-') && !label.ends_with('-')
               && std::ranges::all_of(label, IsHostNameCharacter);
    }

    /// A platform resolver's return code, and the status it is.
    struct PlatformStatusRow
    {
        long code;        ///< The platform's code: a `DNS_STATUS` on Windows, an `h_errno` value elsewhere.
        SrvStatus status; ///< What it reports.
    };

    /// @param rows The platform's codes.
    /// @param code What its resolver returned.
    /// @return The status @p code is; `Other` for every code no row names.
    [[nodiscard]] SrvStatus StatusOfCode(std::span<PlatformStatusRow const> rows, long code) noexcept
    {
        auto const* const row = core::findOrNull(rows, code, &PlatformStatusRow::code);
        return row != nullptr ? row->status : SrvStatus::Other;
    }

    /// The precondition every `Lookup` states, checked in one place.
    /// @param name The name asked for.
    /// @return Whether it may be asked at all.
    [[nodiscard]] bool MayQuery(std::string_view name) noexcept
    {
        assert(!name.empty() && "no DNS domain is no SRV query; the caller must not ask one");
        return !name.empty();
    }

    /// The fixed-size DNS message header: id, flags, and the four section counts.
    constexpr std::size_t DnsHeaderBytes = 12;

    /// What follows a question's name: its type and class.
    constexpr std::size_t DnsQuestionTailBytes = 4;

    /// What follows a resource record's owner name: type, class, TTL and RDATA length.
    constexpr std::size_t DnsRecordFixedBytes = 10;

    /// An SRV record's RDATA before its target: priority, weight and port.
    constexpr std::size_t SrvFixedFieldBytes = 6;

    /// The longest name DNS allows on the wire, length bytes and root included (RFC 1035 3.1).
    constexpr std::size_t DnsMaxNameBytes = 255;

    /// RR type SRV (RFC 2782).
    constexpr std::uint16_t DnsTypeSrv = 33;

    /// RR class IN.
    constexpr std::uint16_t DnsClassInternet = 1;

    /// The TC bit of the header's flags word.
    constexpr std::uint16_t DnsFlagTruncated = 0x0200;

    /// The RCODE field of the header's flags word.
    constexpr std::uint16_t DnsRcodeMask = 0x000F;

    /// The two top bits of a length byte, which mark a compression pointer when both are set.
    constexpr std::uint8_t DnsPointerMark = 0xC0;

    /// The offset a compression pointer carries, below its two marker bits.
    constexpr std::uint16_t DnsPointerOffsetMask = 0x3FFF;

    /// A header RCODE, and the status it is. NOERROR is not a row: it is not a failure.
    struct DnsRcodeRow
    {
        std::uint16_t rcode; ///< The RCODE.
        SrvStatus status;    ///< What it reports.
    };

    /// The RCODEs that are not `Other`.
    constexpr auto DnsRcodeRows = std::array {
        DnsRcodeRow { .rcode = 3, .status = SrvStatus::NameError }, // NXDOMAIN
    };

    /// @param message The message.
    /// @param offset  Where the field starts; the caller has checked two bytes are there.
    /// @return The big-endian 16-bit field at @p offset.
    [[nodiscard]] std::uint16_t Read16(std::span<std::uint8_t const> message, std::size_t offset) noexcept
    {
        return static_cast<std::uint16_t>((message[offset] << 8U) | message[offset + 1]);
    }

    /// A name read from a message.
    struct DnsName
    {
        std::string text; ///< Its labels joined by dots, in presentation form; empty for the root.
        std::size_t end;  ///< The offset just past the name where it SITS -- past its first pointer.
    };

    /// Append one label byte in presentation form.
    ///
    /// A byte that would not read back as itself -- a `.`, which would split the label, a `\`,
    /// and anything not printable ASCII -- is written as a `\DDD` escape, as `dn_expand` does. The
    /// text then says exactly what the wire said, and `IsDialableSrvTarget` refuses it.
    /// @param text The name so far.
    /// @param byte The byte.
    void AppendLabelByte(std::string& text, std::uint8_t byte)
    {
        if (byte > 0x20 && byte < 0x7F && byte != '.' && byte != '\\')
            text += static_cast<char>(byte);
        else
            text += std::format("\\{:03}", byte);
    }

    /// Read the (possibly compressed) name at @p offset.
    ///
    /// Terminates on every input: a pointer must point strictly before itself, so a run of
    /// pointers strictly descends, and every label adds to a length capped at `DnsMaxNameBytes`.
    /// A pointer must also point past the header, where no name is.
    /// @param message The message.
    /// @param offset  Where the name starts.
    /// @return The name, or `std::nullopt` when it runs past the message, loops, is too long, or
    ///         uses a reserved label type.
    [[nodiscard]] std::optional<DnsName> ReadName(std::span<std::uint8_t const> message, std::size_t offset)
    {
        auto name = DnsName { .text = {}, .end = 0 };
        auto end = std::optional<std::size_t> {};
        auto position = offset;
        auto wireBytes = std::size_t { 1 }; // the root's length byte
        while (true)
        {
            if (position >= message.size())
                return std::nullopt;
            auto const length = message[position];
            if ((length & DnsPointerMark) == DnsPointerMark)
            {
                if (position + 1 >= message.size())
                    return std::nullopt;
                auto const target = static_cast<std::size_t>(Read16(message, position) & DnsPointerOffsetMask);
                if (target >= position)
                    return std::nullopt;
                // No name lives in the header, so a pointer into it names whatever the id and the
                // flags happen to spell -- a name only while the QR bit keeps the flags from
                // reading as a label, which is not something to rest a refusal on.
                if (target < DnsHeaderBytes)
                    return std::nullopt;
                if (!end.has_value())
                    end = position + 2;
                position = target;
                continue;
            }
            if ((length & DnsPointerMark) != 0)
                return std::nullopt; // an extended label type: nothing here reads them
            if (length == 0)
            {
                name.end = end.value_or(position + 1);
                return name;
            }
            wireBytes += 1U + length;
            if (wireBytes > DnsMaxNameBytes || position + 1 + length > message.size())
                return std::nullopt;
            if (!name.text.empty())
                name.text += '.';
            for (auto const byte: message.subspan(position + 1, length))
                AppendLabelByte(name.text, byte);
            position += 1U + length;
        }
    }

    /// The target one SRV record's RDATA holds.
    /// @param message The whole message, which a compressed target points back into.
    /// @param rdata   Where the RDATA starts.
    /// @param length  The RDATA's declared length, already checked to lie inside the message.
    /// @return The target, or `std::nullopt` when the RDATA does not hold one -- including a target
    ///         whose name, where it sits, does not end exactly where the RDATA does: one running past
    ///         the record is reading the NEXT record's bytes as a host name, and one ending short
    ///         leaves bytes the record declared and nothing accounts for.
    [[nodiscard]] std::optional<SrvTarget> SrvTargetInRdata(std::span<std::uint8_t const> message,
                                                            std::size_t rdata,
                                                            std::size_t length)
    {
        if (length <= SrvFixedFieldBytes)
            return std::nullopt;
        auto target = ReadName(message, rdata + SrvFixedFieldBytes);
        if (!target.has_value() || target->end != rdata + length)
            return std::nullopt;
        return SrvTarget { .host = std::move(target->text),
                           .port = Read16(message, rdata + 4),
                           .priority = Read16(message, rdata),
                           .weight = Read16(message, rdata + 2) };
    }

#if defined(_WIN32)
    /// The Windows DNS client's codes that are not `Other`.
    constexpr auto PlatformStatusRows = std::array {
        PlatformStatusRow { .code = DNS_ERROR_RCODE_NAME_ERROR, .status = SrvStatus::NameError },
        PlatformStatusRow { .code = DNS_INFO_NO_RECORDS, .status = SrvStatus::EmptyAnswer },
    };

    /// Frees a record list the DNS client returned.
    struct RecordListFree
    {
        /// @param list The list to free.
        void operator()(DNS_RECORD* list) const noexcept
        {
            DnsRecordListFree(list, DnsFreeRecordList);
        }
    };

    /// A record list the DNS client returned, owned.
    using RecordList = std::unique_ptr<DNS_RECORD, RecordListFree>;

    /// What an asynchronous query hands back to the thread waiting for it.
    struct PendingQuery
    {
        std::mutex mutex;                    ///< Guards everything below.
        std::condition_variable completion;  ///< Signalled once `done` is set.
        bool done { false };                 ///< The completion routine has run.
        DNS_STATUS status { ERROR_SUCCESS }; ///< The query's status, once done.
        RecordList records;                  ///< The query's records, once done.
    };

    /// `DnsQueryEx`'s completion routine: take the answer and wake the waiter.
    /// @param context The `PendingQuery` the lookup is waiting on.
    /// @param result  The query's result; its records become the waiter's to free.
    VOID WINAPI OnQueryCompleted(PVOID context, PDNS_QUERY_RESULT result)
    {
        auto* const pending = static_cast<PendingQuery*>(context);
        // Notified UNDER the lock. The waiter returns -- and frees `pending` -- as soon as it sees
        // `done`, which it cannot do before this thread releases the mutex; a notify after the
        // unlock could touch a condition variable in a frame that has already gone.
        std::scoped_lock const lock { pending->mutex };
        pending->status = result->QueryStatus;
        pending->records.reset(result->pQueryRecords);
        result->pQueryRecords = nullptr;
        pending->done = true;
        pending->completion.notify_one();
    }

    /// @param record A record the DNS client returned.
    /// @return Which section of the answer it came from, read without naming a union member.
    [[nodiscard]] DWORD SectionOf(DNS_RECORDW const& record) noexcept
    {
        return std::bit_cast<DNS_RECORD_FLAGS>(record.Flags).Section;
    }

    /// @param record A record the DNS client returned, of type `DNS_TYPE_SRV`.
    /// @return Its SRV data. Every alternative of `DNS_RECORDW::Data` starts at its first byte, so
    ///         the SRV one is copied out rather than read through the union.
    [[nodiscard]] DNS_SRV_DATAW SrvDataOf(DNS_RECORDW const& record) noexcept
    {
        auto srv = DNS_SRV_DATAW {};
        std::memcpy(&srv, &record.Data, sizeof srv);
        return srv;
    }

    /// The targets a finished query found, or why there are none.
    /// @param status  The query's status.
    /// @param records Its records.
    /// @return The usable targets, or the fault.
    [[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> AnswerOf(DNS_STATUS status,
                                                                                   RecordList const& records)
    {
        // `DnsQueryEx` speaks UTF-16 whatever `UNICODE` says: `DNS_QUERY_RESULT::pQueryRecords` is
        // the narrow `PDNS_RECORD` in a build without `UNICODE`, but every string it points at is
        // wide.
        return SrvTargetsInRecordList(status, reinterpret_cast<DNS_RECORDW const*>(records.get()));
    }

    /// `DnsQueryEx`, cancelled at `SrvLookupDeadline`.
    class WindowsSrvResolver final: public ISrvResolver
    {
      public:
        [[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> Lookup(std::string_view name) const override
        {
            if (!MayQuery(name))
                return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };
            auto const wideName = WideTextFromUtf8(name);
            if (!wideName.has_value())
                return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };

            PendingQuery pending;
            DNS_QUERY_REQUEST request {};
            request.Version = DNS_QUERY_REQUEST_VERSION1;
            request.QueryName = wideName->c_str();
            request.QueryType = DNS_TYPE_SRV;
            // TREAT_AS_FQDN: the name is complete as asked. Without it an NXDOMAIN sends the client
            // down the suffix search list, asking `_fastcache._tcp.corp.example.<suffix>` for every
            // suffix -- names nobody published, each spending time inside the deadline.
            request.QueryOptions = DNS_QUERY_STANDARD | DNS_QUERY_TREAT_AS_FQDN;
            request.pQueryCompletionCallback = &OnQueryCompleted;
            request.pQueryContext = &pending;
            DNS_QUERY_RESULT result {};
            result.Version = DNS_QUERY_RESULTS_VERSION1;
            DNS_QUERY_CANCEL cancel {};

            auto const started = ::DnsQueryEx(&request, &result, &cancel);
            if (started != DNS_REQUEST_PENDING)
            {
                // Answered on the spot -- from the client's cache, or refused outright. No
                // completion routine runs, and the answer is in `result`.
                auto const records = RecordList { result.pQueryRecords };
                return AnswerOf(started, records);
            }

            auto lock = std::unique_lock { pending.mutex };
            if (!pending.completion.wait_for(lock, SrvLookupDeadline, [&pending] { return pending.done; }))
            {
                lock.unlock();
                auto const cancelled = ::DnsCancelQuery(&cancel) == ERROR_SUCCESS;
                lock.lock();
                // The completion routine runs exactly once whatever the cancellation did, and it
                // writes into `pending` and `result` -- both in THIS frame, so the frame may not
                // end until it has. How long that takes is the DNS CLIENT's to bound, not
                // `SrvLookupDeadline`'s: a cancellation that took completes the query at once with
                // ERROR_CANCELLED, and one that was refused -- the query was already completing --
                // leaves it to finish on its own, within the client's own query timeout.
                pending.completion.wait(lock, [&pending] { return pending.done; });
                if (cancelled)
                    return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };
                // A refused cancellation means the answer arrived as the deadline passed: it is
                // a real answer, and reporting a resolver failure over it would be a wrong signal.
            }
            return AnswerOf(pending.status, pending.records);
        }
    };
#else
    /// The resolver's codes (`h_errno` values) that are not `Other`.
    constexpr auto PlatformStatusRows = std::array {
        PlatformStatusRow { .code = HOST_NOT_FOUND, .status = SrvStatus::NameError },
        PlatformStatusRow { .code = NO_DATA, .status = SrvStatus::EmptyAnswer },
    };

    /// One pass over the configured servers. A lost datagram is a `ResolverFailed` the caller's
    /// next round asks again, never a second pass that doubles the wait.
    constexpr int ResolverPasses = 1;

    /// How long a server is waited on, in seconds -- `res_state::retrans`.
    constexpr int ResolverRetransmitSeconds = 2;

    // glibc waits `retrans << n` over `nscount` on the n-th server of a pass (all of `retrans` on
    // the first), and BIND-derived resolvers `retrans` on each server of a first pass: either way
    // one pass sums to at most MAXNS * retrans. Inferred from those sources, not measured here.
    static_assert(MAXNS * ResolverRetransmitSeconds * ResolverPasses <= SrvLookupDeadline.count(),
                  "the POSIX resolver's configured worst case must fit inside SrvLookupDeadline");

    /// The largest DNS message there is: its length travels in 16 bits (RFC 1035 4.2.2). Spelled
    /// here rather than as `NS_MAXMSG`, which glibc's <arpa/nameser.h> defines and Apple's does
    /// not -- the macOS build stopped at the first use of it.
    constexpr std::size_t MaxDnsMessageBytes = 65535;
    #if defined(NS_MAXMSG)
    static_assert(MaxDnsMessageBytes == NS_MAXMSG, "the resolver headers disagree about a DNS message's ceiling");
    #endif

    /// The resolver's state, as `res_ninit` fills it: named through the `res_state` pointer
    /// typedef rather than by its reserved struct tag.
    using ResolverStateData = std::remove_pointer_t<res_state>;

    /// A private `res_state`, so a lookup shares nothing with another thread's.
    class ResolverState
    {
      public:
        ResolverState():
            _ready { res_ninit(&_state) == 0 }
        {
        }
        ResolverState(ResolverState const&) = delete;
        ResolverState(ResolverState&&) = delete;
        ResolverState& operator=(ResolverState const&) = delete;
        ResolverState& operator=(ResolverState&&) = delete;

        ~ResolverState()
        {
            if (!_ready)
                return;
    #if defined(__APPLE__)
            // `res_nclose` closes the sockets and frees nothing on Apple's libresolv; this frees both.
            res_ndestroy(&_state);
    #else
            // glibc's `res_nclose` closes the sockets and frees what `res_ninit` allocated.
            res_nclose(&_state);
    #endif
        }

        /// @return Whether `res_ninit` succeeded; nothing else here may be used when it did not.
        [[nodiscard]] bool Ready() const noexcept
        {
            return _ready;
        }

        /// @return The state, for `res_nquery`.
        [[nodiscard]] ResolverStateData& State() noexcept
        {
            return _state;
        }

      private:
        ResolverStateData _state {};
        bool _ready;
    };

    /// `res_nquery` over a private state, configured to finish inside `SrvLookupDeadline`.
    class PosixSrvResolver final: public ISrvResolver
    {
      public:
        [[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> Lookup(std::string_view name) const override
        {
            if (!MayQuery(name))
                return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };

            auto resolver = ResolverState {};
            if (!resolver.Ready())
                return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };

            // Set after `res_ninit`, so the timing `resolv.conf` or `RES_OPTIONS` asked for is
            // overridden rather than inherited. The TCP path is closed on BOTH of its routes,
            // because its connect and read block with no timeout this state can set: RES_USEVC
            // (`options use-vc`, which either source can turn on) would send every query over
            // TCP, and without RES_IGNTC a truncated answer is retried there.
            // `SrvTargetsInMessage` reads a TC answer as truncated rather than as empty.
            auto& state = resolver.State();
            state.retrans = ResolverRetransmitSeconds;
            state.retry = ResolverPasses;
            state.options &= ~static_cast<decltype(state.options)>(RES_USEVC);
            state.options |= RES_IGNTC;

            auto const query = std::string { name };
            auto answer = std::vector<std::uint8_t>(MaxDnsMessageBytes);
            auto const length =
                res_nquery(&state, query.c_str(), ns_c_in, ns_t_srv, answer.data(), static_cast<int>(answer.size()));
            if (length < 0)
                return std::unexpected { SrvFailureOf(SrvFaultOf(StatusOfCode(PlatformStatusRows, state.res_h_errno))) };

            // `res_nquery` returns the answer's FULL length even when it copied less.
            auto const read = std::min(static_cast<std::size_t>(length), answer.size());
            return SrvTargetsInMessage(std::span<std::uint8_t const> { answer }.first(read));
        }
    };
#endif
} // namespace

std::unique_ptr<ISrvResolver> MakeSystemSrvResolver()
{
#if defined(_WIN32)
    return std::make_unique<WindowsSrvResolver>();
#else
    return std::make_unique<PosixSrvResolver>();
#endif
}

#if defined(_WIN32)
std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvTargetsInRecordList(DNS_STATUS status, DNS_RECORDW const* first)
{
    if (status != ERROR_SUCCESS)
        return std::unexpected { SrvFailureOf(SrvFaultOf(StatusOfCode(PlatformStatusRows, status))) };

    auto extent = SrvAnswerExtent::Whole;
    auto targets = std::vector<SrvTarget> {};
    // Both the type and the section are asked: the list also carries the ADDITIONAL section.
    auto const* record = first;
    while (record != nullptr)
    {
        if (record->wType == DNS_TYPE_SRV && SectionOf(*record) == static_cast<DWORD>(DnsSectionAnswer))
        {
            auto const srv = SrvDataOf(*record);
            auto host = srv.pNameTarget != nullptr ? Utf8FromWideText(srv.pNameTarget)
                                                   : std::optional<std::string> { std::string {} };
            if (host.has_value())
                targets.push_back(SrvTarget {
                    .host = std::move(*host), .port = srv.wPort, .priority = srv.wPriority, .weight = srv.wWeight });
            else
                extent = SrvAnswerExtent::Truncated;
        }
        record = record->pNext;
    }
    return SrvAnswerOf(std::move(targets), extent);
}
#endif

std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvTargetsInMessage(std::span<std::uint8_t const> message)
{
    if (message.size() < DnsHeaderBytes)
        return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };

    auto const flags = Read16(message, 2);
    if (auto const rcode = static_cast<std::uint16_t>(flags & DnsRcodeMask); rcode != 0)
    {
        auto const* const row = core::findOrNull(DnsRcodeRows, rcode, &DnsRcodeRow::rcode);
        return std::unexpected { SrvFailureOf(SrvFaultOf(row != nullptr ? row->status : SrvStatus::Other)) };
    }

    auto position = DnsHeaderBytes;
    for ([[maybe_unused]] auto const question: std::views::iota(0U, static_cast<unsigned>(Read16(message, 4))))
    {
        auto const name = ReadName(message, position);
        if (!name.has_value() || name->end + DnsQuestionTailBytes > message.size())
            return std::unexpected { SrvFailureOf(SrvLookupFault::ResolverFailed) };
        position = name->end + DnsQuestionTailBytes;
    }

    auto extent = (flags & DnsFlagTruncated) != 0 ? SrvAnswerExtent::Truncated : SrvAnswerExtent::Whole;
    auto targets = std::vector<SrvTarget> {};
    for ([[maybe_unused]] auto const answer: std::views::iota(0U, static_cast<unsigned>(Read16(message, 6))))
    {
        auto const owner = ReadName(message, position);
        if (!owner.has_value() || owner->end + DnsRecordFixedBytes > message.size())
        {
            // Nothing past a record that will not parse can be located, so the rest is unread.
            extent = SrvAnswerExtent::Truncated;
            break;
        }
        auto const fixed = owner->end;
        auto const rdata = fixed + DnsRecordFixedBytes;
        auto const length = static_cast<std::size_t>(Read16(message, fixed + 8));
        if (rdata + length > message.size())
        {
            extent = SrvAnswerExtent::Truncated;
            break;
        }
        position = rdata + length;

        // A CNAME the name led through sits in the same section; it is not a target.
        if (Read16(message, fixed) != DnsTypeSrv || Read16(message, fixed + 2) != DnsClassInternet)
            continue;
        if (auto target = SrvTargetInRdata(message, rdata, length); target.has_value())
            targets.push_back(std::move(*target));
        else
            extent = SrvAnswerExtent::Truncated;
    }
    return SrvAnswerOf(std::move(targets), extent);
}

SrvLookupFault SrvFaultOf(SrvStatus status) noexcept
{
    return SrvStatusTable[static_cast<std::size_t>(status)].fault;
}

bool IsDialableSrvTarget(std::string_view host) noexcept
{
    if (host.empty() || host.size() > MaxHostNameChars)
        return false;
    return std::ranges::all_of(std::views::split(host, '.'), [](auto const label) {
        return IsHostNameLabel(std::string_view { label.begin(), label.end() });
    });
}

std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvAnswerOf(std::vector<SrvTarget> records, SrvAnswerExtent extent)
{
    // The root first, so the undialable count is only ever records published wrong.
    auto const notOffered = std::erase_if(records, [](SrvTarget const& target) { return IsRootSrvTarget(target.host); });
    auto const undialable =
        std::erase_if(records, [](SrvTarget const& target) { return !IsDialableSrvTarget(target.host); });
    if (!records.empty())
        return records;
    return std::unexpected { SrvLookupFailure { .fault = SrvAnswerExtentTable[static_cast<std::size_t>(extent)].faultIfEmpty,
                                                .undialable = static_cast<std::size_t>(undialable),
                                                .notOffered = static_cast<std::size_t>(notOffered) } };
}

std::string DescribeSrvLookupFailure(SrvLookupFailure const& failure)
{
    auto const plural = [](std::size_t count) {
        return count == 1 ? "" : "s";
    };
    auto clauses = std::vector<std::string> {};
    if (failure.notOffered != 0)
        clauses.push_back(std::format("{} root target{} (the domain says the service is decidedly not offered there)",
                                      failure.notOffered,
                                      plural(failure.notOffered)));
    if (failure.undialable != 0)
        clauses.push_back(std::format("{} target{} dropped as undialable", failure.undialable, plural(failure.undialable)));

    auto out = std::string { SrvLookupFaultName(failure.fault) };
    auto separator = std::string_view { ": " };
    for (auto const& clause: clauses)
    {
        out += separator;
        out += clause;
        separator = ", ";
    }
    return out;
}

} // namespace FastCache
