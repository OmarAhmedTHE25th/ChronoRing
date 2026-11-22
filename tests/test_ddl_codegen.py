#!/usr/bin/env python3
"""End-to-end test for tools/chronoring_ddl.py codegen -> C++20 compile -> run."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    ddl_tool = repo_root / "tools" / "chronoring_ddl.py"
    examples_dir = repo_root / "examples" / "detectors"
    include_dir = repo_root / "include"
    build_dir = repo_root / "build"

    shared_lib = build_dir / "libchronoring_core.so"
    static_lib = build_dir / "libchronoring_core.a"
    if shared_lib.exists():
        link_flags = [str(shared_lib), f"-Wl,-rpath,{build_dir}"]
    elif static_lib.exists():
        link_flags = [str(static_lib)]
    else:
        raise FileNotFoundError(
            f"Neither {shared_lib} nor {static_lib} exists. Build ChronoRing first."
        )

    cxx = os.environ.get("CXX", "g++")
    ddls = [
        ("cryo_snspd_array", examples_dir / "cryo_snspd_array.ddl.json"),
        ("collider_calorimeter", examples_dir / "collider_calorimeter.ddl.json"),
    ]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        for name, ddl_path in ddls:
            out_dir = tmp_path / f"gen_{name}"
            subprocess.run(
                [sys.executable, str(ddl_tool), str(ddl_path), "--out-dir", str(out_dir)],
                check=True,
            )

            cpp_path = out_dir / f"{name}_daq_main.cpp"
            exe_path = out_dir / f"{name}_daq"
            subprocess.run(
                [
                    cxx,
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{include_dir}",
                    f"-I{out_dir}",
                    str(cpp_path),
                    *link_flags,
                    "-lpthread",
                    "-o",
                    str(exe_path),
                ],
                check=True,
            )

            crng_out = tmp_path / f"{name}.crng"
            res = subprocess.run(
                [str(exe_path), "2500", str(crng_out)],
                check=True,
                capture_output=True,
                text=True,
            )
            stdout = res.stdout
            assert f"DDL DAQ [{name}] status=Ok" in stdout, stdout
            assert "consumed=2500" in stdout, stdout
            assert "passed=2500" in stdout, stdout
            assert "chronoring_events_submitted_total 2500" in stdout, stdout
            assert crng_out.exists() and crng_out.stat().st_size > 0, crng_out
            print(f"[PASS] DDL codegen + compile + run: {name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
