// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/TlsWrap.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace FastCache
{

AdminBindFailureReport DescribeToleratedAdminBindFailure(std::string_view address,
                                                         std::uint16_t port,
                                                         std::string_view error)
{
    return AdminBindFailureReport {
        .level = LogLevel::Warn,
        .message =
            std::format("fastcached: cannot bind metrics endpoint {}:{} ({}); continuing without /metrics and /healthz",
                        address,
                        port,
                        error),
    };
}

namespace
{

    /// Hard cap on the request head we will buffer before giving up. The admin
    /// surface only needs the request line and one header, so this is generous; it
    /// exists to stop a slow or hostile client from growing the buffer without bound.
    constexpr std::size_t MaxRequestBytes = 8192;

    Task<bool> WriteAll(ISocket* socket, std::string_view data)
    {
        if (data.empty())
            co_return true;
        auto const result = co_await socket->Write(AsBytes(data));
        co_return result.has_value() && *result == data.size();
    }

    /// Why reading one admin request head ended.
    ///
    /// ## Three states behind one `bool`, and a fourth that was not even behind it
    ///
    /// `ReadRequestHead` used to answer `bool ok`, and every falsy reason was answered
    /// `400 Bad Request` -- including the two in which the peer had **asked nothing**.
    /// The outcomes that were *silently served* rather than refused (`TooLarge`,
    /// `Truncated`) were not behind the bool at all: it came back **true** for them,
    /// which is why counting them as "four states behind a bool" understates it. A
    /// `bool` cannot express "not attempted"; it also cannot express "attempted, and
    /// what I have is a fragment".
    /// Chrome opens speculative *preconnect* sockets ahead of a navigation and may send
    /// on one long after opening it, so past `AdminHttpServer::RequestTimeout` a
    /// perfectly valid `GET /fleet` was answered `400`, intermittently, in the browser
    /// and never under `curl` -- which sends immediately, which is why every fixture in
    /// this tree passed ([#824](https://github.com/LASTRADA-Software/fastcached/issues/824)).
    ///
    /// This is the enum the repository's own rule asks for: *an outcome that can be "not
    /// attempted" is an enum, not a `bool`, and it is fixed at the seam rather than at
    /// the one call site that noticed.* The seam is this return value; the call site
    /// merely looks up the row.
    ///
    /// ## Why two of them are answered with silence
    ///
    /// `.agent/rules/wire-and-protocol.md` settles EOF as *"this peer has finished
    /// SENDING", not "this peer is gone"* -- and a server answers what is already
    /// determined while abandoning what is still pending. **On this surface** (that rule
    /// requires every measurement and every reading to name its own wire) a peer that
    /// sent no byte has determined nothing, so there is no reply it is owed. An
    /// abandoned preconnect is what a *healthy* browser produces many times a minute;
    /// answering it costs a real page a `400`.
    ///
    /// `Idle` and `PeerGone` are kept apart rather than folded into one "silent",
    /// because they are different facts about the peer -- a socket held open with
    /// nothing on it is a preconnect or a slowloris, a socket closed with nothing on it
    /// is an abandoned preconnect or a liveness probe -- and folding two states that
    /// happen to share today's disposition is exactly how this defect was built. What
    /// `PeerGone` *does* fold is EOF and an abortive close, deliberately: both mean
    /// there is no peer to answer and nowhere to send an answer.
    ///
    /// ## Why none of them is counted
    ///
    /// `.agent/rules/metrics-and-observability.md` requires a refusal's wire code and
    /// its counter to be one row, and warns that a refusal answered while nothing rises
    /// is a probed port that looks unused. It also states the limit that applies here:
    /// **not every refusal is an EVENT.** `Idle` and `PeerGone` are what ordinary
    /// browsers and ordinary probes do continuously, so a counter on them would be a
    /// series that never rests and buries whatever it was scraped to show. This is a
    /// decision, not an omission. The admin surface has no counters at all today, and
    /// giving it one for its two *non*-events would be the wrong first row.
    ///
    /// `Malformed` and `TooLarge` are a different case and are uncounted for a
    /// different reason: a rise in either WOULD mean something, and this surface has
    /// no counters at all to raise. That gap is its own work, not this change's.
    enum class AdminHeadOutcome : std::uint8_t
    {
        /// A request line arrived and parsed. The only outcome that is routed.
        Complete = 0,
        /// Not one byte arrived before the read deadline expired.
        Idle,
        /// The peer went away with the head unfinished (EOF, or an abortive close).
        PeerGone,
        /// Bytes arrived, the head never ended, and the read deadline expired.
        ///
        /// The second route to `TooLarge`'s defect, and the more reachable one: a
        /// prefix whose request line has arrived parses and routes, so a head cut
        /// off mid-way used to be SERVED with every header that had not arrived yet
        /// silently missing -- which on a credentialled route is the same spurious
        /// `401`. `RequestTimeout` is per READ and the head has no total budget, so
        /// this needs no oversize client at all.
        ///
        /// It is answered rather than closed, and that is the whole distinction the
        /// enum above is for. `Idle` and `PeerGone` are silent because the peer
        /// determined nothing -- EOF means *finished sending*, so what arrived is
        /// all there was. A DEADLINE says the opposite: the peer has not finished
        /// and this server gave up, which is exactly what `408` reports.
        Truncated,
        /// Bytes arrived and are not a request the server can act on -- either the
        /// line does not parse, or the head never ended and the peer stopped
        /// (`EndOfStream` mid-head). Both are `400`: an unfinished head is a bad
        /// request, not a slow one, so it is not `Truncated`'s `408`.
        Malformed,
        /// The byte cap was reached before the head ended.
        ///
        /// Not merely uncovered before -- *misanswered*. The read loop stopped at the cap
        /// and the truncated buffer was then parsed and **served as though complete**, so
        /// a client whose `Authorization` sat past byte 8192 was answered `401`, which
        /// reads as a wrong credential rather than as an oversize head.
        TooLarge,
        Last
    };

    /// What one head-read outcome is answered with.
    struct AdminHeadOutcomeRow
    {
        /// The outcome this row describes.
        AdminHeadOutcome outcome;
        /// Status line to answer with, or empty to close without answering at all.
        std::string_view status;
        /// The body that goes with `status`; empty wherever `status` is.
        std::string_view body;
        /// Why a rise on this outcome would, or would not, mean something.
        ///
        /// **A COLUMN and not a paragraph, so a new row cannot be written without
        /// answering.** Nothing sends this text; it exists to be a forcing function,
        /// exactly as `Protocol/SurfaceRefusal.hpp`'s `UncountedRefusal::rationale`
        /// does on the `0xFC` wire. The rule it serves is
        /// `.agent/rules/metrics-and-observability.md`'s: *deliberately uncounted must
        /// not be spelled like forgot*. Written as prose above the enum, that decision
        /// sat sixty lines from the rows it was about and asked the author of a seventh
        /// outcome for a status and a body and nothing else.
        ///
        /// This surface reaches no counter today -- `ServeAdminHttp` takes
        /// `IMetricsSink const*` -- and the rationale is the half that has to travel
        /// with the row regardless, because it is what a counter would later be added
        /// against. It is also outside `check-worker-refusals-counted.cmake`'s glob,
        /// which keys on `EncodeErrorReply`, so nothing scans it: the column is the
        /// only thing standing where that scan would.
        std::string_view rationale;
    };

    /// The disposition of every head-read outcome, in enumerator order.
    ///
    /// File-local, like every other part of this parse: nothing outside this file names
    /// an outcome, and `AdminHeader` is public only because `AdminRequest::Header()`
    /// takes one. A table rather than a chain of `if`s for this codebase's usual reason, plus one
    /// specific to it: an empty `status` is how *no response is owed* is spelled, so the
    /// silent outcomes are visible beside the answered ones instead of being a `return`
    /// somebody has to find.
    inline constexpr EnumTable<AdminHeadOutcome, AdminHeadOutcomeRow> AdminHeadOutcomeTable {
        // Never looked up -- a complete head is routed instead. Present because the
        // table is indexed by the enum and a missing row would be a silent zero.
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::Complete,
                              .status = {},
                              .body = {},
                              .rationale = "not an outcome: a complete head is routed, never dispatched here" },
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::Idle,
                              .status = {},
                              .body = {},
                              .rationale = "a rise would mean nothing: an abandoned browser preconnect is what a "
                                           "HEALTHY client produces many times a minute, so the series never rests" },
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::PeerGone,
                              .status = {},
                              .body = {},
                              .rationale = "a rise would mean nothing, for Idle's reason: a peer leaving without "
                                           "asking is ordinary, and it is the same client behaviour observed one "
                                           "way rather than the other" },
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::Truncated,
                              .status = "408 Request Timeout",
                              .body = "request timed out\n",
                              .rationale = "a rise WOULD mean something -- clients cut off mid-head point at a "
                                           "deadline set too tight or a network losing segments -- but this surface "
                                           "reaches no sink; see #828" },
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::Malformed,
                              .status = "400 Bad Request",
                              .body = "bad request\n",
                              .rationale = "a rise WOULD mean something (a broken client, or a probe on the wrong "
                                           "port), but this surface reaches no sink; see #828" },
        AdminHeadOutcomeRow { .outcome = AdminHeadOutcome::TooLarge,
                              .status = "431 Request Header Fields Too Large",
                              .body = "request header fields too large\n",
                              .rationale = "a rise WOULD mean something (a client whose headers outgrew the cap), "
                                           "but this surface reaches no sink; see #828" },
    };
    static_assert(RowsInEnumeratorOrder(AdminHeadOutcomeTable, &AdminHeadOutcomeRow::outcome));

    /// A row that says nothing must carry nothing.
    ///
    /// Not a restatement of the table: it is the one relation between its two columns.
    /// A body beside an empty status is a sentence nothing would ever send, which is how
    /// somebody adding a silent outcome writes down an answer that is then dropped.
    static_assert(std::ranges::all_of(AdminHeadOutcomeTable,
                                      [](AdminHeadOutcomeRow const& row) noexcept {
                                          return !row.status.empty() || row.body.empty();
                                      }),
                  "a head outcome answered with silence must carry no body");

    /// Every row states its counter claim. An empty `rationale` is the spelling of
    /// *forgot*, which is the one thing the column exists to make impossible.
    static_assert(std::ranges::all_of(AdminHeadOutcomeTable,
                                      [](AdminHeadOutcomeRow const& row) noexcept { return !row.rationale.empty(); }),
                  "every head outcome must say whether counting it would mean anything");

    /// Parsed HTTP request head, and why reading it ended. Only `Complete` carries
    /// a method and a target; every other outcome carries nothing but itself.
    ///
    /// Owns each field as its own `std::string` rather than keeping the raw head
    /// and viewing into it. That is not redundancy: an earlier version held the
    /// buffer here and `std::move`d it into this struct *in the same aggregate
    /// initializer* that read `string_view`s over it -- and aggregate
    /// initialization is evaluated left to right, so the move happened first and
    /// every view was already dangling.
    ///
    /// It worked on libstdc++ and returned empty strings on libc++, for a reason
    /// worth keeping: whether the move invalidates the views depends entirely on
    /// whether the buffer was heap-allocated. libc++'s small-string capacity is 22
    /// bytes and `GET /nope HTTP/1.1\r\n\r\n` is exactly 22, so that request was
    /// copied inline and its views broke, while a 26-byte POST heap-allocated and
    /// survived. libstdc++'s capacity is 15, so both allocated and neither broke.
    /// A bug that is invisible on one standard library and depends on request
    /// length on the other is not one to leave to a careful reader.
    struct RequestHead
    {
        std::string method {};
        std::string target {};
        /// One slot per `AdminHeaderTable` row, in its order.
        EnumTable<AdminHeader, std::string> headers {};
        /// Why the read ended; see `AdminHeadOutcome`.
        ///
        /// Defaulted to `Malformed` rather than to the enum's zero. `Complete` is
        /// `0`, so an aggregate that omits this member would value-initialize to
        /// *the request parsed* -- a default that turns forgetting into serving.
        /// The `bool ok { false }` this replaces defaulted the safe way, and the
        /// property is worth keeping across the change rather than rediscovering.
        AdminHeadOutcome outcome { AdminHeadOutcome::Malformed };
    };

    /// Case-insensitive comparison, for a header name.
    ///
    /// HTTP field names are case-insensitive and real clients disagree about the
    /// spelling -- curl sends `Authorization`, some proxies lower-case everything.
    /// A byte comparison here would make the credential work for some callers and
    /// not others, which is the shape of bug that gets diagnosed as "the token is
    /// wrong".
    /// @param a One name.
    /// @param b The other.
    /// @return Whether they name the same header.
    [[nodiscard]] bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) noexcept {
            auto const lower = [](char c) noexcept {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(x) == lower(y);
        });
    }

    /// Trim leading and trailing spaces and tabs.
    /// @param text The field value.
    /// @return The value without its optional surrounding whitespace.
    [[nodiscard]] std::string_view TrimFieldValue(std::string_view text) noexcept
    {
        constexpr std::string_view Blank = " \t";
        auto const first = text.find_first_not_of(Blank);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Blank) - first + 1);
    }

    /// Read the whole request head — up to the blank line that ends the headers,
    /// or the byte cap — then split its first line into method and target and pick
    /// out the one header this surface reads.
    ///
    /// The headers were already **consumed** before any of them was used, and that
    /// is the point: this used to stop at the first CRLF, and a request split across
    /// TCP segments then left `Host:` and `Connection: close` unread in the receive
    /// queue. Closing a socket with unread received data makes the kernel send **RST
    /// instead of FIN**, so the client sees `Connection reset by peer` rather than a
    /// clean end of response — intermittently, depending only on how the request
    /// happened to be segmented.
    ///
    /// That is not a fixture problem. Every consumer of this endpoint is a monitoring
    /// probe, and an endpoint whose job is to report that a worker is healthy must not
    /// be the thing that looks unhealthy. Consuming the request before answering it is
    /// also simply what a correct HTTP server does.
    ///
    /// A client that never sends the blank line is bounded by the accepted socket's
    /// receive timeout, which is what a request timeout is for -- and if it never sent
    /// a byte at all, that bound expiring is `Idle` and is answered with silence
    /// rather than with a `400` (#824). A head that exceeds the cap is answered `431`
    /// and then closed with whatever is left unread.
    ///
    /// **The reset is real, it is PRE-EXISTING, and it does not eat the answer.**
    /// Closing with unread data in the receive queue makes the kernel send RST rather
    /// than FIN, and an RST can discard what this server just wrote. A reviewer
    /// predicted that would destroy this `431`. Measured instead -- Linux loopback,
    /// one daemon built from this branch and one from master (`5e80adcd`), six client
    /// shapes each, the client sending its whole request before reading:
    ///
    /// | request                          | master        | this branch   |
    /// |----------------------------------|---------------|---------------|
    /// | unterminated 9 KB head           | `200`, RST    | `431`, RST    |
    /// | unterminated 60 KB head          | `200`, RST    | `431`, RST    |
    /// | complete head + 60 KB unread body| `200`, RST    | `200`, RST    |
    /// | complete head, nothing unread    | `200`, no RST | `200`, no RST |
    ///
    /// Every response arrived INTACT, on both trees, with the reset observed on the
    /// following `recv`. Delaying the client's first read by 0.5 s, 1 s, 2 s and 5 s
    /// changed nothing: the bytes are queued on the client before the reset reaches
    /// it. The last row is the control -- no unread data, no reset -- which is what
    /// licenses reading the other three as the hazard rather than as noise.
    ///
    /// **So the change moves WHICH response is at risk, not WHETHER one is.** Master
    /// already answered `200 OK` and closed over ~52 KB of unread head, drawing the
    /// identical RST; the third row is a path that predates this branch entirely and
    /// behaves the same on both. Draining first is therefore not a fix for a hazard
    /// this introduced -- it would be a byte budget granted to exactly the peer least
    /// entitled to one, and it is not taken.
    ///
    /// **Conditions, because they are the measurement's and not a guarantee:** Linux
    /// loopback, and a client that finishes SENDING before it reads. The bash-versus-
    /// python result quoted elsewhere in this file is a DIFFERENT shape -- there the
    /// client writes LATE, after the server has closed, and its own write is what
    /// draws the RST onto a response already sitting unread. Reading that measurement
    /// as evidence about this one is the conflation `AGENT.md` warns about under
    /// "a figure is a quantity UNDER CONDITIONS"; the two shapes are not comparable.
    ///
    /// The change of ANSWER is deliberate and is not the reset: on master a 9 KB and a
    /// 60 KB head to `/healthz` are both answered `200 OK`, from a head this server
    /// only read the first 8,192 bytes of. That is the defect. A route reading no
    /// headers survives it by luck; `/fleet` reads a credential and does not.
    Task<RequestHead> ReadRequestHead(ISocket* socket)
    {
        std::string buffer;
        bool sawHeadEnd = false;
        while (!sawHeadEnd && buffer.size() < MaxRequestBytes)
        {
            std::array<std::byte, 1024> chunk {};
            auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
            if (!result.has_value())
            {
                // Answered here rather than remembered in a local the code after the
                // loop reads: whether this was a deadline or a departure is known
                // where the read failed and nowhere else, and a variable carrying it
                // out would need an invariant ("only the FIRST read can leave the
                // buffer empty") that nothing checks.
                //
                // The request deadline is armed as `SO_RCVTIMEO` on the accepted
                // socket (`BlockingListener::SetTimeouts` -- a concrete-type call,
                // with no `IListener` seam, which is why a reactor-backed admin
                // socket would never produce `Idle` at all), so it reaches this loop
                // as a read *error* rather than as a signal of its own.
                // `IsDeadlineExpiry` is what knows that the two platforms spell it
                // differently.
                //
                // Three outcomes, and the buffer decides only the last two: a peer
                // that is GONE is answered by nobody whether it managed to say
                // something first, while a peer this server gave WAITING for is
                // told so -- silently if it had asked nothing, `408` if it had
                // begun. Falling through to the parse here is what served a
                // truncated head, which is `TooLarge`'s defect reached without an
                // oversize client.
                if (!IsDeadlineExpiry(result.error().code))
                    co_return RequestHead { .outcome = AdminHeadOutcome::PeerGone };
                co_return RequestHead { .outcome = buffer.empty() ? AdminHeadOutcome::Idle : AdminHeadOutcome::Truncated };
            }
            if (*result == 0)
                break;
            // Rescan from three bytes before the freshly-appended region: the
            // terminator is four bytes and may straddle a chunk boundary in any of
            // three ways. That keeps the total scan linear rather than re-reading
            // the whole buffer each iteration; append in one call, not byte-by-byte.
            auto const scanFrom = buffer.size() < 3 ? 0 : buffer.size() - 3;
            buffer.append(reinterpret_cast<char const*>(chunk.data()), *result);
            sawHeadEnd = buffer.find("\r\n\r\n", scanFrom) != std::string::npos;
        }

        // Nothing was received, so nothing was asked and nothing is owed. Reaching
        // here with an empty buffer is the EOF path; the deadline path returned
        // above. This is the whole of #824: below, `eol == npos` used to fold an
        // empty buffer in with a genuinely bad request line and answer both `400`.
        if (buffer.empty())
            co_return RequestHead { .outcome = AdminHeadOutcome::PeerGone };

        // **`sawHeadEnd` decides THAT a refusal happens; the byte cap only decides
        // WHICH.** A head with no terminating blank line is a PREFIX of a request,
        // never a request, and parsing one is what used to happen -- silently, which
        // is worse than any refusal: the request line is near the front and always
        // survives truncation, so the prefix routed and answered while every field
        // after the cut was simply gone. A browser whose `Authorization` fell beyond
        // it was told `401`, which reads as a wrong credential.
        //
        // The guard used to be `!sawHeadEnd && size >= MaxRequestBytes`, which closed
        // the cap route and left the EOF route wide open -- and the EOF route needs
        // no oversize client at all, just a peer that half-closes after its request
        // line. Two exits, one condition: whichever way the loop ended, an unfinished
        // head is refused here.
        //
        // **This is the EOF rule applied more carefully than it was.**
        // `.agent/rules/wire-and-protocol.md` says a server answers what is already
        // DETERMINED, and the tempting reading -- EOF means *finished sending*, so
        // serve what arrived -- is what put a control case in this file asserting
        // exactly the bug. An HTTP head with no blank line determines nothing: this
        // server does not know which headers the peer would have sent, so there is
        // no reply it can be right about. `PING` half-closed is a complete command;
        // this is a sentence cut off mid-word. A COMPLETE head half-closed after is
        // still served, and that is the control that belongs beside this.
        if (!sawHeadEnd)
            co_return RequestHead { .outcome = buffer.size() >= MaxRequestBytes ? AdminHeadOutcome::TooLarge
                                                                                : AdminHeadOutcome::Malformed };

        // Unreachable by construction, and kept anyway. `sawHeadEnd` is only ever set
        // by finding `"\r\n\r\n"`, so reaching here guarantees a `"\r\n"` exists and
        // this arm cannot fire -- it is NOT a live `Malformed` route, and a reader
        // counting them should not count it. It stays because the cost of the two
        // outcomes is wildly asymmetric: if that invariant is ever broken by an edit
        // to the loop above, `npos` here becomes the LENGTH of the `string_view`
        // below, which is a read far past the buffer rather than a wrong status code.
        auto const eol = buffer.find("\r\n");
        if (eol == std::string::npos)
            co_return RequestHead { .outcome = AdminHeadOutcome::Malformed };
        std::string_view const line { buffer.data(), eol };

        auto const sp1 = line.find(' ');
        if (sp1 == std::string_view::npos)
            co_return RequestHead { .outcome = AdminHeadOutcome::Malformed };
        auto const rest = line.substr(sp1 + 1);
        auto const sp2 = rest.find(' ');
        auto const target = sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);

        // Walk the remaining field lines for every header this surface reads.
        // Stopping at the blank line rather than scanning the whole buffer keeps a
        // body -- which no admin route has, but which a hostile client may send --
        // from being mistaken for a header.
        //
        // Every line is offered to every row, and the walk does **not** stop at the
        // first match. It did, back when there was one readable header, and that is
        // a shape which stays correct exactly until a second row is added -- at
        // which point whichever header the client happened to send first is the only
        // one that arrives, so a chart's `If-None-Match` would vanish whenever the
        // browser put `Authorization` above it.
        EnumTable<AdminHeader, std::string> headers {};
        for (auto cursor = eol + 2; cursor < buffer.size();)
        {
            auto const lineEnd = buffer.find("\r\n", cursor);
            if (lineEnd == std::string::npos || lineEnd == cursor)
                break;
            std::string_view const field { buffer.data() + cursor, lineEnd - cursor };
            if (auto const colon = field.find(':'); colon != std::string_view::npos)
                for (auto const& row: AdminHeaderTable)
                    if (EqualsIgnoringCase(field.substr(0, colon), row.name))
                    {
                        headers[static_cast<std::size_t>(row.header)] =
                            std::string { TrimFieldValue(field.substr(colon + 1)) };
                        break;
                    }
            cursor = lineEnd + 2;
        }

        co_return RequestHead { .method = std::string { line.substr(0, sp1) },
                                .target = std::string { target },
                                .headers = std::move(headers),
                                .outcome = AdminHeadOutcome::Complete };
    }

    /// Statuses whose responses carry no content at all.
    ///
    /// A property of the status rather than a flag on the response, so a route that
    /// answers `304` cannot get it wrong by forgetting to set one. RFC 9110 SS15.4.5
    /// forbids content on a `304`, and SS8.6 forbids a `Content-Length` that does not
    /// equal what a `200` would have sent -- a client that read one and then found the
    /// connection closed would report a truncated response rather than a cache hit.
    constexpr std::array<std::string_view, 2> BodylessStatuses { "204 No Content", "304 Not Modified" };

    /// Whether a status forbids a body.
    /// @param status The status line without its version.
    /// @return True when neither content nor its headers may be written.
    [[nodiscard]] bool CarriesNoContent(std::string_view status) noexcept
    {
        return std::ranges::any_of(BodylessStatuses, [status](auto const& known) noexcept { return known == status; });
    }

    /// By value, not by reference: this is a coroutine, so a reference parameter
    /// is one whose referent may be gone by the time the first `co_await`
    /// resumes -- the hazard `ServeAdminHttp`'s own signature already avoids.
    Task<bool> WriteResponse(ISocket* socket, AdminResponse response)
    {
        auto const bodyless = CarriesNoContent(response.status);
        auto head = std::format("HTTP/1.1 {}\r\n", response.status);
        if (!bodyless)
            head += std::format("Content-Type: {}\r\nContent-Length: {}\r\n", response.contentType, response.body.size());
        head += "Connection: close\r\n";
        for (auto const& extra: response.extraHeaders)
            head += std::format("{}\r\n", extra);
        head += "\r\n";
        if (!co_await WriteAll(socket, head))
            co_return false;
        if (bodyless)
            co_return true;
        co_return co_await WriteAll(socket, response.body);
    }

    /// The refusals this server answers without consulting a route.
    ///
    /// A table so that the three share one spelling of "status, type, body" with
    /// every route's own answer, rather than three `WriteResponse` calls whose
    /// argument order is checked by nothing.
    [[nodiscard]] AdminResponse PlainRefusal(std::string_view status, std::string_view body)
    {
        return AdminResponse { .status = status, .contentType = "text/plain", .body = std::string { body } };
    }

} // namespace

Task<void> ServeAdminHttp(ISocket* socket,
                          IMetricsSink const* metrics,
                          AdminHttpServer::SnapshotProvider snapshotProvider,
                          std::span<AdminRoute const> routes)
{
    // TLS terminates here rather than in the accept loop, so a handshake failure
    // costs the same detached task a slow request does and never the loop. A
    // plaintext socket's implementation is a no-op, so this is unconditional.
    if (!co_await socket->HandshakeIfNeeded())
        co_return;

    auto const request = co_await ReadRequestHead(socket);
    if (request.outcome != AdminHeadOutcome::Complete)
    {
        // The disposition is read off `AdminHeadOutcomeTable`, never decided here.
        // A `400` spelled at this one call site is what collapsed four outcomes into
        // one answer, and the fix belongs at the seam -- so this branch has no
        // knowledge of which outcome deserves what, and an outcome added later
        // arrives with its answer already attached.
        auto const& row = AdminHeadOutcomeTable[static_cast<std::size_t>(request.outcome)];
        // An empty status is the table's spelling of *no response is owed*: the peer
        // asked nothing, so it is closed without being answered, which is what every
        // mainstream HTTP server does with an abandoned preconnect.
        if (!row.status.empty())
            (void) co_await WriteResponse(socket, PlainRefusal(row.status, row.body));
        co_return;
    }
    if (request.method != "GET")
    {
        // RFC 9110 §15.5.6 makes `Allow` a MUST on a 405, and it is not ceremony:
        // without it a client is told the request was wrong and not which half of
        // it, so a probe pointed at the right path with the wrong verb reads
        // identically to one pointed at a path this server does not serve.
        (void) co_await WriteResponse(socket,
                                      AdminResponse { .status = "405 Method Not Allowed",
                                                      .contentType = "text/plain",
                                                      .body = "method not allowed\n",
                                                      .extraHeaders = { "Allow: GET" } });
        co_return;
    }

    // Strip any query string so `/metrics?foo=bar` still routes.
    std::string_view const target { request.target };
    auto const questionMark = target.find('?');
    auto const path = target.substr(0, questionMark);
    auto const query = questionMark == std::string_view::npos ? std::string_view {} : target.substr(questionMark + 1);

    if (path == "/metrics")
    {
        auto const body = RenderPrometheus(*metrics, snapshotProvider());
        (void) co_await WriteResponse(
            socket, AdminResponse { .status = "200 OK", .contentType = "text/plain; version=0.0.4", .body = body });
        co_return;
    }
    if (path == "/healthz")
    {
        (void) co_await WriteResponse(socket, PlainRefusal("200 OK", "OK\n"));
        co_return;
    }

    AdminRequest routeRequest { .path = path, .query = query };
    for (auto const& row: AdminHeaderTable)
    {
        auto const slot = static_cast<std::size_t>(row.header);
        routeRequest.headers[slot] = request.headers[slot];
    }

    // Kinds in enumerator order, so `Exact` is tried against every route before
    // `Prefix` is tried against any: which route answers a path is then a property
    // of the table rather than of the order a caller happened to build its vector in.
    for (auto const kind: std::views::iota(std::size_t { 0 }, EnumeratorCount<AdminRouteMatch>))
        for (auto const& route: routes)
        {
            if (static_cast<std::size_t>(route.match) != kind || !route.handler)
                continue;
            auto const hit = route.match == AdminRouteMatch::Exact ? route.path == path : path.starts_with(route.path);
            if (!hit)
                continue;
            (void) co_await WriteResponse(socket, route.handler(routeRequest));
            co_return;
        }

    (void) co_await WriteResponse(socket, PlainRefusal("404 Not Found", "not found\n"));
    co_return;
}

AdminHttpServer::AdminHttpServer(IListener& listener,
                                 IMetricsSink const& metrics,
                                 SnapshotProvider snapshotProvider,
                                 ILogger& logger,
                                 std::vector<AdminRoute> routes,
                                 TlsContext* tls) noexcept:
    _listener { listener },
    _metrics { metrics },
    _snapshotProvider { std::move(snapshotProvider) },
    _logger { logger },
    _routes { std::move(routes) },
    _tls { tls }
{
}

/// Serve one accepted admin connection as a detached coroutine. Owns the
/// socket (so the connection outlives the accept loop iteration) and
/// decrements the in-flight counter on exit. Free function so the accept
/// loop can spawn it without capturing this — `inFlight` is passed by
/// pointer with a lifetime that exceeds every spawned task (the
/// AdminHttpServer's destructor blocks elsewhere).
static DetachedTask ServeAdminConnection(std::unique_ptr<ISocket> socket,
                                         IMetricsSink const* metrics,
                                         AdminHttpServer::SnapshotProvider snapshotProvider,
                                         std::span<AdminRoute const> routes,
                                         std::atomic<std::size_t>* inFlight)
{
    co_await ServeAdminHttp(socket.get(), metrics, std::move(snapshotProvider), routes);
    socket->Close();
    inFlight->fetch_sub(1, std::memory_order_acq_rel);
}

Task<void> AdminHttpServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll-timeout on the listening socket is how we wake to observe
            // Shutdown() on POSIX (where Close() does not unblock a parked
            // accept()); it is not a real failure, so loop and re-check the flag.
            if (IsDeadlineExpiry(accepted.error().code))
                continue;
            _logger.Logf(LogLevel::Debug, "admin: accept loop ended ({})", accepted.error().ToString());
            co_return;
        }
        // Spawn the handler as a detached coroutine so a slow or hostile
        // client cannot tie up the accept loop and DoS /metrics and /healthz
        // for everyone else. The in-flight cap bounds peak memory: an
        // attacker that opens MaxConcurrentRequests slow sockets gets a
        // 503-and-close on the next accept rather than queueing in the
        // kernel until the scraper times out.
        auto const before = _inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (before >= MaxConcurrentRequests)
        {
            _inFlight.fetch_sub(1, std::memory_order_acq_rel);
            // Written through the one response writer rather than as a literal
            // whose Content-Length was maintained by hand: the hand-counted one
            // said 32 for a 32-byte body and would have gone on saying 32 for the
            // first person who reworded it, which a client reads as a truncated
            // response or a hung connection rather than as a typo.
            //
            // Plaintext, and deliberately: this refusal happens before the
            // connection has a handler, so nothing has driven a TLS handshake and
            // a TLS client would not read it either way. It costs the attacker a
            // socket and tells an honest client something.
            (void) co_await WriteResponse((*accepted).get(),
                                          PlainRefusal("503 Service Unavailable", "admin: too many concurrent reqs\n"));
            (*accepted)->Close();
            continue;
        }
        ServeAdminConnection(WrapTls(std::move(*accepted), _tls), &_metrics, _snapshotProvider, _routes, &_inFlight);
    }
    co_return;
}

void AdminHttpServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
    // Detached request coroutines may still be in flight. They borrow the
    // metrics sink, snapshot provider and routes held on this object, so we must
    // wait for them to drain before letting Shutdown return — otherwise an admin
    // handler that suspends on a slow write could touch freed members after
    // the server object is destroyed. Bounded by the per-request socket I/O
    // timeout, plus a generous spin-cap so a stuck handler cannot block
    // forever -- through the one shared `DrainWithin`, which measures that ceiling
    // rather than counting polls towards it (#452).
    if (DrainWithin([this] { return _inFlight.load(std::memory_order_acquire) > 0; }) == DrainResult::Ceiling)
        _logger.Logf(LogLevel::Error,
                     "admin: {} request(s) did not finish within the stop ceiling",
                     _inFlight.load(std::memory_order_acquire));
}

} // namespace FastCache
