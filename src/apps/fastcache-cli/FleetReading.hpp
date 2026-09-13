// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"

#include <string_view>

namespace FastCache::Cli
{

/// @file FleetReading.hpp
/// What a `fleet` session's sample reads as: the leader's whole `/fleet.txt`, validated.

/// The source name a fleet reading carries, which no stats source shares.
inline constexpr std::string_view FleetReadingSource = "fleet.txt";

/// The reader for a `fleet` session: the leader's whole document, validated by parsing it.
///
/// **The parse is VALIDATION, and its result is thrown away on purpose.** The reading's value is
/// the document's TEXT, and whoever draws it -- the fleet panel, the piped KPI record -- parses
/// that same text with the same `ParseFleetDocument` when it draws. A parsed copy kept here beside
/// the text would be a second answer to one question, free to go stale against it; so there is
/// none to keep, and a document that cannot be parsed never becomes a reading at all.
///
/// - A fetch that produced no document is the fetch's own outcome (`OutcomeOf`): `Unreachable`
///   when nothing answered, `Refused` when the leader -- or a follower naming it -- declined, with
///   the server's words as the note.
/// - A document that does not parse is `Protocol`, naming what the parser refused -- a body naming
///   no section among them, since an empty fleet is a claim only a leader can make. A follower
///   itself answers the route `503`, naming its leader, which is `Refused`.
/// @param event A `Sample` of a `fleet` session.
/// @return The document as a text scalar from `FleetReadingSource`, or why there is none.
[[nodiscard]] SampleReading ReadFleetSample(DashboardEvent const& event);

/// A piped `fleet` session's record: the `kpi` section of the newest reading, one field per figure.
///
/// **Parsed here, from the reading's text, when it is drawn** -- the reader kept no parsed copy, so
/// this and the fleet panel read one text through one parser. The strip arrives computed (#1302):
/// each field is named by the section's `kpi` column, spelled as the leader spells it -- the key
/// `/fleet.json` carries too -- and holds its `value`, a number where it reads as a finite one, `-`
/// read as absent. A figure counted against something (`compiling-now` of the slots) is followed by
/// `<kpi>-of`. `source` comes first, as every piped record's does.
/// @param model What is known; `latest` is a fleet reading's text when there is one.
/// @return The record: `source` alone before any reading.
[[nodiscard]] Value FleetKpiFigures(DashboardModel const& model);

} // namespace FastCache::Cli
