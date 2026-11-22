Name:           chronoring
Version:        1.2.0
Release:        1%{?dist}
Summary:        Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine
License:        MIT
URL:            https://github.com/OmarAhmedTHE25th/ChronoRing

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++

%description
ChronoRing is a zero-allocation C++20 high-energy physics telemetry ingestion,
SIMD trigger filtering, and memory-mapped columnar storage engine.

%build
%cmake -DCHRONORING_BUILD_TESTS=OFF -DCHRONORING_BUILD_BENCHMARKS=OFF
%cmake_build

%install
%cmake_install

%files
%{_bindir}/chronoring
%{_includedir}/chronoring/
%{_libdir}/libchronoring_core*
%{_libdir}/pkgconfig/chronoring.pc
