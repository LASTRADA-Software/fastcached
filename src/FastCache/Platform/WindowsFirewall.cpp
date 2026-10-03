// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Firewall.hpp>

#include <memory>

#if defined(_WIN32)
    #include <array>
    #include <bit>
    #include <cstddef>
    #include <cstring>
    #include <expected>
    #include <format>
    #include <functional>
    #include <iterator>
    #include <optional>
    #include <set>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    #include <windows.h>

    #include <netfw.h>
    #include <oleauto.h>

    #include <core/Ranges.hpp>
    #include <wrl/client.h>
#endif

namespace FastCache
{

#if !defined(_WIN32)

std::unique_ptr<IFirewall> MakeSystemFirewall()
{
    return nullptr;
}

#else

namespace
{
    using Microsoft::WRL::ComPtr;

    /// One COM apartment for the duration of a call; nothing here outlives it.
    class ComApartment
    {
      public:
        ComApartment() noexcept:
            _result { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) }
        {
        }
        ~ComApartment()
        {
            if (SUCCEEDED(_result))
                CoUninitialize();
        }
        ComApartment(ComApartment const&) = delete;
        ComApartment& operator=(ComApartment const&) = delete;
        ComApartment(ComApartment&&) = delete;
        ComApartment& operator=(ComApartment&&) = delete;

        /// @return Whether COM is usable on this thread (RPC_E_CHANGED_MODE still is).
        [[nodiscard]] bool Usable() const noexcept
        {
            return SUCCEEDED(_result) || _result == RPC_E_CHANGED_MODE;
        }

      private:
        HRESULT _result;
    };

    /// An owned `BSTR`; `SysAllocString` answers null when it runs out of memory, so every
    /// holder asks `Allocated()` before handing one to the firewall.
    class Bstr
    {
      public:
        /// @param text The text to copy into a newly allocated `BSTR`.
        explicit Bstr(std::wstring const& text):
            _value { SysAllocString(text.c_str()) }
        {
        }
        ~Bstr()
        {
            SysFreeString(_value);
        }
        Bstr(Bstr const&) = delete;
        Bstr& operator=(Bstr const&) = delete;
        Bstr(Bstr&&) = delete;
        Bstr& operator=(Bstr&&) = delete;

        /// @return The string, still owned by this object.
        [[nodiscard]] BSTR Get() const noexcept
        {
            return _value;
        }

        /// @return Whether the copy was made; a null `BSTR` reads to COM as an empty string.
        [[nodiscard]] bool Allocated() const noexcept
        {
            return _value != nullptr;
        }

      private:
        BSTR _value;
    };

    /// @param utf8 Text this project holds (UTF-8, `char` is UTF-8 here).
    /// @return The same text for a wide Win32 API.
    [[nodiscard]] std::wstring Wide(std::string_view utf8)
    {
        if (utf8.empty())
            return {};
        auto const length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        (void) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
        return wide;
    }

    /// @param wide Text a wide Win32 API returned.
    /// @return The same text in UTF-8, as this project holds text.
    [[nodiscard]] std::string Narrow(std::wstring_view wide)
    {
        if (wide.empty())
            return {};
        auto const length =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};
        std::string narrow(static_cast<std::size_t>(length), '\0');
        (void) WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), length, nullptr, nullptr);
        return narrow;
    }

    /// @param get A property getter handing out a `BSTR` the caller owns.
    /// @return The property's text, or nullopt when it could not be read or is null.
    template <typename Getter>
    [[nodiscard]] std::optional<std::wstring> ReadText(Getter get)
    {
        BSTR text = nullptr;
        std::optional<std::wstring> read;
        if (SUCCEEDED(get(&text)) && text != nullptr)
            read.emplace(text, SysStringLen(text));
        SysFreeString(text);
        return read;
    }

    /// @param item A `VARIANT` an enumerator handed out, still owned by the caller.
    /// @return Its interface pointer when it holds one (`VT_DISPATCH`), else null; not AddRef'd.
    ///
    /// Read by offset rather than as `item.vt` and `item.pdispVal`: both sit in the anonymous
    /// unions `VARIANT` is built from, and this tree's analyser refuses member access through a
    /// union (`cppcoreguidelines-pro-type-union-access`). The tag is read first and the pointer
    /// only under it, which is the discipline the check asks for.
    [[nodiscard]] IDispatch* DispatchOf(VARIANT const& item) noexcept
    {
        auto const* const bytes = reinterpret_cast<std::byte const*>(&item);
        auto type = VARTYPE { VT_EMPTY };
        std::memcpy(&type, bytes + offsetof(VARIANT, vt), sizeof type);
        if (type != VT_DISPATCH)
            return nullptr;
        std::array<std::byte, sizeof(IDispatch*)> pointer {};
        std::memcpy(pointer.data(), bytes + offsetof(VARIANT, pdispVal), pointer.size());
        return std::bit_cast<IDispatch*>(pointer);
    }

    /// Orders rule names ordinally, ignoring case: two names differing only in case are one name
    /// here, since treating them as two when the firewall does not is the direction that removes
    /// somebody else's rule.
    struct NameLess
    {
        /// @return Whether @p left sorts before @p right, ignoring case.
        [[nodiscard]] bool operator()(std::wstring const& left, std::wstring const& right) const noexcept
        {
            return CompareStringOrdinal(
                       left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE)
                   == CSTR_LESS_THAN;
        }
    };

    /// @param action What was being done.
    /// @param result The failing `HRESULT`.
    /// @return A sentence naming both, with the elevation hint where access was denied.
    [[nodiscard]] std::string Failure(std::string_view action, HRESULT result)
    {
        if (result == E_ACCESSDENIED)
            return std::format("access denied {}; run from an elevated (Administrator) prompt", action);
        return std::format("{} failed (HRESULT 0x{:08X})", action, static_cast<unsigned long>(result));
    }

    /// The policy's rule collection, with the policy it came from held beside it.
    struct PolicyRules
    {
        ComPtr<INetFwPolicy2> policy; ///< The firewall policy.
        ComPtr<INetFwRules> rules;    ///< Its rules.
    };

    /// @return The machine's firewall rules, or why they could not be reached.
    [[nodiscard]] std::expected<PolicyRules, std::string> OpenRules()
    {
        PolicyRules opened;
        if (auto const hr =
                CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&opened.policy));
            FAILED(hr))
            return std::unexpected(Failure("opening the firewall policy", hr));
        if (auto const hr = opened.policy->get_Rules(&opened.rules); FAILED(hr))
            return std::unexpected(Failure("reading the firewall rules", hr));
        return opened;
    }

    /// Every rule's name, split by whether the rule is filed under the group asked about.
    struct GroupCensus
    {
        std::vector<std::wstring> ours;          ///< The group's rule names, in the order the firewall lists them.
        std::set<std::wstring, NameLess> others; ///< Every other rule's name, as that rule spells it.
    };

    /// Walk every rule of @p rules once, sorting names by @p group.
    ///
    /// One walk for a removal and a listing alike, so both read a group the same way: a rule of the
    /// group whose name cannot be read refuses the walk, since a removal would miss it and a
    /// listing would not name it.
    /// @param rules The policy's rules.
    /// @param group The group.
    /// @return The names, or why the walk could not finish.
    [[nodiscard]] std::expected<GroupCensus, std::string> TakeCensus(INetFwRules& rules, std::string_view group)
    {
        ComPtr<IUnknown> enumerable;
        if (auto const hr = rules.get__NewEnum(&enumerable); FAILED(hr))
            return std::unexpected(Failure("listing the firewall rules", hr));
        ComPtr<IEnumVARIANT> each;
        if (auto const hr = enumerable.As(&each); FAILED(hr))
            return std::unexpected(Failure("listing the firewall rules", hr));

        // Collected first and removed after: removing while enumerating skips entries.
        //
        // `INetFwRules::Remove` takes a NAME, and Windows does not keep rule names unique: an
        // operator's own rule may carry the name one of ours does, and removing that name may
        // take theirs. So the same walk records every name seen OUTSIDE the group, and a name
        // on both sides refuses the whole removal before anything is removed.
        auto const wanted = Wide(group);
        GroupCensus census;
        VARIANT item;
        VariantInit(&item);
        ULONG fetched = 0;
        while (true)
        {
            // S_FALSE is the end of the list; a failure is not, and a walk that stopped early
            // would miss both rules of the group and rules sharing their names.
            auto const next = each->Next(1, &item, &fetched);
            if (FAILED(next))
                return std::unexpected(Failure("listing the firewall rules", next));
            if (next != S_OK || fetched != 1)
                break;
            ComPtr<INetFwRule> rule;
            if (auto* const dispatch = DispatchOf(item);
                dispatch != nullptr && SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&rule))))
            {
                auto const grouping = ReadText([&rule](BSTR* text) { return rule->get_Grouping(text); });
                auto name = ReadText([&rule](BSTR* text) { return rule->get_Name(text); });
                if (grouping == wanted)
                {
                    if (!name.has_value())
                    {
                        VariantClear(&item);
                        return std::unexpected(
                            std::format("a rule of '{}' has a name that could not be read; nothing was changed", group));
                    }
                    census.ours.push_back(std::move(*name));
                }
                else if (name.has_value())
                    census.others.insert(std::move(*name));
            }
            VariantClear(&item);
        }
        return census;
    }

    /// One property a new rule is given: what a refusal calls it, and the call that sets it.
    struct RuleProperty
    {
        std::string_view name;                      ///< How a refusal names the property.
        std::function<HRESULT(INetFwRule&)> assign; ///< Sets it on the rule.
    };

    /// The Windows Firewall, through `INetFwPolicy2`.
    class WindowsFirewall final: public IFirewall
    {
      public:
        [[nodiscard]] std::expected<void, std::string> AddInboundAllow(FirewallRule const& rule) override
        {
            ComApartment const apartment;
            if (!apartment.Usable())
                return std::unexpected(std::string { "COM could not be initialised on this thread" });

            auto const opened = OpenRules();
            if (!opened.has_value())
                return std::unexpected(opened.error());
            ComPtr<INetFwRule> created;
            if (auto const hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created));
                FAILED(hr))
                return std::unexpected(Failure("creating a firewall rule", hr));

            std::wstring remote;
            for (auto const& scope: rule.remoteAddresses)
                remote += (remote.empty() ? L"" : L",") + Wide(scope);

            Bstr const name { Wide(rule.name) };
            Bstr const group { Wide(rule.group) };
            Bstr const program { rule.program.wstring() };
            Bstr const service { Wide(rule.serviceName) };
            // Through the kind's row: `*` for a rule admitting every local port, set explicitly
            // rather than left unset -- the remote addresses below are spelled the same way, and
            // a new UDP rule reads back `*` before any port is set, so the two are one rule.
            Bstr const port { Wide(FirewallPortKindRowOf(rule.localPort.kind).firewallText(rule.localPort.number)) };
            Bstr const addresses { remote.empty() ? std::wstring { L"*" } : remote };
            auto const protocol = FirewallProtocolRowOf(rule.protocol).ianaNumber;
            auto const texts = std::to_array<std::pair<std::string_view, Bstr const*>>({
                { "name", &name },
                { "group", &group },
                { "program", &program },
                { "service", &service },
                { "local port", &port },
                { "remote addresses", &addresses },
            });
            if (auto const* const missing =
                    core::findIfOrNull(texts, [](auto const& text) { return !text.second->Allocated(); }))
                return std::unexpected(
                    std::format("out of memory copying the {} of rule '{}' for the firewall", missing->first, rule.name));

            // Assigned in this order, and the order is load-bearing: the protocol BEFORE the port,
            // because the firewall refuses local ports on a rule whose protocol is still "any".
            auto const properties = std::to_array<RuleProperty>({
                { .name = "name", .assign = [&](INetFwRule& r) { return r.put_Name(name.Get()); } },
                { .name = "group", .assign = [&](INetFwRule& r) { return r.put_Grouping(group.Get()); } },
                { .name = "program", .assign = [&](INetFwRule& r) { return r.put_ApplicationName(program.Get()); } },
                { .name = "service", .assign = [&](INetFwRule& r) { return r.put_ServiceName(service.Get()); } },
                { .name = "protocol", .assign = [&](INetFwRule& r) { return r.put_Protocol(protocol); } },
                { .name = "local port", .assign = [&](INetFwRule& r) { return r.put_LocalPorts(port.Get()); } },
                { .name = "remote addresses",
                  .assign = [&](INetFwRule& r) { return r.put_RemoteAddresses(addresses.Get()); } },
                { .name = "direction", .assign = [](INetFwRule& r) { return r.put_Direction(NET_FW_RULE_DIR_IN); } },
                { .name = "action", .assign = [](INetFwRule& r) { return r.put_Action(NET_FW_ACTION_ALLOW); } },
                { .name = "profiles", .assign = [](INetFwRule& r) { return r.put_Profiles(NET_FW_PROFILE2_ALL); } },
                { .name = "enabled state", .assign = [](INetFwRule& r) { return r.put_Enabled(VARIANT_TRUE); } },
            });

            for (auto const& property: properties)
                if (auto const hr = property.assign(*created.Get()); FAILED(hr))
                    return std::unexpected(
                        Failure(std::format("setting the {} of rule '{}'", property.name, rule.name), hr));
            if (auto const hr = opened->rules->Add(created.Get()); FAILED(hr))
                return std::unexpected(Failure(std::format("adding rule '{}'", rule.name), hr));
            return {};
        }

        [[nodiscard]] std::expected<std::size_t, std::string> RemoveGroup(std::string_view group,
                                                                          std::span<std::string const> incoming) override
        {
            ComApartment const apartment;
            if (!apartment.Usable())
                return std::unexpected(std::string { "COM could not be initialised on this thread" });

            auto const opened = OpenRules();
            if (!opened.has_value())
                return std::unexpected(opened.error());
            auto const census = TakeCensus(*opened->rules.Get(), group);
            if (!census.has_value())
                return std::unexpected(census.error());
            auto const& names = census->ours;
            auto const& othersNames = census->others;

            // Both the names being removed and the names about to be added: an added name a rule
            // outside the group already carries makes the NEXT removal ambiguous.
            auto candidates = names;
            std::ranges::transform(
                incoming, std::back_inserter(candidates), [](std::string const& name) { return Wide(name); });
            // The set keeps the outside rule's own spelling, so both spellings are named.
            for (auto const& candidate: candidates)
                if (auto const outside = othersNames.find(candidate); outside != othersNames.end())
                    return std::unexpected(FirewallNameCollision(group, Narrow(candidate), Narrow(*outside)));

            auto removed = std::size_t { 0 };
            for (auto const& name: names)
            {
                Bstr const text { name };
                if (!text.Allocated())
                    return std::unexpected(std::format(
                        "out of memory copying the name of a rule of '{}' for the firewall ({} of {} removed before it)",
                        group,
                        removed,
                        names.size()));
                if (auto const hr = opened->rules->Remove(text.Get()); FAILED(hr))
                    return std::unexpected(Failure(
                        std::format("removing a rule of '{}' ({} of {} removed before it)", group, removed, names.size()),
                        hr));
                ++removed;
            }
            return removed;
        }

        [[nodiscard]] std::expected<std::vector<std::string>, std::string> NamesInGroup(std::string_view group) override
        {
            ComApartment const apartment;
            if (!apartment.Usable())
                return std::unexpected(std::string { "COM could not be initialised on this thread" });

            auto const opened = OpenRules();
            if (!opened.has_value())
                return std::unexpected(opened.error());
            auto const census = TakeCensus(*opened->rules.Get(), group);
            if (!census.has_value())
                return std::unexpected(census.error());
            std::vector<std::string> names;
            names.reserve(census->ours.size());
            std::ranges::transform(
                census->ours, std::back_inserter(names), [](std::wstring const& name) { return Narrow(name); });
            return names;
        }
    };
} // namespace

std::unique_ptr<IFirewall> MakeSystemFirewall()
{
    return std::make_unique<WindowsFirewall>();
}

#endif // _WIN32

} // namespace FastCache
