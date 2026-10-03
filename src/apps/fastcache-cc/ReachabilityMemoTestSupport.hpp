// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ReachabilityMemo.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FastCache::Cc::Testing
{

/// A memo store held in memory: what two launcher PROCESSES share, as one object two
/// `ReachabilityMemo`s load from and save to in turn.
class InMemoryMemoStore final: public IMemoStore
{
  public:
    [[nodiscard]] std::optional<std::string> Read() const override
    {
        return _text;
    }

    bool Write(std::string_view text) override
    {
        _text = std::string { text };
        ++_writes;
        return true;
    }

    /// Replace what is stored, as a crashed or foreign writer would.
    /// @param text The new contents, or nothing for a store holding no memo at all.
    void Plant(std::optional<std::string> text)
    {
        _text = std::move(text);
    }

    /// How many times a memo was saved into it.
    /// @return The number of `Write` calls so far.
    [[nodiscard]] int Writes() const noexcept
    {
        return _writes;
    }

  private:
    std::optional<std::string> _text;
    int _writes { 0 };
};

} // namespace FastCache::Cc::Testing
