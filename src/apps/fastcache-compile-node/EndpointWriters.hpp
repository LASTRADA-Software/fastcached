// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <string_view>

namespace FastCache::Node
{

/// Every function on a framed endpoint that may put bytes on a connection.
///
/// **A PRIVATE enum: nothing transmits it, nothing persists it, and it must not gain
/// an explicit `= N`** -- a value on a private enum asserts a contract that does not
/// exist. It is an argument each write names, and its ordinals are this build's alone.
///
/// ## Why a table for four functions
///
/// `FrameEndpoint`'s connection loop has an **exactly-one-writer** property: what goes
/// out on a connection, and in what order, is decided by one statement sequence. The
/// property was *structural* -- the loop was the only writer because it was the only
/// thing that wrote -- and structural is a property nothing tells you about
/// ([#675](https://github.com/LASTRADA-Software/fastcached/issues/675)). The loop also
/// sits at clang-tidy's cognitive-complexity ceiling, so the next lane that has to
/// reduce it meets a function that punishes the obvious way of doing so: extract the
/// nearest block and, as likely as not, you have extracted the write. The property is
/// then an agreement between two functions rather than a fact about one, and what it
/// costs is an interleaved partial write, which on this wire is a desync rather than an
/// error -- a client reading a length out of the middle of somebody else's frame.
///
/// So a write NAMES which of the sanctioned writers it is. A fifth writer is a fifth
/// row carrying its own reason, which is a thing a reviewer sees; it is not a
/// `WriteAll` call that appeared in a helper, which is not. That is a refusal by ROW
/// where there was previously only a refusal by absence, and the two are the same
/// answer only while nothing else is consulted.
///
/// ## What this is NOT
///
/// It is **declared, not enforced.** Nothing here stops two of these writing at once,
/// and nothing here stops a new helper naming `Loop` -- the scan that reads this table
/// cannot say which FUNCTION a call sits in without parsing bodies, and it does not.
/// Enforcing it needs a write-side claim on `ISocket`, the mirror of
/// `Detail::ClaimReadSlot`, folded into the operation so no site has a line to forget:
/// that is a `Net/` change, it needs a lifetime shared with a detached pulse task, and
/// it is [#1218](https://github.com/LASTRADA-Software/fastcached/issues/1218), which
/// carries the two shapes it could take and why the choice is worth making before any
/// code. Stating the gap plainly is the point -- a guard described as stronger than it
/// is gets trusted for the case it does not cover.
///
/// Exactly one of these is CONCURRENT with another: `Pulse` writes while the loop is
/// suspended inside `IFrameResponder::Answer`, which is the one window in which the
/// loop writes nothing. `ReclaimFromPulse` is where the socket comes back, and it runs
/// before every write that follows the answer.
enum class EndpointWriter : std::uint8_t
{
    Loop,          ///< `ServeConnection`'s own statement sequence.
    DeferredSweep, ///< `ExplainIfSwept`.
    Pulse,         ///< `PulseProgress`.
    AtCapacity,    ///< `RefuseAtCapacity`.

    Last, ///< Enumerator count; not a writer.
};

/// One sanctioned writer: which function it is, and why it may write at all.
struct EndpointWriterRow
{
    EndpointWriter writer;      ///< Which one this row describes.
    std::string_view name;      ///< The enumerator's own spelling, as a call site writes it.
    std::string_view function;  ///< The function that does the writing.
    std::string_view rationale; ///< Why the loop cannot do this itself.
};

/// The writers this endpoint has, in enumerator order.
///
/// The `rationale` is a forcing function rather than a dead field: a fifth writer that
/// cannot be given one is a fifth writer that should not exist, and the answer is to
/// hand the bytes back to the loop instead.
inline constexpr EnumTable<EndpointWriter, EndpointWriterRow> EndpointWriterTable {
    { { .writer = EndpointWriter::Loop,
        .name = "Loop",
        .function = "ServeConnection",
        .rationale = "The connection loop itself, and the default answer. Every refusal decided "
                     "from a header and every reply leaves by this statement sequence, which is "
                     "what makes their order a fact rather than an agreement." },
      { .writer = EndpointWriter::DeferredSweep,
        .name = "DeferredSweep",
        .function = "ExplainIfSwept",
        .rationale = "The one place a deferred sweep is observed. The loop holds several writes "
                     "and a check-whether-you-were-swept rule spread over them is a rule to "
                     "forget at the next one somebody adds, so the question is asked once, on "
                     "the way out of the responder, and answers with a write of its own." },
      { .writer = EndpointWriter::Pulse,
        .name = "Pulse",
        .function = "PulseProgress",
        .rationale = "The only CONCURRENT writer, and only while the loop is suspended inside "
                     "Answer, which is the one window in which the loop writes nothing. It "
                     "writes and never reads, the mirror of WatchPeer's rule; ReclaimFromPulse "
                     "is where the socket comes back, before any reply." },
      { .writer = EndpointWriter::AtCapacity,
        .name = "AtCapacity",
        .function = "RefuseAtCapacity",
        .rationale = "A connection that never reaches the loop. Its own task rather than a "
                     "write in the accept loop, so refusing never parks that loop on a client "
                     "which is not reading." } }
};

static_assert(RowsInEnumeratorOrder(EndpointWriterTable, [](EndpointWriterRow const& row) { return row.writer; }));

} // namespace FastCache::Node
