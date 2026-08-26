// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Registry.hpp>

#if defined(_WIN32)
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #include <ranges>
    #include <utility>

    #include <windows.h>
#endif

namespace FastCache
{

#if defined(_WIN32)

namespace
{
    /// The predefined handle a hive names.
    ///
    /// A function rather than a table, because the `HKEY_*` macros are
    /// reinterpret_casts of magic constants and so cannot sit in a `constexpr`
    /// array. The switch carries no `default:`, which buys the same guarantee an
    /// `EnumTable` would: a hive added to the enum is a compile error here rather
    /// than a lookup that silently starts from nothing.
    ///
    /// @param hive The hive to translate.
    /// @return Its predefined key handle.
    [[nodiscard]] HKEY RootOf(RegistryHive hive) noexcept
    {
        switch (hive)
        {
            case RegistryHive::LocalMachine:
                return HKEY_LOCAL_MACHINE;
            case RegistryHive::CurrentUser:
                return HKEY_CURRENT_USER;
        }
        // Unreachable while the switch is total, and null rather than a hive of
        // its own choosing so that if it ever is reached the open fails and the
        // caller reports "absent" -- never the wrong machine-wide key.
        return {};
    }

    /// An open registry key that closes itself.
    ///
    /// Every path out of the two functions below is an early return -- a key that
    /// is not there, a value of the wrong type, an enumeration that stops short --
    /// so a hand-written `RegCloseKey` before each would be one leak per branch
    /// anybody later adds.
    class RegKey
    {
      public:
        /// @param hive Root to open beneath.
        /// @param subKey Key path, backslash-separated.
        /// @param view Which of the host's two registry views to read.
        RegKey(RegistryHive hive, std::string_view subKey, RegistryView view)
        {
            REGSAM const access = KEY_READ | (view == RegistryView::ThirtyTwoBit ? KEY_WOW64_32KEY : REGSAM { 0 });
            std::string const path { subKey };
            if (::RegOpenKeyExA(RootOf(hive), path.c_str(), 0, access, &_key) != ERROR_SUCCESS)
                _key = nullptr;
        }

        ~RegKey()
        {
            if (_key != nullptr)
                ::RegCloseKey(_key);
        }

        RegKey(RegKey const&) = delete;
        RegKey& operator=(RegKey const&) = delete;
        RegKey(RegKey&&) = delete;
        RegKey& operator=(RegKey&&) = delete;

        /// @return True when the key was opened.
        [[nodiscard]] bool Valid() const noexcept
        {
            return _key != nullptr;
        }

        /// @return The raw handle; null when the key could not be opened.
        [[nodiscard]] HKEY Get() const noexcept
        {
            return _key;
        }

      private:
        HKEY _key { nullptr };
    };

    /// Expand `%VAR%` references the way a `REG_EXPAND_SZ` value expects.
    /// @param value The unexpanded value.
    /// @return The expanded value, or @p value when expansion fails.
    [[nodiscard]] std::string ExpandValue(std::string const& value)
    {
        // The size query counts the terminating NUL, and reports 0 on failure --
        // which is not the same as "expands to nothing", so it falls back to the
        // input rather than to an empty string.
        DWORD const needed = ::ExpandEnvironmentStringsA(value.c_str(), nullptr, 0);
        if (needed == 0)
            return value;

        std::string expanded(needed, '\0');
        DWORD const written = ::ExpandEnvironmentStringsA(value.c_str(), expanded.data(), needed);
        if (written == 0 || written > needed)
            return value;
        expanded.resize(written - 1);
        return expanded;
    }

    /// The registry's own ceiling on a value name, in characters.
    ///
    /// It is what bounds the grow-and-retry below: `RegEnumValue` does not report
    /// the size a name needed when it refuses one, so the only way to find out is
    /// to ask again with more room -- and a loop that grows until the API is happy,
    /// with nothing saying when to stop, is a loop that never does.
    constexpr DWORD MaxValueNameChars = 16'383;

    /// The value name at @p index, whatever its length.
    ///
    /// Split out because the retry is the whole point. `RegQueryInfoKeyA` reports
    /// the longest name it saw at the moment it was asked, and a longer one created
    /// between that call and this one comes back as `ERROR_MORE_DATA` -- which the
    /// obvious loop reads as "no more values" and stops at. For the Windows SDK's
    /// `Installed Roots`, whose values ARE the installed kits, that is a toolchain
    /// this machine has and discovery never sees, with nothing anywhere saying so.
    ///
    /// @param key The open key.
    /// @param index Enumeration index.
    /// @param lengthHint The longest name `RegQueryInfoKeyA` sampled, excluding
    ///        the terminator.
    /// @return The name, or `std::nullopt` at the end of the enumeration (and for
    ///         a name the registry's own ceiling says cannot exist).
    [[nodiscard]] std::optional<std::string> ValueNameAt(HKEY key, DWORD index, DWORD lengthHint)
    {
        for (DWORD capacity = lengthHint + 1; capacity <= MaxValueNameChars + 1; capacity *= 2)
        {
            std::string name(capacity, '\0');
            DWORD used = capacity;
            auto const status = ::RegEnumValueA(key, index, name.data(), &used, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_SUCCESS)
            {
                name.resize(used);
                return name;
            }
            if (status != ERROR_MORE_DATA)
                return std::nullopt;
        }
        return std::nullopt;
    }

    /// Drop everything from the first embedded NUL onwards.
    ///
    /// `RegQueryValueEx` reports the stored byte count, and whether that count
    /// includes a terminator is up to whoever wrote the value -- the API
    /// documents that a string may be stored without one. Sizing the result from
    /// the byte count alone therefore yields a path with a trailing `\0` inside
    /// it about as often as not, and such a string compares unequal to the same
    /// path spelled by hand and opens nothing.
    ///
    /// @param text The raw bytes, already sized to the reported length.
    /// @return The text up to the first NUL.
    [[nodiscard]] std::string TrimAtNul(std::string text)
    {
        if (auto const nul = text.find('\0'); nul != std::string::npos)
            text.resize(nul);
        return text;
    }
} // namespace

std::optional<std::string> ReadRegistryString(RegistryHive hive,
                                              std::string_view subKey,
                                              std::string_view valueName,
                                              RegistryView view)
{
    RegKey const key { hive, subKey, view };
    if (!key.Valid())
        return std::nullopt;

    std::string const name { valueName };

    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExA(key.Get(), name.c_str(), nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
        return std::nullopt;

    // Any other type is absent rather than reinterpreted -- see the header.
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return std::nullopt;
    if (bytes == 0)
        return std::string {};

    std::string raw(bytes, '\0');
    if (::RegQueryValueExA(key.Get(), name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(raw.data()), &bytes)
        != ERROR_SUCCESS)
        return std::nullopt;
    // Re-checked after the fetch: the value can be rewritten between the two
    // calls, and the second call's type is the one describing the bytes in hand.
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return std::nullopt;

    raw.resize(bytes);
    auto trimmed = TrimAtNul(std::move(raw));
    if (type == REG_EXPAND_SZ)
        return ExpandValue(trimmed);
    return trimmed;
}

std::vector<std::string> ListRegistryValueNames(RegistryHive hive, std::string_view subKey, RegistryView view)
{
    RegKey const key { hive, subKey, view };
    if (!key.Valid())
        return {};

    DWORD count = 0;
    DWORD longestName = 0;
    if (::RegQueryInfoKeyA(
            key.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &longestName, nullptr, nullptr, nullptr)
        != ERROR_SUCCESS)
        return {};

    // `count` sizes the vector and does NOT bound the loop. It is a sample taken
    // before the enumeration began, and a value added while it runs makes the real
    // end one entry further on; only ERROR_NO_MORE_ITEMS says where that is.
    std::vector<std::string> names;
    names.reserve(count);
    for (DWORD const index: std::views::iota(DWORD { 0 }))
    {
        auto name = ValueNameAt(key.Get(), index, longestName);
        if (!name.has_value())
            break;
        names.push_back(*std::move(name));
    }
    return names;
}

#else

// Named rather than left anonymous even though nothing reads them: this is the
// contract's other half, and a reader arriving here should see the same signature
// the header states rather than have to go back for it.
std::optional<std::string> ReadRegistryString(RegistryHive /*hive*/,
                                              std::string_view /*subKey*/,
                                              std::string_view /*valueName*/,
                                              RegistryView /*view*/)
{
    return std::nullopt;
}

std::vector<std::string> ListRegistryValueNames(RegistryHive /*hive*/, std::string_view /*subKey*/, RegistryView /*view*/)
{
    return {};
}

#endif

} // namespace FastCache
