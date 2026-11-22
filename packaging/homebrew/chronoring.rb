class Chronoring < Formula
  desc "Lock-free ring-buffer telemetry, SIMD trigger, and columnar persistence engine"
  homepage "https://github.com/OmarAhmedTHE25th/ChronoRing"
  url "https://github.com/OmarAhmedTHE25th/ChronoRing/archive/refs/tags/v1.2.0.tar.gz"
  license "MIT"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCHRONORING_BUILD_TESTS=OFF",
           "-DCHRONORING_BUILD_BENCHMARKS=OFF",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "1.2.0", shell_output("#{bin}/chronoring --version")
  end
end
