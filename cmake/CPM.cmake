# SPDX-License-Identifier: Apache-2.0
# CPM.cmake bootstrap — downloads CPM.cmake on first use, caches it.
# https://github.com/cpm-cmake/CPM.cmake

# Bound every transfer below, and every `git clone` CPM performs after it, so a
# stalled one ends instead of hanging forever (#526). Included FIRST because
# the git half works by exporting environment variables that later subprocesses
# inherit, and the `file(DOWNLOAD)` half reads a value it defines. This file is
# the one place both are reached from: it is included before any CPMAddPackage,
# so a dependency added later cannot be added unbounded.
include("${CMAKE_CURRENT_LIST_DIR}/FetchTransferBound.cmake")

set(CPM_DOWNLOAD_VERSION 0.40.5)
set(CPM_HASH_SUM "c46b876ae3b9f994b4f05a4c15553e0485636862064f1fcc9d8b4f832086bc5d")

if(CPM_SOURCE_CACHE)
    set(CPM_DOWNLOAD_LOCATION "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
elseif(DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_DOWNLOAD_LOCATION "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
else()
    set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
endif()

# Expand relative path. This is important if the provided path contains a tilde (~)
get_filename_component(CPM_DOWNLOAD_LOCATION ${CPM_DOWNLOAD_LOCATION} ABSOLUTE)

# `INACTIVITY_TIMEOUT` and not `TIMEOUT`: the bound is on SILENCE, so a slow but
# progressing download still completes however long it takes. See
# FetchTransferBound.cmake for where the seconds come from.
#
# `STATUS` rather than a bare call, because a `file(DOWNLOAD)` that fails
# without it is SILENT -- it writes a truncated file and configure continues to
# `include()` it, which reports as a CMake syntax error inside a file nobody
# wrote. `EXPECTED_HASH` already refuses a short read, but only when the
# download is believed to have finished; on a timeout it is the status that
# carries the reason, so the failure names the transfer rather than the
# consequence.
file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
    ${CPM_DOWNLOAD_LOCATION}
    EXPECTED_HASH SHA256=${CPM_HASH_SUM}
    INACTIVITY_TIMEOUT ${FASTCACHED_FETCH_SILENCE_SECONDS}
    STATUS cpmDownloadStatus
)
list(GET cpmDownloadStatus 0 cpmDownloadCode)
if(NOT cpmDownloadCode EQUAL 0)
    list(GET cpmDownloadStatus 1 cpmDownloadMessage)
    message(FATAL_ERROR
        "could not download the CPM.cmake bootstrap: ${cpmDownloadMessage}\n"
        "  from: https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake\n"
        "  into: ${CPM_DOWNLOAD_LOCATION}\n"
        "A transfer that delivered under ${FASTCACHED_FETCH_MIN_BYTES_PER_SECOND} byte(s)/second for "
        "${FASTCACHED_FETCH_SILENCE_SECONDS}s is abandoned rather than waited on -- see "
        "cmake/FetchTransferBound.cmake. Re-run the configure, or point CPM_SOURCE_CACHE at a "
        "directory that already holds the bootstrap.")
endif()

include(${CPM_DOWNLOAD_LOCATION})
