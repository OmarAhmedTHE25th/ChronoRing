(use-modules (guix packages)
             (guix build-system cmake)
             (guix licenses))

(define-public chronoring
  (package
    (name "chronoring")
    (version "1.2.0")
    (source #f)
    (build-system cmake-build-system)
    (arguments
     `(#:configure-flags '("-DCHRONORING_BUILD_TESTS=OFF"
                           "-DCHRONORING_BUILD_BENCHMARKS=OFF")))
    (synopsis "Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine")
    (description "Zero-allocation C++20 detector telemetry ingestion and columnar persistence.")
    (home-page "https://github.com/OmarAhmedTHE25th/ChronoRing")
    (license expat)))
