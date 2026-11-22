from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class ChronoRingConan(ConanFile):
    name = "chronoring"
    version = "1.2.0"
    license = "MIT"
    url = "https://github.com/OmarAhmedTHE25th/ChronoRing"
    description = "Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine"
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False]}
    default_options = {"shared": False}
    exports_sources = "CMakeLists.txt", "cmake/*", "include/*", "src/*"

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure(variables={
            "CHRONORING_BUILD_SHARED": self.options.shared,
            "CHRONORING_BUILD_TESTS": False,
            "CHRONORING_BUILD_BENCHMARKS": False,
        })
        cmake.build()

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.libs = ["chronoring_core"]
