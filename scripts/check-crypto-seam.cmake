# SPDX-License-Identifier: Apache-2.0
#
# No first-party file includes a Monocypher header except the Core units that ARE the seam:
# `src/FastCache/Core/Ed25519.cpp` and `src/FastCache/Core/X25519.cpp` (#178).
#
# ## Why a seam, and why a check rather than a build rule
#
# Monocypher is vendored (`vendor/monocypher`, vendor/VENDOR.md) to give every node an identity
# of its own: Ed25519 signatures and X25519 key agreement, over which the Raft handshake, the
# 0xFC node proof, the lease and the roster will all be rebuilt. Every one of those is a place a
# cryptographic primitive can be misused -- the default EdDSA over BLAKE2b called where RFC 8032
# Ed25519 was meant, a shared secret used raw instead of through HKDF, an all-zero X25519 result
# accepted, a seed passed to a function that wipes it. The Core units are where each of those is
# decided ONCE and checked against the RFC vectors. A second caller of Monocypher is a second
# place to get each of them wrong, and nothing about a green build says it was.
#
# The build cannot say it: the include directories are PUBLIC on `fastcache-monocypher`, as they
# must be for the two units to reach them, and FastCache links the library PRIVATE -- which keeps
# the headers from FastCache's CONSUMERS but not from any file inside FastCache itself. So it is a
# scan, in the default ctest set, like `ranges-seam`.
#
# ## What is scanned, and what counts as an include
#
# First-party C++, DERIVED rather than listed: `fastcached_first_party_cxx`, which is every
# tracked C++ source (`git ls-files`, or a directory walk where there is no index) minus the
# third-party roots of `scripts/lib/third-party-roots.txt`. Monocypher's own sources include each
# other and are declined, and counted as declined.
#
# An include is a `#include` or `#import` directive, in code with comments stripped
# (`fastcached_scan_code_lines`), naming a file whose LAST path component is `monocypher.h`,
# `monocypher-ed25519.h`, or either `.c`: a relative path into `vendor/` is the same crossing as
# the angled spelling, and including a `.c` compiles the implementation into another unit. Matched
# case-INSENSITIVELY, because a filesystem that is (Windows, macOS by default) resolves
# `<Monocypher.h>` to the same file. `MonocypherBytes.hpp`, the seam's own pointer bridge, is not a
# Monocypher header and is not matched.
#
# ## The permitted rows are the positive control
#
# Each permitted file must EXIST, be ENUMERATED, and be SEEN including a Monocypher header. A row
# that has stopped doing any of the three is refused as stale: that is what a broken pattern, a
# moved seam or an enumeration that skipped `src/` all look like, and each of them would otherwise
# report a clean tree over files it never read.
#
# ## Blind spots, stated
#
# - A header reached through a macro (`#include MONOCYPHER_HEADER`) is not seen. None exists.
# - A file that is not C++ by extension is not scanned; this project compiles no C of its own.
# - This guards WHO may call Monocypher, not whether those two call it correctly. That is what
#   the RFC vectors and the negative cases beside them are for.
#
# Runs as `cmake -P`. The verdict is `CMake Error` in the output, never the exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> [-DGIT_EXECUTABLE=<git>] -P scripts/check-crypto-seam.cmake

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# The files permitted past the seam, each with the reason it is one of them. `<path>|<reason>`.
set(FastCachedCryptoSeamUnits
    "src/FastCache/Core/Ed25519.cpp|RFC 8032 Ed25519 key pairs, signing and verification, over monocypher-ed25519.h"
    "src/FastCache/Core/X25519.cpp|RFC 7748 X25519 public keys and shared secrets, refusing the all-zero result"
)

set(permittedPaths "")
foreach(row IN LISTS FastCachedCryptoSeamUnits)
    fastcached_row_fields("${row}" permittedPath permittedReason)
    string(STRIP "${permittedReason}" permittedReason)
    if(permittedReason STREQUAL "")
        message(FATAL_ERROR
            "crypto-seam: the permitted row for ${permittedPath} gives no reason. A file allowed past "
            "the seam is a decision, and without its reason it reads exactly like one somebody forgot.")
    endif()
    list(APPEND permittedPaths "${permittedPath}")
    set("seenInclude_${permittedPath}" FALSE)
endforeach()

# What a Monocypher include looks like, over LOWERCASED text: the directive, an opening delimiter,
# any directories, and a last component that is one of Monocypher's four files.
set(includePattern "^[ \t]*#[ \t]*(include|import)[ \t]*[<\"]([^>\"]*/)?monocypher(-ed25519)?\\.[ch][>\"]")

# ---------------------------------------------------------------------------
fastcached_first_party_cxx("${FASTCACHED_SOURCE_DIR}" sourceFiles declinedFiles scanSource)
list(LENGTH declinedFiles declinedCount)
if(NOT sourceFiles)
    message(FATAL_ERROR
        "crypto-seam: the scan (${scanSource}) matched no first-party C++ source at all. That is not a "
        "clean tree but a scan that stopped working -- a moved source root, a FASTCACHED_SOURCE_DIR "
        "pointing elsewhere, or a third-party root that swallowed the tree.")
endif()
list(LENGTH sourceFiles fileCount)

set(violations "")
set(enumeratedPermitted "")
foreach(relative IN LISTS sourceFiles)
    if(relative IN_LIST permittedPaths)
        list(APPEND enumeratedPermitted "${relative}")
    endif()
    # The index can name a file deleted from the work tree and not yet staged.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    string(TOLOWER "${wholeFile}" lowered)
    # A whole-file test first; the line walk is the costly part and almost no file needs it.
    string(FIND "${lowered}" "monocypher" mentionAt)
    if(mentionAt EQUAL -1)
        continue()
    endif()
    fastcached_scan_code_lines("${lowered}" "${includePattern}" hits)
    foreach(hit IN LISTS hits)
        if(NOT hit MATCHES "^use:([0-9]+)$")
            continue()
        endif()
        if(relative IN_LIST permittedPaths)
            set("seenInclude_${relative}" TRUE)
        else()
            list(APPEND violations "${relative}:${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
set(stale "")
foreach(permittedPath IN LISTS permittedPaths)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${permittedPath}")
        list(APPEND stale "${permittedPath}: does not exist")
    elseif(NOT permittedPath IN_LIST enumeratedPermitted)
        list(APPEND stale "${permittedPath}: exists but the enumeration (${scanSource}) did not list it, so nothing says the rest of the tree was listed either")
    elseif(NOT seenInclude_${permittedPath})
        list(APPEND stale "${permittedPath}: was read and includes no Monocypher header -- the row excuses nothing, or the pattern has stopped matching the spelling it exists to find")
    endif()
endforeach()

if(violations OR stale)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}: includes a Monocypher header outside the crypto seam")
    endforeach()
    foreach(entry IN LISTS stale)
        message("  STALE permitted row -- ${entry}")
    endforeach()
    message("")
    message("Monocypher is reached through src/FastCache/Core/Ed25519.hpp, X25519.hpp and Hkdf.hpp,")
    message("and only Ed25519.cpp and X25519.cpp may include its headers. For a violation: call the")
    message("Core primitive instead, and if it does not offer what you need, ADD it there -- beside")
    message("the RFC vectors that check it -- rather than reaching past it. Adding a permitted row")
    message("to scripts/check-crypto-seam.cmake is a new seam unit, and needs the same vectors.")
    message("")
    message("A STALE row means a permitted file stopped including Monocypher, moved, or was never")
    message("enumerated. Fix the row, or find why the scan did not see what it is anchored on.")
    message("")
    message("NOT covered: a macro-spelled include, and whether the seam units use Monocypher correctly.")
    message("Enumerated ${fileCount} first-party C++ source(s) via ${scanSource}; declined ${declinedCount} under third-party roots.")
    list(LENGTH violations violationCount)
    list(LENGTH stale staleCount)
    message(FATAL_ERROR "crypto-seam: ${violationCount} include(s) past the seam, ${staleCount} stale permitted row(s)")
endif()

list(LENGTH permittedPaths permittedCount)
message(STATUS
    "crypto-seam: ${fileCount} first-party C++ source(s) via ${scanSource}, ${declinedCount} declined under "
    "third-party roots; ${permittedCount} of ${permittedCount} permitted unit(s) seen including Monocypher, "
    "none other does")
