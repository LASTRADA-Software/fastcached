// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliValue.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

/// What one command invocation concluded.
///
/// **One code per outcome rather than success-or-failure, because one exit code cannot
/// answer several questions.** How many there are is `OutcomeTable`'s to say, and nothing
/// else states it: the help renders the table, and `ctest -R cli-exit-code-docs` holds the
/// operator page's listing to it. The tree has already paid for the collapsed version: one status
/// answering *did the file parse*, *would it start* and *would it bind* was three
/// fixtures leaning on the same number, and it stopped being able to carry all three
/// the moment one of them changed meaning.
///
/// The pairs that must never share a code:
///   - `Negative` against `Unreachable` -- a cache miss and a dead daemon. A script
///     that retries on one and gives up on the other cannot be written if they agree.
///   - `Refused` against `Protocol` -- the server said no, versus the server said
///     something this client could not read. Different people fix those.
///   - `Usage` against everything -- the operator's mistake, not the system's.
///   - `Local` against `Refused` -- this machine could not do it, versus the server
///     declined. One remedy is here (redirect the output, use another terminal) and the
///     other is at the server, and a script told `refused` goes to the wrong one.
///
/// A private enum; only the `code` column below is a published contract.
enum class Outcome : std::uint8_t
{
    Affirmative, ///< Answered, and the answer is yes / here it is.
    Negative,    ///< Answered, and the answer is no: a miss, an absent key, a false predicate.
    Usage,       ///< The command line was wrong. Nothing was sent.
    Unreachable, ///< Nothing answered, or the connection failed.
    Refused,     ///< The server answered and declined.
    Protocol,    ///< Bytes arrived that this client cannot read as a reply.
    Local,       ///< This machine could not carry the command out; the server is not implicated.
    Last,
};

/// One outcome's exit code and what it means.
struct OutcomeSpec
{
    Outcome outcome;          ///< The enumerator this row describes.
    int code;                 ///< The process exit code. A published contract.
    std::string_view name;    ///< Stable lower-case name, for logs and `--help`.
    std::string_view meaning; ///< One line an operator can act on.
};

/// The outcomes, one row per enumerator, in enumerator order.
///
/// `2` is usage for the reason every other binary in this tree uses it: `1` is what a
/// supervisor reads as *it ran and then died*, so a refusal must not claim it. Here
/// `1` is load-bearing in the other direction -- it is the *answer* `no`, which is a
/// successful exchange -- so nothing else may take it.
inline constexpr EnumTable<Outcome, OutcomeSpec> OutcomeTable { {
    { .outcome = Outcome::Affirmative, .code = 0, .name = "ok", .meaning = "the command was answered" },
    { .outcome = Outcome::Negative,
      .code = 1,
      .name = "no",
      .meaning = "the command was answered and the answer is no (a miss, or no such key)" },
    { .outcome = Outcome::Usage, .code = 2, .name = "usage", .meaning = "the command line was wrong; nothing was sent" },
    { .outcome = Outcome::Unreachable,
      .code = 3,
      .name = "unreachable",
      .meaning = "the server could not be reached, or the connection failed" },
    { .outcome = Outcome::Refused, .code = 4, .name = "refused", .meaning = "the server answered and declined" },
    { .outcome = Outcome::Protocol,
      .code = 5,
      .name = "protocol",
      .meaning = "the reply could not be read; the peer may not be a fastcached" },
    { .outcome = Outcome::Local,
      .code = 6,
      .name = "local",
      .meaning = "this machine could not carry the command out; the server is not implicated" },
} };

static_assert(RowsInEnumeratorOrder(OutcomeTable, &OutcomeSpec::outcome),
              "OutcomeTable must hold one row per Outcome, in enumerator order");

/// The row describing @p outcome.
/// @param outcome The outcome.
/// @return Its row; never null for a value below `Last`.
[[nodiscard]] OutcomeSpec const* DescriptorOf(Outcome outcome) noexcept;

/// The process exit code for @p outcome.
/// @param outcome The outcome.
/// @return Its exit code.
[[nodiscard]] int ExitCodeOf(Outcome outcome) noexcept;

/// What one command produced.
///
/// `value` goes to stdout in the chosen format; `advisories` go to **stderr**, in every
/// format including the human one.
///
/// **That split is deliberate and it is what keeps all five formats parseable.** An
/// advisory is a remark *about* the answer -- which stats source answered, that a value
/// came back as bytes rather than text, that a key exists but has no expiry. Folding it
/// into stdout would either corrupt the document or force every format to grow a
/// metadata envelope that `jq` users then have to reach through. Provenance a *machine*
/// needs is different and is carried as an ordinary field of `value`, which is why
/// `stats` reports its source on stdout rather than only in a remark.
struct Answer
{
    Value value {};                           ///< The answer, for stdout.
    std::vector<std::string> advisories {};   ///< Remarks about the answer, for stderr.
    Outcome outcome { Outcome::Affirmative }; ///< What it concluded.

    /// Bytes to write to stdout verbatim, instead of rendering `value`.
    ///
    /// Set only when the operator asked for the bytes with `--raw`. It is the one
    /// escape from the value model, and it is a deliberate one: a cached value is an
    /// arbitrary byte string, and piping one to a file is an ordinary thing to want.
    /// Every other path classifies bytes as text or base64 rather than emitting them,
    /// so this is where "I know what I am doing" is spelled.
    std::optional<std::string> rawPayload {};

    /// The fleet section `value`'s columns belong to, when they have scales to look up.
    ///
    /// **The PRODUCER states this; nothing infers it.** `CellFormat`'s own header says a
    /// consumer reads a column's scale off the column NAME, which it already has -- so what
    /// is missing at a render site is not the scale but which section's column tables to ask,
    /// and only the verb that fetched the table knows that. Inferring it from the verb name
    /// and its operand at the render site would put fleet knowledge in `main`, and it would
    /// be one more place to forget (#1488).
    ///
    /// Engaged only for a table whose columns are the leader's. Disengaged means "these
    /// columns have no scales", which is the truth for every other verb and is why the human
    /// renderer must not go looking on its own.
    std::optional<Distributed::FleetSection> columnScales {};
};

/// An answer carrying nothing but an outcome and one remark.
/// @param outcome What was concluded.
/// @param advisory The remark; may be empty.
/// @return The answer.
[[nodiscard]] Answer Concluded(Outcome outcome, std::string advisory = {});

/// An answer carrying a value.
/// @param value The answer.
/// @param outcome What was concluded.
/// @return The answer.
[[nodiscard]] Answer Answered(Value value, Outcome outcome = Outcome::Affirmative);

/// Put the connections' own remarks in front of @p answer's, saying each sentence once.
///
/// **One fault, one sentence.** Two connections dialled at an address nothing answers each report
/// `cannot reach <address> (...)` word for word, and printed twice they read as two faults. So a remark
/// identical to one already said is dropped, wherever it repeats -- a connection's, or the verb's own. A
/// DIFFERENT sentence about the same address is kept: two dials can fail two ways.
/// @param answer The answer; its advisories come after @p remarks.
/// @param remarks The connections' remarks, in the order they were made.
void PrependRemarks(Answer& answer, std::span<std::string const> remarks);

} // namespace FastCache::Cli
