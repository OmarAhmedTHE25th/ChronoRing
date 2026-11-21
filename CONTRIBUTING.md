# Contributing to ChronoRing

Thank you for your interest in contributing to **ChronoRing**!

## Development Standards

- **C++20 Only**: All core code uses standard C++20 and OS primitives (`shm_open`/`mmap` on POSIX, `CreateFileMapping`/`MapViewOfFile` on Win32). No external runtime dependencies are permitted.
- **Zero Warnings**: Every commit must compile cleanly under:
  - GCC (`-Wall -Wextra -Werror -Wpedantic`)
  - Clang (`-Wall -Wextra -Werror -Wpedantic`)
  - MSVC (`/W4 /WX`)
- **Zero Allocation on Hot Paths**: Ring buffer push/pop, SIMD trigger evaluation, CRC32C calculation, and active page staging must never perform heap allocations during steady-state event processing.
- **Sanitizer Cleanliness**: All unit tests must pass under both AddressSanitizer + UndefinedBehaviorSanitizer (`CHRONORING_ENABLE_ASAN_UBSAN=ON`) and ThreadSanitizer (`CHRONORING_ENABLE_TSAN=ON`).

## Local Verification Workflow

```bash
# 1. Standard Release build + unit tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# 2. AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCHRONORING_ENABLE_ASAN_UBSAN=ON -DCHRONORING_BUILD_BENCHMARKS=OFF
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

# 3. ThreadSanitizer
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCHRONORING_ENABLE_TSAN=ON -DCHRONORING_BUILD_BENCHMARKS=OFF
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

## Pull Request Checklist

1. Add or update unit tests in `tests/` covering any new functionality or edge cases.
2. Ensure bit-exact equivalence between Scalar, SSE4.2, AVX2, and NEON trigger backends.
3. Update `README.md` or `docs/DESIGN.md` when introducing new public APIs or on-disk format flags.
