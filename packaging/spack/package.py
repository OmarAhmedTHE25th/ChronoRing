from spack.package import *


class Chronoring(CMakePackage):
    homepage = "https://github.com/OmarAhmedTHE25th/ChronoRing"
    url = "https://github.com/OmarAhmedTHE25th/ChronoRing/archive/refs/tags/v1.2.0.tar.gz"
    license("MIT")

    version("1.2.0", sha256="0000000000000000000000000000000000000000000000000000000000000000")
    depends_on("cxx", type="build")
    depends_on("cmake@3.20:", type="build")

    def cmake_args(self):
        return [
            self.define("CHRONORING_BUILD_TESTS", False),
            self.define("CHRONORING_BUILD_BENCHMARKS", False),
        ]
