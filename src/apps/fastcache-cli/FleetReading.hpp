// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file FleetReading.hpp
/// What a `fleet` session's sample reads as: the leader's KPI strip, and the whole `/fleet.txt` parsed once.

/// The source name a fleet reading carries, which no stats source shares.
inline constexpr std::string_view FleetReadingSource = "fleet.txt";

/// The document a `fleet` session asks the leader for, and the route its source line names.
inline constexpr std::string_view FleetDocumentRoute = "/fleet.txt";

/// How a fleet reader parses the leader's document: `ParseFleetDocument`, or a test's counting stand-in.
using FleetParser = std::expected<FleetDocument, std::string> (*)(std::string_view document);

/// The reader for a `fleet` session: the leader's whole document, parsed ONCE, here.
///
/// **The parse decides whether there is a reading, and nothing parses the document again.** Its
/// result is handed over as the reading's `document`, which the model keeps for the newest sample
/// only, and which the fleet panel draws. The reading's VALUE is the `kpi` section as a record (one
/// field per figure, `<kpi>-of` after a figure counted against something) -- what the model's
/// history keeps for every sample, so that history holds a few numbers per entry, never the whole
/// text 256 times over, and a piped record takes the strip without a parser. A document that cannot
/// be parsed never becomes a reading at all.
///
/// - A fetch that produced no document is the fetch's own outcome (`OutcomeOf`): `Unreachable`
///   when nothing answered, `Refused` when the leader -- or a follower naming it -- declined, with
///   the server's words as the note.
/// - A document that does not parse is `Protocol`, naming what the parser refused -- a body naming
///   no section among them, since an empty fleet is a claim only a leader can make. A follower
///   itself answers the route `503`, naming its leader, which is `Refused`.
/// @param event A `Sample` of a `fleet` session.
/// @return The KPI record from `FleetReadingSource` with the document's parse, or why there is none.
[[nodiscard]] SampleReading ReadFleetSample(DashboardEvent const& event);

/// `ReadFleetSample`, through @p parse: the seam that lets a test count how often a sample is parsed.
/// @param event A `Sample` of a `fleet` session.
/// @param parse What reads the document; `ReadFleetSample` passes `ParseFleetDocument`.
/// @return As `ReadFleetSample`.
[[nodiscard]] SampleReading ReadFleetSampleThrough(DashboardEvent const& event, FleetParser parse);

/// A piped `fleet` session's record: the `kpi` section of the newest reading, one field per figure.
///
/// **Taken from the reading, never parsed again**: the reader made the strip a record when it parsed
/// the document, so this puts `source` in front of it and nothing more. The strip arrives computed (#1302):
/// each field is named by the section's `kpi` column, spelled as the leader spells it -- the key
/// `/fleet.json` carries too -- and holds its `value`, a number where it reads as a finite one, `-`
/// read as absent. A figure counted against something (`compiling-now` of the slots) is followed by
/// `<kpi>-of`. `source` comes first, as every piped record's does.
/// @param model What is known; `latest` is a fleet reading's KPI record when there is one.
/// @return The record: `source` alone before any reading.
[[nodiscard]] Value FleetKpiFigures(DashboardModel const& model);

} // namespace FastCache::Cli
