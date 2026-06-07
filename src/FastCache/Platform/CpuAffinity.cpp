// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/CpuAffinity.hpp>

#include <cstddef>

#if defined(__linux__)

    #include <pthread.h>
    #include <sched.h>
    #include <unistd.h>

namespace FastCache
{

bool PinCallingThreadToCpu(std::size_t coreIndex) noexcept
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(coreIndex, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

std::size_t OnlineCpuCount() noexcept
{
    auto const n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

} // namespace FastCache

#elif defined(__APPLE__)

    #include <pthread.h>
    #include <unistd.h>

    #include <mach/mach.h>
    #include <mach/thread_act.h>
    #include <mach/thread_policy.h>

namespace FastCache
{

bool PinCallingThreadToCpu(std::size_t coreIndex) noexcept
{
    // Darwin offers only an affinity *hint* (no hard pinning): threads sharing
    // an affinity tag are kept on the same L2 where possible. Use the core
    // index as the tag so each reactor lands in a distinct affinity set.
    thread_affinity_policy_data_t policy { static_cast<integer_t>(coreIndex) + 1 };
    auto const thread = ::pthread_mach_thread_np(::pthread_self());
    return ::thread_policy_set(
               thread, THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_AFFINITY_POLICY_COUNT)
           == KERN_SUCCESS;
}

std::size_t OnlineCpuCount() noexcept
{
    auto const n = ::sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

} // namespace FastCache

#elif defined(_WIN32)

    #include <windows.h>

namespace FastCache
{

bool PinCallingThreadToCpu(std::size_t coreIndex) noexcept
{
    if (coreIndex >= sizeof(DWORD_PTR) * 8)
        return false; // SetThreadAffinityMask only addresses bits in one group
    DWORD_PTR const mask = static_cast<DWORD_PTR>(1) << coreIndex;
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
}

std::size_t OnlineCpuCount() noexcept
{
    SYSTEM_INFO info {};
    ::GetSystemInfo(&info);
    return info.dwNumberOfProcessors;
}

} // namespace FastCache

#else

namespace FastCache
{

bool PinCallingThreadToCpu(std::size_t /*coreIndex*/) noexcept
{
    return false; // unsupported platform — caller treats this as "not pinned"
}

std::size_t OnlineCpuCount() noexcept
{
    return 0;
}

} // namespace FastCache

#endif
