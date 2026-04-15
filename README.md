# OS-Jackfruit — Minimal Multi-Container Runtime with Kernel Memory Monitor

## Overview

**OS-Jackfruit** is a minimal container runtime built for Linux that demonstrates the **core ideas behind container systems such as Docker and containerd**.
It includes a **user-space runtime (`engine`)**, a **supervisor process**, and a **Linux kernel module (`monitor`)** that monitors container memory usage.

The project demonstrates fundamental operating system concepts including:

* Linux namespaces
* Process isolation
* Inter-process communication (IPC)
* Producer–consumer logging pipelines
* Kernel modules
* Memory monitoring and enforcement
* Scheduler behavior under different workloads
* Container lifecycle management

This runtime allows users to **start isolated containers**, **monitor their memory usage from the kernel**, **log container output asynchronously**, and **observe CPU scheduling behavior**.

---

# System Architecture

```
                +------------------+
                |   CLI (engine)   |
                | start / stop / ps|
                +--------+---------+
                         |
                         | Unix Domain Socket
                         v
                +------------------+
                |   Supervisor     |
                | Container Runtime|
                +--------+---------+
                         |
                         | clone() + namespaces
                         v
                +------------------+
                |   Container      |
                |  isolated proc   |
                +--------+---------+
                         |
                         | stdout/stderr pipe
                         v
                +------------------+
                | Logging Pipeline |
                | bounded buffer   |
                | logging thread   |
                +--------+---------+
                         |
                         v
                    logs/<container>.log

Kernel Space
---------------------------------------------
                +------------------+
                | container_monitor|
                | kernel module    |
                +--------+---------+
                         |
                         v
               monitors container RSS
               enforces memory limits
```

---

# Features

### Multi-Container Runtime

The runtime supports running multiple containers simultaneously with isolated environments.

### Supervisor Process

A long-running supervisor process manages:

* container creation
* container lifecycle
* metadata
* logging
* kernel module communication

### CLI Interface

The CLI (`engine`) sends commands to the supervisor through a **Unix Domain Socket**.

Supported commands:

```
engine supervisor
engine start
engine run
engine ps
engine logs
engine stop
```

### Logging Pipeline

Container stdout/stderr is captured through:

```
container output
      ↓
pipe()
      ↓
bounded buffer
      ↓
logging thread
      ↓
logs/<container>.log
```

This prevents container processes from blocking on disk writes.

### Kernel Memory Monitor

A custom Linux kernel module:

* registers container processes
* monitors memory usage
* emits warnings when soft limits are exceeded
* kills processes when hard limits are exceeded

### Scheduler Experiments

The runtime includes workloads to demonstrate scheduler behavior:

* `cpu_hog` → CPU-intensive process
* `io_pulse` → I/O-intensive process
* `memory_hog` → memory allocation stress

### Container Lifecycle Management

The supervisor tracks container processes and cleans up resources after termination.

---

# Repository Structure

```
OS-Jackfruit/
└── boilerplate/
    ├── engine.c             # container runtime (user space)
    ├── monitor.c            # kernel module
    ├── monitor_ioctl.h      # ioctl interface
    ├── cpu_hog.c            # CPU workload generator
    ├── io_pulse.c           # I/O workload generator
    ├── memory_hog.c         # memory stress generator
    ├── Makefile             # build configuration
    ├── rootfs-base/         # base container filesystem
    ├── rootfs-alpha/        # sample container filesystem
    ├── rootfs-beta/
    └── logs/                # container logs
```

---

# Requirements

* Linux kernel **5.x or newer**
* GCC
* Linux kernel headers installed
* Root privileges (for namespaces and kernel module loading)

Install dependencies:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

---

# Build Instructions

Navigate to the project directory:

```bash
cd OS-Jackfruit/boilerplate
```

Build the runtime and kernel module:

```bash
make
```

This produces:

```
engine
cpu_hog
io_pulse
memory_hog
monitor.ko
```

---

# Load Kernel Module

```bash
sudo insmod monitor.ko
```

Verify:

```bash
ls /dev/container_monitor
```

Expected output:

```
/dev/container_monitor
```

Check kernel logs:

```bash
sudo dmesg | grep container_monitor
```

---

# Running the Runtime

Open two terminals.

## Terminal 1 — Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

Output:

```
Supervisor running with rootfs: ./rootfs-base
```

---

## Terminal 2 — Start Containers

### Start container

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
```

Output in supervisor:

```
Started container alpha with pid 1234
```

---

### List containers

```bash
sudo ./engine ps
```

---

### Stop container

```bash
sudo ./engine stop alpha
```

---

# Logging Demonstration

Run a container producing output:

```bash
sudo ./engine start logtest ./rootfs-alpha /bin/echo
```

View logs:

```bash
ls logs
cat logs/logtest.log
```

---

# Memory Monitoring Demonstration

Run memory stress container:

```bash
sudo ./engine start mem ./rootfs-alpha ./memory_hog --soft-mib 5 --hard-mib 8
```

Check kernel logs:

```bash
sudo dmesg | grep container_monitor
```

Example output:

```
[container_monitor] Registering container=mem pid=12345 soft=5242880 hard=8388608
```

---

# Scheduler Experiment

Run CPU and IO workloads:

```bash
sudo ./engine start cpu ./rootfs-alpha ./cpu_hog
sudo ./engine start io ./rootfs-beta ./io_pulse
```

Observe scheduling behavior:

```bash
top
```

---

# Container Cleanup Demonstration

Run a short-lived container:

```bash
sudo ./engine start test ./rootfs-alpha /bin/echo
```

Supervisor output:

```
Container process 12345 exited
```

---

# Key Concepts Demonstrated

| Concept             | Description                         |
| ------------------- | ----------------------------------- |
| Linux Namespaces    | Process isolation (PID, mount, UTS) |
| clone()             | Container process creation          |
| Unix Domain Sockets | IPC between CLI and supervisor      |
| Producer–Consumer   | Logging pipeline implementation     |
| Kernel Modules      | Memory monitoring implementation    |
| Scheduler Behavior  | CPU vs IO workloads                 |
| Process Lifecycle   | Container cleanup with waitpid      |

---

# Possible Improvements

* Network namespace support
* Cgroups integration
* Image management
* Container networking
* Advanced resource limits
* CLI improvements
* Container filesystem layering

---

# License

GPL License — required for Linux kernel modules.

---

# Authors

Student implementation of a **minimal container runtime and kernel memory monitor** for operating systems coursework.
