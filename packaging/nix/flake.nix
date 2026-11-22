{
  description = "ChronoRing C++20 Lock-Free Telemetry & SIMD Trigger Engine";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let
      pkgs = nixpkgs.legacyPackages.x86_64-linux;
    in {
      packages.x86_64-linux.default = pkgs.stdenv.mkDerivation {
        pname = "chronoring";
        version = "1.2.0";
        src = ../..;
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
        cmakeFlags = [ "-DCHRONORING_BUILD_TESTS=OFF" "-DCHRONORING_BUILD_BENCHMARKS=OFF" ];
      };
    };
}
