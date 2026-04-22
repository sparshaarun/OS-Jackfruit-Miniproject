# OS Jackfruit Mini Project

## 1. Team Information

| Name                   |       SRN     |
| ------------           |  -----------  |
| Sparsha Arun           | PES2UG24CS513 |
| Sri Vaishnavi Peri     | PES2UG24CS517 |

---

# 2. Build, Load, and Run Instructions

## Requirements

* Ubuntu 22.04.05
* GCC
* Linux kernel headers
* Make

Install dependencies:

```bash id="9m2fa"
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

---

# Project Structure

| File            | Description                                 |
| --------------- | ------------------------------------------- |
| engine.c        | User-space container runtime and supervisor |
| monitor.c       | Kernel-space memory monitoring module       |
| monitor_ioctl.h | Shared ioctl definitions                    |
| cpu_hog.c       | CPU-bound workload                          |
| memory_hog.c    | Memory stress workload                      |
| io_hog.c        | I/O-bound workload                          |
| Makefile        | Build system                                |

---

# Build the Project

```bash id="7v1cd"
make
```

This builds:

* engine
* monitor.ko
* workload binaries

---

# Load Kernel Module

```bash id="3d0qw"
sudo insmod monitor.ko
```

Verify device creation:

```bash id="1p7xr"
ls -l /dev/container_monitor
```

---

# Start Supervisor

```bash id="5t8bn"
sudo ./engine supervisor ./rootfs-base
```

Expected output:

```text id="6k3le"
Supervisor listening on /tmp/mini_runtime.sock
```

---

# Create Writable RootFS Copies

Open another terminal and run:

```bash id="2x9rm"
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

# Copy Workload Programs into Containers

```bash id="4q6ys"
cp cpu_hog ./rootfs-alpha/
cp cpu_hog ./rootfs-beta/

cp memory_hog ./rootfs-alpha/

cp io_hog ./rootfs-beta/
```

---

# Launch Containers

## Start Interactive Shell Container

```bash id="8f5ua"
sudo ./engine start alpha ./rootfs-alpha /bin/sh
```

## Start CPU Workload

```bash id="7r4kt"
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog"
```

## Start Memory Workload

```bash id="9y2wh"
sudo ./engine start beta ./rootfs-beta "/memory_hog"
```

---

# CLI Commands

## List Running Containers

```bash id="0u8pc"
sudo ./engine ps
```

## Inspect Container Logs

```bash id="5m7dx"
sudo ./engine logs alpha
```

## Stop Container

```bash id="2n1qa"
sudo ./engine stop alpha
```

---

# Memory Monitoring Demonstration

The kernel module tracks container RSS memory usage using soft and hard limits.

Example configuration:

* Soft limit: 50 MB
* Hard limit: 80 MB

Soft limit behavior:

* Kernel logs a warning message.

Hard limit behavior:

* Kernel sends SIGKILL to the container process.

View kernel logs:

```bash id="4b3ls"
sudo dmesg | tail -20
```

Expected events:

```text id="1j8ne"
[container_monitor] SOFT LIMIT container=alpha ...
[container_monitor] HARD LIMIT container=alpha ...
```

---

# Scheduling Experiments

## Run Two CPU-Bound Containers

```bash id="0s9vk"
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog"
sudo nice -n 10 ./engine start beta ./rootfs-beta "/cpu_hog"
```

## Observe Scheduling Behavior

```bash id="5z1hp"
top
```

or

```bash id="8x4cm"
ps -eo pid,ni,cmd | grep cpu_hog
```

Observation:

* Lower nice value receives more CPU share.
* Higher nice value progresses slower.

---

# Logging Pipeline

The runtime uses:

* producer-consumer threads
* bounded-buffer logging
* pipes for container stdout/stderr capture

View generated logs:

```bash id="7q2lf"
cat alpha.log
```

---

# Cleanup

## Stop Containers

```bash id="3r8dg"
sudo ./engine stop alpha
sudo ./engine stop beta
```

## Verify No Zombie Processes

```bash id="4m1sn"
ps aux | grep defunct
```

## Unload Kernel Module

```bash id="2p5zt"
sudo rmmod monitor
```

---

# CI-Safe Build

GitHub Actions smoke test:

```bash id="6w9yb"
make ci
```

---

# Example Full Run Sequence

## Terminal 1

```bash id="9k2ea"
sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base
```

## Terminal 2

```bash id="5u7cq"
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

cp cpu_hog ./rootfs-alpha/
cp cpu_hog ./rootfs-beta/
cp memory_hog ./rootfs-alpha/

sudo ./engine start alpha ./rootfs-alpha "/cpu_hog"
sudo ./engine start beta ./rootfs-beta "/memory_hog"

sudo ./engine ps
sudo ./engine logs alpha
```
## Terminal 3

```bash id="8v6ln"
sudo dmesg | tail -20
```

# Demo Screenshots

The complete demo screenshots and explanations are provided in the attached PDF:

[Open Demo PDF](OS_MINIPROJECT.pdf)

---

# Engineering Analysis

## Isolation Mechanisms

The runtime achieves isolation using Linux namespaces and filesystem isolation techniques. PID namespaces give containers their own process tree, UTS namespaces isolate hostnames, and mount namespaces isolate filesystem views. `chroot()` changes the container’s root filesystem so processes cannot access host files outside the container. However, all containers still share the same host Linux kernel.

---

## Supervisor and Process Lifecycle

A long-running supervisor is useful because it manages multiple containers simultaneously and tracks their lifecycle. It creates child container processes, stores metadata such as PID and state, and handles signals like stop or kill requests. The supervisor also reaps exited child processes using `waitpid()` to prevent zombie processes.

---

## IPC, Threads, and Synchronization

The project uses UNIX domain sockets for CLI-to-supervisor communication and pipes for transferring container logs to the supervisor. Shared structures like the bounded log buffer can face race conditions if multiple threads access them simultaneously. Mutexes and condition variables were used to ensure safe producer-consumer synchronization and prevent corruption or deadlocks.

---

## Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical memory currently used by a process, but it does not include swapped-out memory or total virtual memory. Soft limits generate warnings while hard limits enforce process termination to protect system stability. The enforcement logic belongs in kernel space because the kernel has direct and reliable access to process memory information and scheduling control.

---

## Scheduling Behavior

The scheduling experiments showed that Linux distributes CPU time dynamically between running workloads using the Completely Fair Scheduler (CFS). Higher-priority or lower nice-value processes received more CPU time compared to lower-priority workloads. This demonstrates Linux scheduling goals such as fairness, responsiveness, and efficient CPU utilization.

---

# Design Decisions and Tradeoffs

## Namespace Isolation

**Design Choice:**
The runtime uses PID, UTS, and mount namespaces along with `chroot()` for container isolation.

**Tradeoff:**
Namespaces provide lightweight isolation but do not offer the same security guarantees as full virtual machines.

**Justification:**
This approach was the right choice because it provides efficient process and filesystem isolation with low overhead while still sharing the host kernel.

---

## Supervisor Architecture

**Design Choice:**
A persistent parent supervisor process was used to manage all containers.

**Tradeoff:**
Keeping a supervisor alive increases complexity because it must handle metadata tracking, IPC, and process cleanup.

**Justification:**
This design allows centralized control of container lifecycle management, logging, monitoring, and cleanup for multiple concurrent containers.

---

## IPC and Logging

**Design Choice:**
UNIX domain sockets were used for CLI communication and pipes with a bounded-buffer producer-consumer model were used for logging.

**Tradeoff:**
The synchronization logic adds threading complexity and requires careful race-condition handling.

**Justification:**
This design cleanly separates control communication from logging data flow while ensuring reliable concurrent log handling without data corruption.

---

## Kernel Memory Monitor

**Design Choice:**
Memory monitoring and limit enforcement were implemented inside a kernel module using ioctl communication.

**Tradeoff:**
Kernel-space code is harder to debug and mistakes can affect system stability.

**Justification:**
Kernel-space monitoring was necessary because the kernel has direct access to process memory statistics and can reliably enforce hard memory limits.

---

## Scheduling Experiments

**Design Choice:**
CPU-bound and I/O-bound workloads were executed with different priorities using Linux scheduling controls such as nice values.

**Tradeoff:**
Scheduling behavior can vary depending on host system load and timing conditions.

**Justification:**
This approach effectively demonstrated how the Linux Completely Fair Scheduler dynamically distributes CPU time and balances fairness and responsiveness.

---

# Scheduler Experiment Results

Two CPU-bound workloads were executed simultaneously in separate containers using different nice values to observe Linux scheduling behavior.

| Container | Workload | Nice Value | Observed Behavior                              |
| --------- | -------- | ---------- | ---------------------------------------------- |
| alpha     | cpu_hog  | -5         | Received more CPU time and progressed faster   |
| beta      | cpu_hog  | 10         | Received lower CPU share and progressed slower |

The experiment showed that the Linux Completely Fair Scheduler (CFS) allocates CPU time based on process priority and fairness. The container with the lower nice value (`alpha`) received more favorable scheduling and completed work more quickly than the lower-priority container (`beta`). This demonstrates how Linux balances CPU allocation while still allowing all runnable processes to make progress concurrently.




