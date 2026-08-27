# SPDX-License-Identifier: Apache-2.0
# Utf8CodePage.cmake - what encoding a `char` is on Windows.
#
# Windows keeps command lines, environment blocks and paths as UTF-16 and
# transcodes them for a narrow caller through the process's ACTIVE CODE PAGE. That
# one setting therefore decides `argv`, `getenv`, every `...A` API this tree calls
# (CreateProcessA in the launcher's process runner, CreateServiceA and
# GetModuleFileNameA in ServiceControl, RegQueryValueExA in Registry) and
# `std::filesystem::path`'s narrow conversions in BOTH directions -- MSVC's
# `__std_fs_code_page()` answers CP_ACP unless the CRT locale is UTF-8.
#
# Its default is the host's legacy code page, 1252 on a Western install. Since
# #141 the fleet refuses a registration whose fingerprint or endpoint is not valid
# UTF-8, so `--toolchain=gruen=...` spelled with a real umlaut arrived as one
# CP-1252 byte, was refused, and the node logged the refusal every heartbeat while
# never joining the fleet -- for a reason invisible from where it was typed
# (issue #155).
#
# ## Why a manifest and not a conversion at the boundary
#
# The obvious fix is GetCommandLineW + CommandLineToArgvW + WideCharToMultiByte in
# a seam every main() calls. It converts ONE boundary and leaves the rest on the
# legacy code page, which turns a wrong encoding into a SPLIT one: UTF-8 argv fed
# to `std::filesystem::path` decodes as CP-1252 and names a different file, and
# fed to CreateProcessA spawns the compiler with a mangled command line. Both work
# today. Closing that gap means policing a second convention across 45 path
# constructions, 122 `.string()` calls and six `...A` call sites, forever, with no
# compiler enforcement.
#
# Declaring the code page instead makes ONE convention true process-wide, with no
# conversion, no wide entry point, no dependency, and no call site to remember.
#
include(ProjectTargets)

# ## The floor, and what happens below it
#
# `activeCodePage` is honoured from Windows 10 1903 / Server 2022. An older host
# ignores it in SILENCE, which is the one thing a build-time setting must not be
# allowed to be -- so `FastCache::NarrowTextIsUtf8()` reports the outcome rather
# than the intent, and a Catch2 case asserts it. See
# src/FastCache/Platform/NarrowText.hpp.

# Attach the manifest to every executable this project defines.
#
# Applied by walking the build system rather than by a line in each of the ten
# add_executable() sites: an executable that missed one would still build, still
# run, and still be wrong only for the operator who typed a non-ASCII argument at
# it. Doing it here means a new binary is covered by existing rather than by
# somebody remembering.
#
# Call once, after every add_subdirectory() -- fastcached_collect_executables()
# reads the build system as it stands, so a target added later is a target left
# out.
function(fastcached_declare_utf8_code_page)
    if(NOT WIN32)
        return()
    endif()

    # MSVC-like only, which covers cl and clang-cl (CMake sets MSVC for both, and
    # both were verified to embed this). CMake's manifest machinery has no MinGW
    # path, and a warning rather than a hard error because refusing to configure
    # would make this tree unbuildable there over an encoding default. The build
    # still says so twice: this line, and the NarrowText case going red.
    if(NOT MSVC)
        message(WARNING
            "[Utf8CodePage] ${CMAKE_CXX_COMPILER_ID} on Windows has no manifest support here, so this "
            "build's executables keep the host's legacy code page: a non-ASCII --toolchain, --advertise "
            "or --node-id will not reach the fleet as UTF-8. See cmake/Utf8CodePage.cmake.")
        return()
    endif()

    fastcached_collect_executables("${CMAKE_SOURCE_DIR}" executables)

    # Not an error, unlike the same check in cmake/Coverage.cmake: coverage needs a
    # suite to measure, while this needs only whatever was built, and a configure
    # with every FASTCACHED_BUILD_* off legitimately builds no executable at all.
    # Said out loud regardless -- if the call ever moves ahead of the
    # add_subdirectory() calls that define the targets, this line is the only thing
    # that would say so.
    if(NOT executables)
        message(STATUS "[Utf8CodePage] no executables under src/; nothing to declare")
        return()
    endif()

    # Resolved from this file's own directory rather than from a variable set at
    # include time, so the answer does not depend on the scope the call is made in.
    set(manifest "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows/utf8.manifest")

    foreach(executable IN LISTS executables)
        target_sources(${executable} PRIVATE "${manifest}")
    endforeach()

    list(JOIN executables ", " declared)
    message(STATUS "[Utf8CodePage] UTF-8 process code page declared by: ${declared}")
endfunction()
