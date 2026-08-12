// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace FastCache::TestClient
{

/// What the test client was asked to do.
enum class Action : std::uint8_t
{
    Store,    ///< Compile, frame and STORE a value.
    Fetch,    ///< FETCH a value and validate what comes back.
    ShowHelp, ///< Print the usage text and exit.
};

/// A parsed command line.
struct Args
{
    Action action { Action::Store };  ///< The sub-command.
    std::string host { "127.0.0.1" }; ///< Daemon host.
    std::uint16_t port { 0 };         ///< Daemon port; required.
    std::string key {};               ///< Cache key.
    std::string cohort { "default" }; ///< Prefetch cohort id.
    std::string srcRoot {};           ///< Checkout source root.
    std::string buildTree {};         ///< Build output root.
    std::string compiler { "cl" };    ///< Compiler to drive (cl or clang-cl).
    std::string source {};            ///< Source file to compile.
    std::string object {};            ///< Object path: output for store, expected-write for fetch.
};

/// The accepted options, in the order `--help` documents them.
///
/// The single source of truth for this tool's CLI: the parser matches these
/// rows and the help text is rendered from them. Adding an option is adding a
/// row.
/// @return A view of the static table; never empty.
[[nodiscard]] std::span<OptionSpec<Args> const> TestClientOptions() noexcept;

/// Parse a command line.
///
/// The sub-command is positional and must come first; everything after it is
/// matched against `TestClientOptions()`. An unknown option is a hard error
/// rather than something silently ignored, and an option cannot swallow the
/// following flag as its value.
/// @param argv The arguments with the program name already removed.
/// @return The parsed arguments, or the first error encountered.
[[nodiscard]] std::expected<Args, ConfigError> ParseArgs(std::span<char const* const> argv);

/// Render the usage text.
/// @param color Whether to emit ANSI SGR escapes; see StdoutSupportsColor.
/// @return The complete usage text, ending in a newline.
[[nodiscard]] std::string HelpText(UsageColor color = UsageColor::Plain);

} // namespace FastCache::TestClient
