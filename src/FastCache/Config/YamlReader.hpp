// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/ConfigError.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// One top-level key from a YAML document, with the scalar values it carried.
struct YamlSetting
{
    /// The top-level key, verbatim. Never interpreted here.
    std::string key;

    /// One entry for a scalar; one per element for a sequence of scalars.
    ///
    /// A sequence rather than a second shape, because that is what a repeatable
    /// setting looks like in YAML and a caller applying values one at a time does
    /// not need to know which spelling it met.
    std::vector<std::string> values;

    /// One-based source line of the key, or 0 when the document would not say.
    /// Carried so a rejection points at the line an operator has to edit.
    unsigned line { 0 };
};

/// Read a YAML file as top-level key/scalar settings, without knowing what any
/// key MEANS.
///
/// The ONE door both binaries read a configuration file through. It returns the
/// file's shape and nothing else, and each binary drives its own option table from it
/// (`ApplyFileSettings`), so a value reaches the same parser the command line does.
/// The daemon had a second reader that parsed straight into `Config` through a
/// hand-written key ladder, with yaml-cpp's own conversions: it accepted `port: 0x50`
/// and `bind: ""` while argv refused both, and it is gone (#1437).
///
/// **yaml-cpp stays out of the header** — values come back as `std::string`, so the
/// dependency remains an implementation detail of this translation unit.
///
/// Deliberately shallow. A value that is a map, or a sequence containing anything
/// but scalars, is a `TypeMismatch` naming the key rather than something flattened:
/// a caller that applies values through a table has no way to represent nesting,
/// and silently ignoring a nested block would be a setting an operator wrote and
/// nothing read. That is why the daemon's listeners are `listen:` and `listen_tls:`,
/// sequences of `host:port` spelling the two flags, rather than one nested block.
/// A key that is not a scalar is a `ParseError`.
///
/// A top-level key written twice is a `ParseError` naming the key and both lines.
/// The parser hands over both entries, and without the refusal a scalar setting
/// would keep the last one written while a list setting appended both -- a value
/// the operator wrote either discarded or doubled, and nothing saying which.
///
/// An empty document is success carrying nothing, because a fully-commented
/// reference file is a legitimate and expected configuration.
/// @param path Filesystem path of the YAML file.
/// @return The settings in document order, or why the file could not be read.
[[nodiscard]] std::expected<std::vector<YamlSetting>, ConfigError> ReadYamlSettings(std::filesystem::path const& path);

} // namespace FastCache
