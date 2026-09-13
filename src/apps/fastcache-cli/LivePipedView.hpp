// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliFormat.hpp"
#include "CliValue.hpp"
#include "DashboardLoop.hpp"
#include "DashboardPanel.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file LivePipedView.hpp
/// The `Piped` rung's view: one record per sample, in the `--format` the operator asked for.
///
/// **A pipe is the bottom rung of the same ladder, not a special case** (#134 §1.6). There is
/// no frame to redraw, so a frame is one line: the header once, before the first reading, then
/// one row per sample. `--format=json` is one document per line. Nothing here writes an escape
/// sequence -- there is no colour input to ask for one -- because a stream somebody pipes into
/// `tee` and a script is the one place a stray `ESC` is never wanted (§9.13).
///
/// **The columns are fixed by the first reading and never move.** A header printed once is a
/// promise about every row under it, so a later reading is looked up BY NAME against it: a field
/// the header does not name is dropped and a column the reading lacks renders absent. The
/// alternative, a header per change of shape, is a stream no `read` loop can consume.
///
/// **So the header is written after the first SUCCESSFUL sample, never before it.** Some columns
/// exist only because of what a reading carried -- a cache's tiers, which no flag and no panel
/// states -- and a header written earlier would have to guess them: omit a tier the daemon runs,
/// or name one it does not. Waiting costs nothing a program can see, because a failure before the
/// first reading writes no row either. What that reading carried is the whole of it: a tier that
/// appears later gains no column, and one that goes away renders absent under its heading.
///
/// **A sample that failed is a row of absent cells, not a missing row** -- a gap, which is what
/// §9.17 calls a failure after the first success. What tells the view the sample failed is the
/// model's `runLength` being zero, which a failure and nothing else leaves behind. A failure
/// BEFORE the first reading has no columns to be absent in, so it writes nothing: the exit
/// code is what reports a run that never read anything.
///
/// `kv` is the one format that is not one line per sample, because `name=value` lines are its
/// grammar: each sample is one block of them, and a blank line separates two, which is how
/// `RenderValue` already lays out a `kv` table a row at a time.

/// What a piped row reports, from what the dashboard knows.
///
/// **Injected, so the rows are the subject's figures rather than whatever the reading holds.**
/// A plain function pointer for the reason `SampleReader` is one: a projection has nothing to
/// carry, and a capture is how ambient state would reach a renderer.
/// @param model What is known; `latest` is engaged.
/// @return A record; its field names become the header the first time.
using FigureProjection = Value (*)(DashboardModel const& model);

/// Every figure @p panel draws, as one record: what a piped row of that panel reports.
///
/// **Through the panel's own table and its own computation**, so a piped header names exactly the
/// figures an interactive run draws and each value is the one drawn: the newest cell of the
/// figure's series (`FigureSeries`, which takes a counter's rate through `CounterRateSeries`).
/// Unformatted, since a stream is read by a program: a rate is per the row's unit after its
/// scale, a proportion is a fraction, bytes are bytes. Absent wherever the panel draws absent --
/// before a rate has two readings, beside a gap, for a field the source does not carry -- and never
/// zero in its place.
///
/// Fields in panel order, each named by the row's own machine `key` -- never by its label, which is
/// worded for the screen: `source`, then each rate row followed by the figures beside it, then each
/// level row followed by its limit (`limitKey`), then, for each tier the newest reading carries
/// (`TiersIn`, the panel's own answer), every tier column as `TierFigureKey`: `memory_items`,
/// `disk_bytes_used`. A level row naming no field exists to say why there is nothing to draw, and
/// has no field here. With no reading there are no tiers, which is why `PipedRecordView` writes its
/// header from a reading.
/// @param panel The panel.
/// @param model What is known.
/// @return The record.
[[nodiscard]] Value PanelFigures(PanelSpec const& panel, DashboardModel const& model);

/// `PanelFigures` over `CachePanel()`: the `cache` subject's piped record.
/// @param model What is known.
/// @return The record.
[[nodiscard]] Value CacheFigures(DashboardModel const& model);

/// `PanelFigures` over `NodePanel()`: the `node` subject's piped record.
/// @param model What is known.
/// @return The record.
[[nodiscard]] Value NodeFigures(DashboardModel const& model);

/// Streams one record per sample.
class PipedRecordView final: public IDashboardView
{
  public:
    /// @param format How each record is written.
    /// @param absentOverride From `--absent`: what an absent cell renders as; ignored by `json`.
    /// @param project What a row reports.
    PipedRecordView(OutputFormat format, std::optional<std::string> absentOverride, FigureProjection project);

    /// The lines this frame owes, with nothing placed over them: a pipe draws no image.
    /// @param model What is known.
    /// @return The frame; its text is `Lines`.
    [[nodiscard]] DashboardFrame PlacedFrame(DashboardModel const& model) override;

  private:
    /// The lines this frame owes: the header when none was written yet, then one row.
    /// @param model What is known.
    /// @return The text, or nothing before the first reading.
    [[nodiscard]] std::string Lines(DashboardModel const& model);

    /// One row, rendered in the view's format.
    /// @param cells One cell per column.
    /// @return The row's text.
    [[nodiscard]] std::string Row(std::vector<Cell> const& cells) const;

    /// What an absent cell reads as in the view's format.
    /// @return The text.
    [[nodiscard]] std::string_view AbsentText() const noexcept;

    OutputFormat _format;
    std::optional<std::string> _absentOverride;
    FigureProjection _project;
    std::vector<std::string> _columns {};
    std::vector<std::size_t> _widths {};
    std::size_t _rows { 0 };
};

} // namespace FastCache::Cli
