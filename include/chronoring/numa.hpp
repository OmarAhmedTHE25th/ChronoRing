#pragma once

#include <cstdint>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace chronoring {

enum class NumaPolicy : std::uint8_t {
    Default = 0,
    BindLocalNode,
    Interleave
};

[[nodiscard]] inline std::uint32_t available_cpu_count() noexcept {
    const unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : static_cast<std::uint32_t>(n);
}

[[nodiscard]] inline int current_cpu_id() noexcept {
#ifdef __linux__
    return ::sched_getcpu();
#elif defined(_WIN32)
    return static_cast<int>(GetCurrentProcessorNumber());
#else
    return -1;
#endif
}

inline bool pin_current_thread_to_core(int core_id) noexcept {
    if (core_id < 0) {
        return false;
    }
#ifdef _WIN32
    if (core_id >= 64) {
        return false;
    }
    const DWORD_PTR mask = DWORD_PTR{1} << static_cast<unsigned>(core_id);
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (core_id >= CPU_SETSIZE) {
        return false;
    }
    CPU_SET(core_id, &cpuset);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core_id;
    return false;
#endif
}

inline bool pin_current_thread_with_policy(int core_id, NumaPolicy policy) noexcept {
    (void)policy;
    return pin_current_thread_to_core(core_id);
}

} // namespace chronoring
