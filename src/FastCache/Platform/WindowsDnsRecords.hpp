// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file WindowsDnsRecords.hpp
/// How a Windows DNS client's record list becomes SRV targets, as a pure function the tests drive
/// with a list they build themselves.
///
/// A header of its own, and Windows-only, because its signature names `windns.h` types: putting it
/// in `SrvResolver.hpp` would pull `<windows.h>` into everything that includes the seam.
#if defined(_WIN32)

    #include <FastCache/Platform/SrvResolver.hpp>

    #include <expected>
    #include <vector>

    #include <windows.h>

    #include <windns.h>

namespace FastCache
{

/// The SRV targets a finished `DnsQueryEx` found, or why there are none.
///
/// The DNS client parses the message itself, so what is left to decide is which records count:
/// only `DNS_TYPE_SRV` records from the ANSWER section. The list also carries the additional
/// section -- a target's own A records, and any SRV records a server volunteered there -- and a
/// CNAME the name led through. A target whose UTF-16 is not valid is unreadable and makes the
/// answer `Truncated` for `SrvAnswerOf`, which also drops RFC 2782's "." target.
/// @param status  The query's status: `DNS_ERROR_RCODE_NAME_ERROR` is `NoSuchName`,
///                `DNS_INFO_NO_RECORDS` `NoAnswer`, any other failure `ResolverFailed`.
/// @param first   The first record, or null. Read as UTF-16 whatever `UNICODE` says, because that
///                is what `DnsQueryEx` writes.
/// @return The usable targets (never empty), or the fault.
[[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvTargetsInRecordList(DNS_STATUS status,
                                                                                             DNS_RECORDW const* first);

} // namespace FastCache

#endif
