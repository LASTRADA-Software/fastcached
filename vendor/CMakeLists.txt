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
    endo/tui/runtime/PollEventSource.cpp
    endo/tui/runtime/TuiRuntime.cpp
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
    endo/tui/runtime/platform/TerminalEventSourceWin32.cpp
)
set(_fcTuiPosixSources
    endo/tui/platform/Terminal.cpp
    endo/tui/platform/TerminalInput.cpp
    endo/tui/platform/TerminalOutput.cpp
    endo/tui/runtime/platform/TerminalEventSourcePosix.cpp
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

# `endo::platform::SignalHandler`, which `runtime/` calls into. ONE file for every
# platform rather than Wakeup's three, because upstream puts the `#if` inside the
# implementation instead of selecting between files -- so this is unconditional and
# the difference from the block above is upstream's shape, not a decision here.
#
# It was missing from the first two imports. Every member of the class is `static`
# and the header declares them all without defining any, so every call site compiled
# and the gap was invisible until something linked: two undefined symbols on Windows,
# three on POSIX. See VENDOR.md's closure paragraph -- the twelve headers are a
# measured closure and the implementation files beside them are hand-picked, which is
# the part that has now been wrong twice.
list(APPEND _fcTuiSources endo/platform/SignalHandler.cpp)

# Every vendored translation unit this target does NOT build, and why. Three
# groups, and the third is the one that needs stating: "nothing reached it" is a
# real reason, but it is indistinguishable from "nobody looked" unless it is
# written down.
#
#   stb_image   image decoding, which a stats view does none of. The sources stay
#               on disk verbatim; only the target is subset.
#   unreached   an interactive editor's widgets. Present, working, and simply not
#               on any path from a stats panel.
#
# There were THREE groups until #1374, and the third was `coro`: runtime/ excluded so
# the tree would not carry a second coroutine library and a second clock. runtime/ is
# now BUILT, and both halves of why are worth keeping, because one of them was false.
#
# The false half, deleted rather than reworded: this file used to say
# `EventSource::wait(int timeoutMs)` "is a poll with a timeout -- the blocking design
# that loop exists not to be". It is a blocking readiness wait, structurally what
# `EpollReactor::RunLoop` is. EventSource.hpp's own parameter doc reads "-1 to block
# indefinitely, 0 to poll, >0 to wait that many ms"; the POSIX implementation is one
# `::poll(fds, n, timeoutMs)` over a descriptor set with no re-check loop, and the
# Win32 one is `WaitForMultipleObjects(..., FALSE, timeout)`. `timeoutMs` is the timer
# deadline, not a re-check interval. The comment read the `poll` in `PollEventSource`
# as the polling PATTERN when it is the syscall's NAME -- and that wrong sentence cost
# real work: it told two readers that endo's runtime was a polling design needing
# replacement, and one of them wrote an actual `co_await SleepUntil(now + interval)`
# loop to replace a readiness-driven one.
#
# The true half, which stands and is now a DECISION rather than an obstacle: runtime/
# is the only thing reaching `coro/Task.hpp`, `coro/Cancellation.hpp` and
# `platform/Clock.hpp`, so building it does put a second coroutine type and a second
# clock beside `Async/Task` and `IClock`. That cost is accepted deliberately. It buys
# the only descriptor-parking coroutine API available: `IReactor` has none -- `Stop`,
# two `Schedule(deadline, ...)` overloads, `CancelPending`, `Clock`, `RunLoop` -- and
# readiness reaches it only through `ISocket` (#1372). The alternative was an
# `EventSource` backed by our reactor, which needs #1372 closed across epoll, IOCP and
# kqueue first and then a blocking `wait()` built over a run loop that wants to be the
# outer loop; `RunLoop` is unbounded and `Stop` only posts a wakeup, so there is no
# bounded "run one turn" to build it from, leaving a reactor thread plus a condvar --
# cross-thread coroutine resumption, which `IocpReactor.hpp` names unsafe.
#
# So the choice was made on FAILURE MODE, not on size: this direction fails as a
# compile error or a visibly wrong dashboard, the other fails as a coroutine resumed
# on the wrong thread under IOCP -- green everywhere, wrong in production. The clock
# half of the duplication is nearly free, because `endo::platform::SteadyTimePoint`
# and `FastCache::TimePoint` are both `std::chrono::steady_clock::time_point`, so our
# own `ManualClock` drives `TuiRuntime` through a forwarder.
#
# QuestionComponent.cpp moved to `unreached` rather than leaving the list: it was
# excluded for reaching coro through runtime/Modal.hpp, which has stopped being a
# reason, and nothing we build reaches it -- only itself and its own test. It is still
# forbidden, for a reason that is true and checkable now.
set(_fcTuiNotBuilt
    # stb_image
    ImageLoader.cpp ImageProvider.cpp StbImageImpl.cpp
    # unreached
    CommandPalettePopup.cpp CommandRegistry.cpp CompletionPopup.cpp Dialog.cpp
    FuzzyPickerPopup.cpp
    InputField.cpp KeyBindings.cpp List.cpp LogPanel.cpp
    QuestionComponent.cpp
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
endforeach()

# THE runtime/ GUARD IS GONE, and this paragraph is what replaces it, because a
# deleted guard with no account of where its property went is how a decision gets
# reversed for free -- which is the exact failure the guard was written against.
#
# It refused any source under endo/tui/runtime/. #1374 decided runtime/ IS built, so
# its premise is false and it had to go: a guard that refuses what the tree now does
# cannot be reworded into usefulness, and contorting it to refuse something it cannot
# see would be worse -- a guard that cannot fire reads exactly like one that does.
#
# What it was really protecting is NOT "runtime/ is unbuilt". It is "the second
# coroutine vocabulary does not spread into first-party code": nothing under src/ may
# include <coro/...> or <platform/Clock.hpp>, which is VENDOR.md's rule and is still
# in force. That property is unguarded and this file cannot guard it, for a reason
# worth writing down rather than rediscovering:
#
#   the include root below is `endo/`, exposed PUBLIC, and it necessarily carries
#   tui/, coro/, platform/, crispy/ and testing/ together. So <coro/Task.hpp> is
#   reachable by construction from anything linking fastcache-tui, and narrowing the
#   root is not available -- it would hide <tui/...> from the consumers that need it.
#   The leak is therefore a property of src/, not of this target's source list, and
#   the loop above cannot see src/ at all.
#
# Measured when the guard was removed: src/ reaches none of it today (a grep for
# `#include <coro/` and `#include <platform/Clock.hpp>` over src/ returns nothing,
# with the vendored runtime/ headers as the positive control that the pattern
# matches what it is meant to match, ~50ms). So this is an open hazard with no
# current violation, not a live defect -- filed rather than left here, because a
# comment nothing checks cannot fail.

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
