// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DirectManifest.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc::Test
{

/// Hashes each file at the moment `BuildManifest` asks. That is RECORD time, which is what
/// `BuildManifest` did by itself before #1650, and it is right for every case here that is about
/// CLASSIFICATION rather than timing. Test-only on purpose: production hands `BuildManifest` an
/// input witness, so nothing it records can have been read after the compile.
class HashedWhenAsked final: public IManifestContent
{
  public:
    [[nodiscard]] std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const override
    {
        auto hash = HashFileContents(resolvedPath);
        if (hash.empty())
            return std::unexpected(ManifestFault::Unreadable);
        return hash;
    }
};

/// Answers from a fixed script and records every path it was asked about, so a case can assert
/// that `BuildManifest` took its hashes from here and asked about exactly the paths an input
/// witness would have covered.
class ScriptedContent final: public IManifestContent
{
  public:
    /// One scripted answer.
    struct Answer
    {
        std::string path;                                   ///< Resolved path asked about.
        std::expected<std::string, ManifestFault> response; ///< What to answer.
    };

    /// @param answers The script; a path missing from it answers `Unreadable`.
    explicit ScriptedContent(std::vector<Answer> answers):
        _answers { std::move(answers) }
    {
    }

    [[nodiscard]] std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const override
    {
        _asked.emplace_back(resolvedPath);
        for (auto const& answer: _answers)
            if (answer.path == resolvedPath)
                return answer.response;
        return std::unexpected(ManifestFault::Unreadable);
    }

    /// @return Every path asked about, in order.
    [[nodiscard]] std::vector<std::string> const& Asked() const noexcept
    {
        return _asked;
    }

  private:
    std::vector<Answer> _answers;
    mutable std::vector<std::string> _asked;
};

} // namespace FastCache::Cc::Test
