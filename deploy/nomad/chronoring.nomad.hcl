job "chronoring-daq" {
  datacenters = ["dc1"]
  type        = "system"

  group "telemetry" {
    network {
      port "dashboard" {
        static = 8080
      }
    }

    task "chronoring" {
      driver = "docker"

      config {
        image    = "ghcr.io/omarahmedthe25th/chronoring:1.2.0"
        args     = ["--demo", "1000000", "/alloc/data/live.crng"]
        ipc_mode = "host"
        cap_add  = ["ipc_lock", "sys_nice"]
      }

      resources {
        cpu        = 4000
        cores      = 4
        memory     = 1024
        memory_max = 1024
      }
    }
  }
}
