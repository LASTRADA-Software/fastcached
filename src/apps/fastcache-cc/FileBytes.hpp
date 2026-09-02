// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <vector>

namespace FastCache::Cc
{

/// Read a whole file as bytes.
///
/// **One spelling, in a header, and both halves of that are load-bearing.** This
/// directory had grown three copies of the same signature -- `CompileJob.cpp`,
/// `main.cpp` and the hit verifier -- and only the first was written the way this
/// project has twice decided it has to be. A header rather than a row of
/// `_fc_cc_sources` because `main.cpp` is in no test target (#370) and can include
/// a header regardless, which a shared translation unit would not fix.
///
/// Sized from the stream and read in one call, deliberately NOT through
/// `std::istreambuf_iterator`. GCC 14 at `-O2` inlines that iterator's `sgetc` and
/// then reports `-Werror=null-dereference` inside `<streambuf>` itself -- a false
/// positive, but one this project cannot silence, since warnings are errors and the
/// rule is to fix them at the source rather than suppress them. It is also one
/// allocation and one read instead of a per-character loop with geometric regrowth
/// and then a second full-size copy, so the workaround is the better implementation
/// regardless of the warning.
///
/// @param path The file to read.
/// @return Its bytes, or nothing when it could not be opened, sized or read.
[[nodiscard]] inline std::optional<std::vector<std::byte>> ReadFileBytes(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary | std::ios::ate };
    if (!in)
        return std::nullopt;
    auto const size = in.tellg();
    if (size < 0)
        return std::nullopt;
    in.seekg(0, std::ios::beg);

    std::vector<std::byte> out(static_cast<std::size_t>(size));
    if (!out.empty() && !in.read(reinterpret_cast<char*>(out.data()), size))
        return std::nullopt;
    return out;
}

} // namespace FastCache::Cc
