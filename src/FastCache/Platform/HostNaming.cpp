// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostNaming.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
    #include <FastCache/Platform/NarrowText.hpp>

    #include <windows.h>
#else
    #include <sys/socket.h>
    #include <sys/types.h>

    #include <netdb.h>
#endif

namespace FastCache
{

namespace
{
    /// @param text A DNS name.
    /// @return @p text without one trailing root dot.
    [[nodiscard]] std::string_view WithoutRootDot(std::string_view text) noexcept
    {
        if (text.ends_with('.'))
            text.remove_suffix(1);
        return text;
    }

    /// @param text A DNS name.
    /// @return Its first label: everything before the first dot.
    [[nodiscard]] std::string_view FirstLabelOf(std::string_view text) noexcept
    {
        return text.substr(0, text.find('.'));
    }

    /// @param character One byte of a DNS name.
    /// @return Its ASCII lower-case form; any other byte unchanged.
    [[nodiscard]] constexpr char AsciiLower(char character) noexcept
    {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    }

    /// @return Whether @p left and @p right are the same DNS text, ASCII case aside.
    [[nodiscard]] bool SameDnsText(std::string_view left, std::string_view right) noexcept
    {
        return std::ranges::equal(
            left, right, [](char const one, char const two) { return AsciiLower(one) == AsciiLower(two); });
    }

    /// The names, judged once and handed back unchanged.
    class FixedHostNaming final: public IHostNaming
    {
      public:
        /// @param answer What `JudgeHostNaming` made of the platform's names.
        explicit FixedHostNaming(HostNamingAnswer answer):
            _answer { std::move(answer) }
        {
        }

        [[nodiscard]] std::string FullyQualifiedName() const override
        {
            return _answer.fqdn;
        }
        [[nodiscard]] std::string PrimaryDnsSuffix() const override
        {
            return _answer.suffix;
        }
        [[nodiscard]] std::string DeclinedName() const override
        {
            return _answer.declined;
        }

      private:
        HostNamingAnswer _answer;
    };

#if defined(_WIN32)
    /// One of this machine's names, as `GetComputerNameExW` reports it.
    /// @param format Which name.
    /// @return The name in UTF-8 (empty when the machine has none of this kind), or
    ///         `std::nullopt` when the platform would not say.
    [[nodiscard]] std::optional<std::string> ComputerName(COMPUTER_NAME_FORMAT format)
    {
        // Asked for its size first: the documented contract is that a too-small buffer fails
        // with ERROR_MORE_DATA and reports the size INCLUDING the terminator, while an empty
        // name may succeed outright on the sizing call.
        DWORD size = 0;
        if (::GetComputerNameExW(format, nullptr, &size) != 0)
            return std::string {};
        if (::GetLastError() != ERROR_MORE_DATA || size == 0)
            return std::nullopt;

        std::wstring name;
        name.resize(size);
        if (::GetComputerNameExW(format, name.data(), &size) == 0)
            return std::nullopt;
        // On success the size EXCLUDES the terminator.
        name.resize(size);
        return Utf8FromWideText(name);
    }

    /// @return This machine's naming, read from local configuration.
    [[nodiscard]] std::unique_ptr<IHostNaming> ReadHostNaming()
    {
        // A name the platform would not give, or gave empty, falls back to the bare host name, so
        // the FQDN is empty only when the machine will not name itself at all.
        auto const fqdn = ComputerName(ComputerNameDnsFullyQualified).value_or(std::string {});
        auto const suffix = ComputerName(ComputerNameDnsDomain).value_or(std::string {});
        auto const name = WithoutRootDot(fqdn);
        return std::make_unique<FixedHostNaming>(
            JudgeHostNaming(name.empty() ? QueryHostFacts().hostName : std::string { name }, WithoutRootDot(suffix)));
    }
#else
    /// Frees what `getaddrinfo` returned.
    struct AddressInfoFree
    {
        /// @param list The list to free.
        void operator()(addrinfo* list) const noexcept
        {
            ::freeaddrinfo(list);
        }
    };

    /// @param hostName What the machine calls itself.
    /// @return The resolver's canonical name for it, or empty when it answered none.
    [[nodiscard]] std::string CanonicalNameOf(std::string const& hostName)
    {
        if (hostName.empty())
            return {};

        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_CANONNAME;
        addrinfo* found = nullptr;
        if (::getaddrinfo(hostName.c_str(), nullptr, &hints, &found) != 0 || found == nullptr)
            return {};
        auto const list = std::unique_ptr<addrinfo, AddressInfoFree> { found };
        return list->ai_canonname != nullptr ? std::string { list->ai_canonname } : std::string {};
    }

    /// @return This machine's naming, from its host name and what the resolver says of it.
    [[nodiscard]] std::unique_ptr<IHostNaming> ReadHostNaming()
    {
        // A COPY, never a reference into `QueryHostFacts`' static: this runs on a thread the
        // start detaches once its bound runs out (`ThreadedHostNamingLookup`), and a process that
        // then exits destroys that static while `getaddrinfo` below is still inside it. Nothing
        // this read uses after its wait may be something the exit tears down.
        auto const hostName = QueryHostFacts().hostName;
        auto const fqdn = FullyQualifiedNameFrom(hostName, CanonicalNameOf(hostName));
        return std::make_unique<FixedHostNaming>(JudgeHostNaming(fqdn, DnsSuffixOf(fqdn)));
    }
#endif
} // namespace

std::unique_ptr<IHostNaming> MakeSystemHostNaming()
{
    return ReadHostNaming();
}

std::string DnsSuffixOf(std::string_view fqdn)
{
    auto const name = WithoutRootDot(fqdn);
    auto const dot = name.find('.');
    return dot == std::string_view::npos ? std::string {} : std::string { name.substr(dot + 1) };
}

HostNamingAnswer JudgeHostNaming(std::string_view fqdn, std::string_view suffix)
{
    auto const name = WithoutRootDot(fqdn);
    auto const isPlaceholder = [](std::string_view text) {
        return std::ranges::any_of(PlaceholderDnsSuffixes,
                                   [text](std::string_view placeholder) { return SameDnsText(text, placeholder); });
    };
    if (isPlaceholder(WithoutRootDot(suffix)) || isPlaceholder(DnsSuffixOf(name)))
        return HostNamingAnswer { .fqdn = std::string { FirstLabelOf(name) },
                                  .suffix = {},
                                  .declined = std::string { name } };
    return HostNamingAnswer { .fqdn = std::string { name },
                              .suffix = std::string { WithoutRootDot(suffix) },
                              .declined = {} };
}

std::string FullyQualifiedNameFrom(std::string_view hostName, std::string_view canonicalName)
{
    auto const canonical = WithoutRootDot(canonicalName);
    auto const label = FirstLabelOf(hostName);
    if (label.empty() || canonical.empty())
        return std::string { hostName };

    auto const namesThisHost = SameDnsText(canonical, hostName)
                               || (canonical.size() > label.size() && canonical[label.size()] == '.'
                                   && SameDnsText(canonical.substr(0, label.size()), label));
    return std::string { namesThisHost ? canonical : hostName };
}

} // namespace FastCache
