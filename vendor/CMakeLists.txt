# SPDX-License-Identifier: Apache-2.0
#
# The vendored endo TUI, built as one static library for fastcache-cli's
# `live-stats` view (#134).
#
# WHY THIS FILE EXISTS AT ALL, rather than `add_subdirectory(endo/tui)`:
# `endo/tui/CMakeLists.txt` is part of the VERBATIM copy and is deliberately not
# used. It names `endo-platform`, `coro`, `stb_image` and three endo-only helper
# functions (`set_pedantic_compiler_warnings`, `enable_sanitizers`,
# `enable_clang_tidy`), none of which exist here. Editing it to suit us would put
# a local change inside the contribution diff on day one, which is the whole thing
# `vendor/VENDOR.md` says we must not do. So it stays untouched and unread, and
# this file -- which is OURS -- states the target in fastcached's own terms.

# ----------------------------------------------------------------------------
# The source set, which is a CLOSURE rather than a hand-picked list: the
# transitive include closure of the components #134 needs, plus the platform
# terminal implementations that supply `tui/Terminal.hpp`,
# `tui/TerminalInput.hpp` and `tui/TerminalOutput.hpp` (those live under
# `platform/`, so they are not the `.cpp` sibling of any header in the closure
# and a closure walk alone misses them).
#
# Every other vendored translation unit is named in `_fcTuiNotBuilt` below with
# the reason it is not built, and the two lists are asserted to PARTITION the
# tree. That is not bookkeeping: the first version of this file carried the
# counts in prose, said "40 upstream, 19 left out", and both numbers were wrong
# -- upstream's target is 48 per platform and 27 were left out. `HyperlinkEmitter`
# appeared in no list and no rationale at all, which is precisely what an
# unchecked hand count hides. A total stated beside a list is derived from it or
# it is a second claim, and the second claim is the one that rots.
set(_fcTuiSources
    endo/tui/Box.cpp
    endo/tui/Buffer.cpp
    endo/tui/Canvas.cpp
    endo/tui/Cell.cpp
    endo/tui/Component.cpp
    endo/tui/GenericSyntaxHighlighter.cpp
    endo/tui/HoverState.cpp
    endo/tui/HyperlinkEmitter.cpp
    endo/tui/MarkdownHtml.cpp
    endo/tui/MarkdownRenderer.cpp
    endo/tui/MarkdownTable.cpp
    endo/tui/MockTerminalOutput.cpp
    endo/tui/Screen.cpp
    endo/tui/SgrBuilder.cpp
    endo/tui/Sixel.cpp
    endo/tui/StyledText.cpp
    endo/tui/Text.cpp
    endo/tui/Theme.cpp
    endo/tui/Tooltip.cpp
    endo/tui/TreeTableView.cpp
    endo/tui/Unicode.cpp
    endo/tui/VtParser.cpp
    endo/tui/platform/TerminalShared.cpp
)

# The platform terminal implementations. Named as BOTH sets regardless of which
# host is configuring, because the partition below is a statement about the vendored
# TREE and the tree is the same everywhere -- a file built only on Windows is still
# classified on Linux. Splitting on `if(WIN32)` alone would leave the other
# platform's three unaccounted for, and the assertion would refuse every build on
# both hosts. (It did, which is how this shape was arrived at.)
set(_fcTuiWin32Sources
    endo/tui/platform/TerminalWin32.cpp
    endo/tui/platform/TerminalInputWin32.cpp
    endo/tui/platform/TerminalOutputWin32.cpp
)
set(_fcTuiPosixSources
    endo/tui/platform/Terminal.cpp
    endo/tui/platform/TerminalInput.cpp
    endo/tui/platform/TerminalOutput.cpp
)

if(WIN32)
    list(APPEND _fcTuiSources ${_fcTuiWin32Sources})
else()
    list(APPEND _fcTuiSources ${_fcTuiPosixSources})
endif()

# `endo::platform::Wakeup`, which `TerminalInput.cpp` calls into on both platforms.
# Not part of the tui tree and not header-only: Wakeup.hpp declares the class and
# the three files below define it, one per platform, exactly as upstream selects
# them. Linux gets an eventfd, other UNIX a self-pipe, Windows a manual-reset Event.
if(WIN32)
    list(APPEND _fcTuiSources endo/platform/windows/WindowsWakeup.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _fcTuiSources endo/platform/linux/LinuxWakeup.cpp)
else()
    list(APPEND _fcTuiSources endo/platform/posix/PosixWakeup.cpp)
endif()

# Every vendored translation unit this target does NOT build, and why. Three
# groups, and the third is the one that needs stating: "nothing reached it" is a
# real reason, but it is indistinguishable from "nobody looked" unless it is
# written down.
#
#   stb_image   image decoding, which a stats view does none of. The sources stay
#               on disk verbatim; only the target is subset.
#   coro        the event runtime. UPSTREAM'S OWN TARGET COMPILES runtime/ --
#               TuiRuntime.cpp and PollEventSource.cpp unconditionally, plus a
#               per-platform TerminalEventSource -- so leaving it out is a decision
#               this file is making, not a property of the vendored code, and beside
#               upstream's build file it reads like an oversight.
#
#               The decision: runtime/ is the only thing that reaches `coro/Task.hpp`,
#               `coro/Cancellation.hpp` and `platform/Clock.hpp`. Compiling it would
#               put a SECOND coroutine library and a SECOND clock into a tree whose
#               central rule is one source of truth per concept -- the tree that says
#               there is exactly one TCP client and not to write a second. fastcached
#               has `Async/Task`, `Async/Cancellation` and `IClock`, and the vendored
#               TUI reaches them through one adapter layer instead.
#
#               SECOND REASON, and it is the stronger one -- recorded because the
#               first would not survive its own resolution. The dependency argument
#               above says runtime/ is not BUILDABLE here; a later re-sync that
#               settles the coro question could then build it in good faith and be
#               right about everything this comment said. But `live-stats` runs a
#               COROUTINE-NATIVE event loop of ours, and `runtime/`'s
#               `EventSource::wait(int timeoutMs)` is a poll with a timeout -- the
#               blocking design that loop exists not to be. So runtime/ is REPLACED,
#               not merely unbuildable, and it stays out even once its dependencies
#               would link. We vendored the rendering surface and left the event
#               loop behind, deliberately.
#
#               A decision that is load-bearing for two reasons and records one is a
#               cleanup away from being reversed for free.
#
#               That is arguable, and someone may later decide the adapter is worse
#               than the duplication. What is not acceptable is the change happening
#               by accident, so the guard below refuses it rather than leaving a
#               sentence for a reader who has already stopped reading.
#               QuestionComponent is in this group rather than `unreached` because it
#               reaches coro through runtime/Modal.hpp.
#   unreached   an interactive editor's widgets. Present, working, and simply not
#               on any path from a stats panel.
set(_fcTuiNotBuilt
    # stb_image
    ImageLoader.cpp ImageProvider.cpp StbImageImpl.cpp
    # coro
    QuestionComponent.cpp
    runtime/PollEventSource.cpp runtime/TuiRuntime.cpp
    runtime/platform/TerminalEventSourcePosix.cpp
    runtime/platform/TerminalEventSourceWin32.cpp
    # unreached
    CommandPalettePopup.cpp CommandRegistry.cpp CompletionPopup.cpp Dialog.cpp
    FuzzyPickerPopup.cpp
    InputField.cpp KeyBindings.cpp List.cpp LogPanel.cpp
    SemanticBlockClient.cpp Spinner.cpp StatusBar.cpp
    completer/Completer.cpp completer/FuzzyMatch.cpp completer/SmartCaseMatch.cpp
)

# The partition, asserted rather than believed.
#
# Both directions, because they fail differently. A named source that is GONE is
# caught by CMake at generate time anyway (`Cannot find source file:`), so what
# this adds there is the remedy sentence, not the filename. The other direction is
# the one nothing else covers: a re-sync that ADDS a file leaves it built by
# nothing, classified by nothing and mentioned nowhere -- exactly how
# `HyperlinkEmitter` went unnoticed -- and the build stays green.
#
# Tests are excluded from the partition because this target builds none: endo's
# test binary is upstream's, and fastcached's own suite is the one that matters
# here.
file(GLOB_RECURSE _fcTuiOnDisk RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/endo/tui"
     "${CMAKE_CURRENT_SOURCE_DIR}/endo/tui/*.cpp")
list(FILTER _fcTuiOnDisk EXCLUDE REGEX "(_test|test_main)\\.cpp$")

# `NOT <var>` and never `<var> STREQUAL ""`: an APPEND into a misspelled name
# leaves the variable UNSET, where STREQUAL compares against the literal string
# and does not fire. That trap is in AGENT.md, and the first draft of this file
# fell into it.
if(NOT _fcTuiOnDisk)
    message(FATAL_ERROR
        "vendor: no vendored translation units were found under endo/tui, so the "
        "partition below would be taken over an empty set and agree perfectly.")
endif()

# Classified = not-built + built-here + built-on-the-other-platform. The third term
# is what makes this a statement about the tree rather than about this host.
set(_fcTuiClassified ${_fcTuiNotBuilt})
foreach(_src IN LISTS _fcTuiSources _fcTuiWin32Sources _fcTuiPosixSources)
    # Only the `endo/tui/` half: the partition is a statement about that tree, and
    # `_fcTuiSources` also carries the Wakeup implementation from `endo/platform/`,
    # which lives outside it. Without the filter those entries survive the REPLACE
    # unchanged and the existence check below looks for `endo/tui/endo/platform/...`.
    if(_src MATCHES "^endo/tui/")
        string(REPLACE "endo/tui/" "" _rel "${_src}")
        list(APPEND _fcTuiClassified "${_rel}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _fcTuiClassified)

foreach(_src IN LISTS _fcTuiOnDisk)
    # The partition covers endo/tui only; the Wakeup sources sit outside that tree
    # and are named directly above, so they are not candidates here.
    if(NOT _src IN_LIST _fcTuiClassified)
        message(FATAL_ERROR
            "vendor: endo/tui/${_src} is in the vendored tree but appears in neither "
            "_fcTuiSources nor _fcTuiNotBuilt. An upstream re-sync added it. Decide "
            "which it is -- build it, or name it in _fcTuiNotBuilt with the reason -- "
            "because a source that is classified by nothing is built by nothing and "
            "mentioned nowhere, and the build stays green either way.")
    endif()
endforeach()

foreach(_src IN LISTS _fcTuiSources)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
        message(FATAL_ERROR
            "vendor: ${_src} is built by this target but is not in the tree. "
            "If this followed an upstream re-sync, the file moved or was removed "
            "there: update the list and record the change in vendor/VENDOR.md.")
    endif()
    # The runtime/ guard. Folded into the loop the source list already walks, rather
    # than written as a separate scan, because a guard riding on something the site
    # must do cannot be forgotten -- adding a source is what trips it.
    if(_src MATCHES "^endo/tui/runtime/")
        message(FATAL_ERROR
            "vendor: ${_src} is under endo/tui/runtime/, which this target does not "
            "build.\n\n"
            "runtime/ is the ONLY thing in the vendored tree that reaches coro/Task.hpp, "
            "coro/Cancellation.hpp and platform/Clock.hpp. Building it puts a second "
            "coroutine library and a second clock into a tree that has one of each -- "
            "FastCache::Async::Task, FastCache::Async::Cancellation and FastCache::IClock "
            "are the authoritative ones, and vendor/VENDOR.md says so in the words a "
            "reader of this refusal needs.\n\n"
            "Upstream's own src/tui/CMakeLists.txt DOES compile runtime/, so this "
            "exclusion looks like an oversight beside it and that is exactly why it is "
            "guarded rather than merely commented.\n\n"
            "AND THERE IS A SECOND REASON, which outlives the first: `live-stats` runs a "
            "COROUTINE-NATIVE event loop, and runtime/'s EventSource::wait(int timeoutMs) "
            "is a poll with a timeout -- the blocking design that loop exists not to be. "
            "So runtime/ is REPLACED rather than merely unbuildable, and it stays out even "
            "once the coro dependency question is settled and it WOULD link. If you are "
            "reading this because you just solved that question: solving it is not "
            "sufficient.\n\n"
            "If the adapter layer has turned out worse than the duplication, that is a "
            "decision worth making -- make it deliberately: delete this guard, say why "
            "in vendor/VENDOR.md's \"Two of some things\", and vendor the coro and "
            "platform/Clock sources the runtime needs, which this import does not carry "
            "as built sources. Adding the row alone will not link.")
    endif()
endforeach()

foreach(_src IN LISTS _fcTuiClassified)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/endo/tui/${_src}")
        message(FATAL_ERROR
            "vendor: endo/tui/${_src} is named in this file but is not in the tree. "
            "If this followed an upstream re-sync, the file moved or was removed there: "
            "update the list and record the change in vendor/VENDOR.md, rather than "
            "deleting the line, which would silently drop whatever used it.")
    endif()
endforeach()

# Compiled ONCE, linked twice -- as the static library the CLI will use, and as a
# shared library that exists only to make the linker check the source set.
add_library(fastcache-tui-objects OBJECT ${_fcTuiSources})
add_library(fastcache-tui STATIC $<TARGET_OBJECTS:fastcache-tui-objects>)

# The include root is `vendor/endo`, which is what makes the copy verbatim
# possible: upstream spells its own includes `<tui/...>`, `<platform/...>` and
# `<crispy/...>`, so reproducing endo's `src/` layout under one root resolves
# every one of them with no edit to any vendored file.
#
# SYSTEM, so warnings raised inside these headers do not reach the first-party
# translation units that include them. This tree builds with -Wconversion -Wextra
# and PEDANTIC_COMPILER_WERROR ON for Windows presets, and third-party headers
# are not ours to fix.
target_include_directories(fastcache-tui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/endo")

target_link_libraries(fastcache-tui PUBLIC unicode::unicode)
target_compile_features(fastcache-tui PUBLIC cxx_std_23)

# `/utf-8` on MSVC, and this is CORRECTNESS rather than warning policy -- the
# distinction matters because the symptom arrives as a warning and the tempting
# repair is to silence it.
#
# `cmake/Utf8CodePage.cmake` applies this flag directory-wide AFTER every
# third-party fetch, deliberately, "so a third-party source with a byte this would
# refuse is never handed the flag". That is right for a dependency of unknown
# encoding. It is wrong for THIS one: the vendored TUI is UTF-8 and its whole job
# is drawing box characters.
#
# Without the flag MSVC reads these sources in the host ANSI code page and
# re-encodes their narrow literals into it. Measured with cl.exe on a CP-1252
# host, ~50 diagnostics in Box.cpp alone:
#
#     Box.cpp(29): warning C4566: character represented by universal-character-name
#     '─' cannot be represented in the current code page (1252)
#
# `─` is BOX DRAWINGS LIGHT HORIZONTAL. So the failure is not the warning --
# it is that every frame, gauge and sparkline glyph would be substituted on the way
# in, and a TUI whose borders are question marks would ship on Windows with nothing
# but a warning nobody reads. Under PEDANTIC_COMPILER_WERROR (the default on the
# Windows presets) it is at least loud; suppressing C4566 would make it silent and
# WRONG, which is the worse of the two.
#
# Per TARGET, never `add_compile_options`: this says the vendored TUI is UTF-8, and
# it must not become a claim about every other third-party source in the build,
# which is the claim Utf8CodePage.cmake declines to make.
if(MSVC)
    target_compile_options(fastcache-tui-objects PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# The Windows macro trio, for the reason `src/apps/fastcache-cc/CMakeLists.txt`
# already states: they are PUBLIC on the FastCache target and therefore NOT
# INHERITED by a target that does not link it. This one does not.
#
# `NOMINMAX` is the one that bites here and it bites in vendored code, so it is
# worth naming what it costs. `vendor/endo/platform/Types.hpp` includes
# <windows.h>, which `tui/TerminalInput.hpp` pulls in, which `Screen.cpp` reaches
# -- and <windows.h> defines `min` and `max` as MACROS. `std::min(...)` then
# preprocesses to `std::(...)`, and MSVC reports:
#
#     Screen.cpp(563): error C2589: '(': illegal token on right side of '::'
#
# Measured: 17 errors across Screen.cpp alone, none of which names a macro, and
# the first one masks the rest -- a CI error line is a lower bound. The vendored
# sources are correct C++ and must not be touched; the definition is what was
# missing.
#
# WIN32_LEAN_AND_MEAN and _WIN32_WINNT ride along rather than waiting to be needed
# separately: the trio is how this tree spells "this target includes <windows.h>",
# and splitting it would leave the next vendored Win32 translation unit to
# rediscover the winsock redefinition that comment describes.
if(WIN32)
    target_compile_definitions(fastcache-tui-objects PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

# Belt AND braces, deliberately.
#
# This directory is added BEFORE `include(PedanticCompiler)` / `include(ClangTidy)`
# in the top-level CMakeLists, which is the mechanism yaml-cpp, Catch2, zstd and
# lz4 already rely on -- `add_compile_options()` and `CMAKE_CXX_CLANG_TIDY` are
# directory-scoped and reach only what is defined after them. That ordering is a
# property of a line in ANOTHER file, though, and nothing here would notice if it
# moved: the analyser would simply start failing the build inside upstream code,
# with `WarningsAsErrors: "*"` and naming rules this code has no reason to satisfy.
# So the target says it for itself as well, where a reader of THIS file can see it.
#
# `CXX_SCAN_FOR_MODULES OFF` rides along for the same reason, and pays twice. No
# vendored source declares or imports a module, so the scan is 21 edges of pure
# waste -- measured at ~24% of this target's build cost in a reference build,
# which is where it lands, since a compiler launcher turns scanning off globally
# anyway. It also removes three `unknown argument` errors (`-fdeps-format`,
# `-fmodule-mapper`, `-fmodules-ts`) that any clang-tidy invocation over these
# TUs would otherwise report before it got as far as the code.
set_target_properties(fastcache-tui-objects fastcache-tui PROPERTIES
    CXX_CLANG_TIDY ""
    C_CLANG_TIDY ""
    CXX_SCAN_FOR_MODULES OFF
)
target_include_directories(fastcache-tui-objects SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/endo")
target_link_libraries(fastcache-tui-objects PUBLIC unicode::unicode)
target_compile_features(fastcache-tui-objects PUBLIC cxx_std_23)

# ----------------------------------------------------------------------------
# THE LINK-CLOSURE PROBE, and it is not decoration.
#
# The source set above is a CLOSURE, and the first version of it was derived by
# reading `#include` lines. That produced a set with FIFTEEN unresolved `tui::`
# symbols -- `HyperlinkEmitter`, `MarkdownTable`, `GenericSyntaxHighlighter` and
# `MarkdownRenderer::defaultTheme`, all called from translation units that WERE
# built. Every one of them compiled. `ninja` was green, the partition assertion
# above was green, and `vendor-verbatim` was green, because a STATIC ARCHIVE NEVER
# RESOLVES SYMBOLS: it is a bag of objects, and the complaint is deferred to
# whoever links it. The first consumer would have met a wall of link errors naming
# upstream symbols, one ticket later, with nothing here to explain why.
#
# A header graph and a symbol graph are different graphs. So the question is put to
# the LINKER rather than to a reader: a shared library over the same objects must
# resolve everything, and every platform this project builds on fails that link by
# default when it cannot -- `--no-undefined` on ELF, `-undefined error` on Mach-O,
# and a DLL link on Windows, where unresolved externals are already fatal.
#
# It costs one link, not one compile: the objects are shared with the static
# library through `$<TARGET_OBJECTS:>`. `EXCLUDE_FROM_ALL` is deliberately NOT set
# -- a check that only runs when somebody names it is a check that runs never.
#
# An EXECUTABLE over the objects, not a shared library. The shared-library form was
# tried first and it works on ELF only by accident: libunicode builds its static
# libraries without -fPIC, so the link dies on a relocation error before it can say
# anything about the vendored objects -- a probe that fails for a reason having
# nothing to do with its subject. An executable needs no PIC, and linking the
# OBJECTS directly (rather than the archive) is what makes every one of them
# participate: an archive contributes only the members something references, which
# is precisely the leniency being tested for.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/linkprobe.cpp"
     "// Generated. See vendor/CMakeLists.txt: this exists so the linker is made to\n"
     "// resolve every symbol the vendored objects reference. It is never run.\n"
     "int main() { return 0; }\n")
add_executable(fastcache-tui-linkprobe
    "${CMAKE_CURRENT_BINARY_DIR}/linkprobe.cpp"
    $<TARGET_OBJECTS:fastcache-tui-objects>
)
target_link_libraries(fastcache-tui-linkprobe PRIVATE unicode::unicode)
set_target_properties(fastcache-tui-linkprobe PROPERTIES
    CXX_CLANG_TIDY ""
    C_CLANG_TIDY ""
    CXX_SCAN_FOR_MODULES OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/linkprobe"
)
