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
# Demo Screenshots

The complete demo screenshots and explanations are provided in the attached PDF:

[Open Demo PDF](OS_MINIPROJECT.pdf)

## Terminal 3

```bash id="8v6ln"
sudo dmesg | tail -20
```
