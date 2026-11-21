cmake_minimum_required(VERSION 3.20)
project(ChronoRing VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CHRONORING_BUILD_TESTS "Build ChronoRing unit tests" ON)
option(CHRONORING_BUILD_BENCHMARKS "Build ChronoRing Google Benchmarks" ON)
option(CHRONORING_BUILD_SHARED "Build chronoring_core as a shared library" OFF)
option(CHRONORING_BUILD_FUZZERS "Build libFuzzer targets (requires Clang)" OFF)
option(CHRONORING_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CHRONORING_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(CHRONORING_ENABLE_ASAN_UBSAN AND CHRONORING_ENABLE_TSAN)
    message(FATAL_ERROR "Cannot enable both ASan/UBSan and TSan simultaneously")
endif()

find_package(Threads REQUIRED)

add_library(chronoring_warnings INTERFACE)
if(MSVC)
    target_compile_options(chronoring_warnings INTERFACE /W4 /WX /wd4324 /permissive- /EHsc)
    target_compile_definitions(chronoring_warnings INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(chronoring_warnings INTERFACE -Wall -Wextra -Werror -Wpedantic)
endif()

if(CHRONORING_ENABLE_ASAN_UBSAN)
    if(MSVC)
        target_compile_options(chronoring_warnings INTERFACE /fsanitize=address)
    else()
        target_compile_options(chronoring_warnings INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(chronoring_warnings INTERFACE -fsanitize=address,undefined)
    endif()
elseif(CHRONORING_ENABLE_TSAN)
    if(NOT MSVC)
        target_compile_options(chronoring_warnings INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(chronoring_warnings INTERFACE -fsanitize=thread)
    else()
        message(WARNING "ThreadSanitizer is not supported with MSVC")
    endif()
endif()

if(CHRONORING_BUILD_SHARED)
    add_library(chronoring_core SHARED
        src/simd_trigger.cpp
        src/shm_ring.cpp
        src/columnar_sink.cpp
        src/c_api.cpp
    )
    target_compile_definitions(chronoring_core
        PRIVATE CHRONORING_SHARED_BUILD
        INTERFACE CHRONORING_SHARED_USE
    )
    set_target_properties(chronoring_core PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
else()
    add_library(chronoring_core STATIC
        src/simd_trigger.cpp
        src/shm_ring.cpp
        src/columnar_sink.cpp
        src/c_api.cpp
    )
endif()

add_library(ChronoRing::chronoring_core ALIAS chronoring_core)

target_include_directories(chronoring_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(chronoring_core
    PUBLIC
        Threads::Threads
    PRIVATE
        $<BUILD_INTERFACE:chronoring_warnings>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(chronoring_core PUBLIC rt)
endif()

add_executable(chronoring
    src/main.cpp
)
target_link_libraries(chronoring
    PRIVATE
        chronoring_core
        $<BUILD_INTERFACE:chronoring_warnings>
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/chronoring.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/chronoring.pc
    @ONLY
)

install(TARGETS chronoring_core chronoring
    EXPORT ChronoRingTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/chronoring
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(EXPORT ChronoRingTargets
    FILE ChronoRingTargets.cmake
    NAMESPACE ChronoRing::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChronoRing
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chronoring.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

if(CHRONORING_BUILD_TESTS OR CHRONORING_BUILD_BENCHMARKS)
    include(FetchContent)
endif()

if(CHRONORING_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
            URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    set(CHRONORING_TESTS
        test_spsc_ring
        test_mpsc_ring
        test_shm_ring
        test_simd_trigger
        test_columnar_sink
        test_c_api
    )

    foreach(test_name IN LISTS CHRONORING_TESTS)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name}
            PRIVATE
                chronoring_core
                $<BUILD_INTERFACE:chronoring_warnings>
                GTest::gtest_main
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

if(CHRONORING_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            googlebenchmark
            URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz
            URL_HASH SHA256=6bc180a57d23d4d9515519f92b0c83d61b05b5bab188961f36ac7b06b0d9e9ce
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    add_executable(bench_chronoring benchmarks/bench_chronoring.cpp)
    target_link_libraries(bench_chronoring
        PRIVATE
            chronoring_core
            $<BUILD_INTERFACE:chronoring_warnings>
            benchmark::benchmark
    )
endif()

if(CHRONORING_BUILD_FUZZERS)
    foreach(fuzz_name IN ITEMS fuzz_rings fuzz_columnar)
        add_executable(${fuzz_name} fuzz/${fuzz_name}.cpp)
        target_link_libraries(${fuzz_name} PRIVATE chronoring_core)
        target_compile_options(${fuzz_name} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(${fuzz_name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
