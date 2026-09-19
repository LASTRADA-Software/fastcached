// SPDX-License-Identifier: Apache-2.0
//
// Ends the process at a Catch2 assertion made on a thread that is not the one running its
// test case (#1211).
//
// ## Why
//
// Catch2's assertions are not thread-safe. A `REQUIRE` on a helper thread writes the same
// run and reporter state the case's thread is writing, and under a cumulative reporter
// every assertion is appended to the current section's node. That is what #1211 was:
// `FrameEndpoint_test`'s `Exchange` asserted on three `std::async` threads at once, the
// JUnit reporter's node for that case was damaged, and the damage surfaced minutes later
// somewhere else -- a SIGSEGV inside `JunitReporter::writeAssertion` at run end, a
// `free(): invalid pointer`, or glibc printing `malloc(): invalid size (unsorted)` and then
// wedging inside `abort` for good. Measured before the fix: 5 of 18 runs of `[frame]` on
// Linux and 10 of 20 on Windows under the JUnit reporter, and none under the console
// reporter or ctest's one-case-per-process registration, which is how every gate ran it.
//
// Nothing else could see it. A crash names where the damage was FOUND, never who did it.
// And ThreadSanitizer cannot: Catch2 is built without the sanitizer here (measured: a
// clang-tsan build's `libCatch2d.a` references no `__tsan_func_entry`), so a race inside
// its state is in code TSan never observes.
//
// ## Why a listener, and why it ends the process
//
// Catch2 3.6 announces every assertion, passing or not, through `assertionStarting` on the
// thread making it and whatever reporter is attached -- so this hook sees each one before it
// touches any shared state. Reporting after the fact would be too late: by then the state
// is already damaged, and the report would be written through it. So the process ends at
// the offending assertion, naming the macro, where it is and the case, on every run and
// under every reporter. That turns a crash a few percent of runs somewhere else into a
// failure every run at the line to fix.
//
// SIGABRT's disposition is reset first. Catch2's own fatal-condition handler would
// otherwise report the abort through the very state this guard protects, from the same
// wrong thread.
//
// ## What it cannot see
//
// Only ASSERTIONS reach `assertionStarting`. `INFO`, `CAPTURE`, `UNSCOPED_INFO` and
// `SECTION` on a helper thread race the same state through other entry points, and this
// guard is blind to them. Its silence says nothing about those.
//
// ## How it arrives
//
// As an INTERFACE source of the Catch2 target (the top-level `CMakeLists.txt`), so every
// executable that links Catch2 compiles it through the link itself and no list can leave
// one out. `ctest -R off-thread-assertion-guard-coverage` reads the generated link lines,
// and `ctest -R off-thread-assertion-canary` watches it accept an assertion on the case's
// thread and end the process at one off it.
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace
{

/// Ends the process at an assertion made off the thread running its test case.
class OffThreadAssertionGuard final: public Catch::EventListenerBase
{
  public:
    using EventListenerBase::EventListenerBase;

    /// Remember which thread runs this case, and its name for the message.
    /// @param info The case starting.
    void testCaseStarting(Catch::TestCaseInfo const& info) override
    {
        // The name is the registry's own string, which lives for the whole run, so a
        // pointer to it is what a helper thread may read without copying anything.
        _caseName.store(&info.name, std::memory_order_release);
        _caseThread.store(std::this_thread::get_id(), std::memory_order_release);
    }

    /// End the process when @p info is being asserted on any other thread.
    /// @param info The assertion about to be evaluated.
    void assertionStarting(Catch::AssertionInfo const& info) override
    {
        if (std::this_thread::get_id() == _caseThread.load(std::memory_order_acquire))
            return;

        auto const* const name = _caseName.load(std::memory_order_acquire);
        std::println(std::cerr,
                     "\nfatal (#1211): {} at {}:{} ran on a thread that is not the one running test case \"{}\".\n"
                     "Catch2's assertions are not thread-safe: one made off the case's thread races the "
                     "reporter's state and damages the heap, which surfaces later as a crash or a hang somewhere "
                     "else. Return what the helper thread observed -- or keep it with OffThreadWaits::Keep "
                     "(src/tests/BoundedWait.hpp) -- and assert it on the case's thread after .get() or .join().",
                     std::string_view { info.macroName.data(), info.macroName.size() },
                     info.lineInfo.file,
                     info.lineInfo.line,
                     name != nullptr ? std::string_view { *name } : std::string_view { "(none started)" });
        std::cerr.flush();
        std::signal(SIGABRT, SIG_DFL);
        std::abort();
    }

  private:
    std::atomic<std::thread::id> _caseThread {};
    std::atomic<std::string const*> _caseName { nullptr };
};

} // namespace

CATCH_REGISTER_LISTENER(OffThreadAssertionGuard)
