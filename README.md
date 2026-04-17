# Multi-Container Runtime

## Description

This project focuses on building a minimal container runtime in C along with a supervisor process and a kernel-space memory monitor. It demonstrates core operating system concepts such as process isolation, inter-process communication (IPC), synchronization, and scheduling behavior using Linux primitives.

---

## Team Members

* Karamsetty Aaryesh(PES2UG24CS215)
* Kanishk Kartik (PES2UG24CS213)

---

## Features

* Management of multiple containers using a persistent supervisor process
* Isolation using Linux namespaces (PID, UTS, mount)
* Filesystem isolation using `chroot()`
* Logging system implemented using producer-consumer pattern with bounded buffer
* CLI interface communicating with supervisor via UNIX domain sockets
* Kernel module for monitoring container memory usage
* Enforcement of soft and hard memory limits
* Scheduling experiments using CPU-bound workloads

---

## Build Instructions

```bash
cd boilerplate
make
sudo insmod monitor.ko
```

---

## Run Instructions

### Start Supervisor

```bash
sudo ./engine supervisor ../rootfs-base
```

---

### Prepare Containers

```bash
cd ..
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cd boilerplate
```

---

### Start Containers

```bash
sudo ./engine start alpha ../rootfs-alpha "/bin/sleep 100"
sudo ./engine start beta ../rootfs-beta "/bin/sleep 100"
```

---

### List Containers

```bash
sudo ./engine ps
```

---

### View Logs

```bash
sudo ./engine logs alpha
```

---

### Stop Containers

```bash
sudo ./engine stop alpha
```

---

## Memory Monitoring Test

```bash
sudo ./engine run alpha ../rootfs-alpha "/memory_hog"
dmesg | tail
```

* Soft limit → generates warning
* Hard limit → terminates container

---

## Scheduling Experiment

```bash
sudo ./engine start c1 ../rootfs-alpha "/bin/sleep 100"
sudo nice -n 10 ./engine start c2 ../rootfs-beta "/bin/sleep 100"
top
```

**Observation:**
Processes with lower nice values receive higher CPU priority, resulting in faster execution compared to lower priority processes.

---

## Logging System

* Container output (stdout and stderr) is redirected using pipes
* A producer thread reads output and pushes it into a bounded buffer
* A consumer thread writes buffered data into per-container log files
* Mutexes and condition variables are used to ensure proper synchronization and avoid race conditions

---

## IPC Design

* CLI communicates with the supervisor using UNIX domain sockets
* Logging uses pipes as a separate communication channel
* This separation ensures clarity between control messages and data flow

---

## Memory Monitoring

* A kernel module tracks container processes using their PIDs
* A linked list is maintained in kernel space for active processes
* Memory usage is checked periodically using RSS
* Soft limit triggers a warning log
* Hard limit results in process termination using SIGKILL

---

## Design Decisions & Tradeoffs

* Namespaces combined with `chroot()` provide simple isolation, though not as strong as full container systems
* UNIX domain sockets were chosen for control communication due to their simplicity and reliability
* Producer-consumer logging avoids data loss but adds synchronization complexity
* Kernel-level memory enforcement ensures accuracy but increases implementation complexity

---

## Engineering Analysis

* **Isolation:** Achieved using namespaces and `chroot`, though kernel resources like CPU and memory are still shared
* **Supervisor:** Central process manages lifecycle, tracks metadata, and ensures proper cleanup
* **IPC & Synchronization:** Pipes handle logging while sockets handle control; mutexes prevent race conditions
* **Memory Management:** RSS measures actual physical memory usage; kernel module ensures strict enforcement
* **Scheduling:** Linux Completely Fair Scheduler (CFS) distributes CPU time based on priority and fairness

---

## Additional Notes

One of the challenges faced during implementation was ensuring correct execution of binaries inside the container filesystem. This was resolved by properly configuring the root filesystem and using absolute paths for execution. Another challenge was maintaining synchronization in the logging system without causing deadlocks, which was handled using condition variables.

---
