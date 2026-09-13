// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliValue.hpp"

#include <FastCache/Distributed/FleetView.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

using Distributed::FleetSection;

/// @file FleetDocument.hpp
/// Reading `/fleet.txt`: one section, or the whole document -- the ONE parser this client has.
///
/// **One parser for every reader of the leader's text.** The `fleet` verb reads one section,
/// a `live-stats fleet` session reads the whole document per sample, and a panel draws from
/// what that session read. Two copies of the grammar would be two places for a column the
/// leader renders to go missing, which is the defect #1320 records one repository over.
///
/// **A header of its own rather than a row of `CliVerbs.hpp`**, because reading the markers
/// needs `FleetSectionTable`, and that table's header carries the scheduler's and the
/// cluster's vocabulary: every translation unit including the verb table would compile it for
/// the three that read a fleet document.

/// One `/fleet.txt` section, as a table.
///
/// The header line names the columns and every later line is a row, which is the
/// whole grammar -- there is no column list here, and there must not be: the
/// leader decided them from `FleetColumn`, and a second list in this binary would
/// be a place for the two to disagree.
///
/// A cell of `-` becomes the real `Absent`, so `--absent` and the JSON `null`
/// behave as they do for every other verb rather than a dash being text that
/// happens to look absent. Everything else stays TEXT, escaping included: this
/// client has no model of which columns are numbers, and inventing one would be a
/// second description of the same columns.
/// @param document One section's rendering.
/// @return The table, or why the text is not one.
[[nodiscard]] std::expected<Value, std::string> FleetTable(std::string_view document);

/// The whole `/fleet.txt` document, one table per section it carried.
struct FleetDocument
{
    /// Each section's table, indexed by `FleetSection`; disengaged for a section the
    /// document did not carry.
    std::array<std::optional<Value>, static_cast<std::size_t>(FleetSection::Last)> sections {};

    /// The table @p section carried, or nullptr when the document had none.
    /// @param section Which section.
    /// @return The table; non-owning, valid while this document is unmodified.
    [[nodiscard]] Value const* Section(FleetSection section) const noexcept
    {
        auto const& slot = sections.at(static_cast<std::size_t>(section));
        return slot.has_value() ? &*slot : nullptr;
    }
};

/// Read the whole-document form of `/fleet.txt`: every section behind a `# <key>` marker.
///
/// **The markers are `FleetSectionTable`'s keys, looked up, never restated**, so a section the
/// leader grows is read the day this client's table has its row. A marker naming a key this
/// build does not know is SKIPPED with its lines rather than refused: a newer leader serving a
/// seventh section must not fail every sample an older client takes, when the six it knows are
/// all there.
///
/// Outside a section, a `#` line that is not a marker is a comment: the notes a follower
/// answers with. INSIDE one, every line but a marker is a row, a leading `#` included, because a
/// first cell can be text a peer chose and the renderer writes no comment there. Blank lines
/// separate sections and mean nothing else.
///
/// Refused, naming what was wrong:
///   - a line that is neither a comment nor inside a section, before the first marker;
///   - the same section twice, which leaves no way to say which one is the fleet;
///   - a section whose rows do not fit its own header (`FleetTable`'s refusal, named);
///   - a document carrying no section this build knows, which is what a follower's
///     all-comment answer looks like and never an empty fleet.
/// @param document The body of `/fleet.txt` with no `section` parameter.
/// @return The sections, or why the text is not such a document.
[[nodiscard]] std::expected<FleetDocument, std::string> ParseFleetDocument(std::string_view document);

} // namespace FastCache::Cli
