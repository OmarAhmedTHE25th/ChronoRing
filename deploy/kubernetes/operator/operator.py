#!/usr/bin/env python3
"""Minimal Kubernetes Operator reconciling ChronoRingPipeline CRs to DaemonSets."""

from __future__ import annotations

import json
import sys
from typing import Any


def render_daemonset(cr: dict[str, Any]) -> dict[str, Any]:
    meta = cr.get("metadata", {})
    name = meta.get("name", "chronoring")
    ns = meta.get("namespace", "default")
    spec = cr.get("spec", {})
    hugepages = spec.get("hugepages2Mi", "512Mi")
    cpu_core = int(spec.get("cpuCore", 2))
    out_path = spec.get("outputPath", "/var/lib/chronoring/stream.crng")

    return {
        "apiVersion": "apps/v1",
        "kind": "DaemonSet",
        "metadata": {"name": f"{name}-daq", "namespace": ns},
        "spec": {
            "selector": {"matchLabels": {"app": name}},
            "template": {
                "metadata": {"labels": {"app": name}},
                "spec": {
                    "hostIPC": True,
                    "containers": [
                        {
                            "name": "chronoring",
                            "image": "ghcr.io/omarahmedthe25th/chronoring:1.2.0",
                            "args": ["--demo", "1000000", out_path],
                            "env": [{"name": "CHRONORING_PIN_CORE", "value": str(cpu_core)}],
                            "resources": {
                                "requests": {
                                    "cpu": "4",
                                    "memory": "1Gi",
                                    "hugepages-2Mi": hugepages,
                                },
                                "limits": {
                                    "cpu": "4",
                                    "memory": "1Gi",
                                    "hugepages-2Mi": hugepages,
                                },
                            },
                            "volumeMounts": [
                                {"name": "hugepage", "mountPath": "/dev/hugepages"},
                                {"name": "shm", "mountPath": "/dev/shm"},
                            ],
                        }
                    ],
                    "volumes": [
                        {"name": "hugepage", "emptyDir": {"medium": "HugePages-2Mi"}},
                        {"name": "shm", "emptyDir": {"medium": "Memory", "sizeLimit": "512Mi"}},
                    ],
                },
            },
        },
    }


if __name__ == "__main__":
    sample_cr = {"metadata": {"name": "detector-daq", "namespace": "physics"}, "spec": {"cpuCore": 3}}
    sys.stdout.write(json.dumps(render_daemonset(sample_cr), indent=2) + "\n")
