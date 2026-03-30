# CEP146 NDD Lab 9 — System Health Monitor

## Overview
A real-time Linux system health monitor written in C with an ncurses TUI.

## Features
- **Base**: CPU utilization + available memory, updated every second
- **Bar Graphs**: Horizontal bars for CPU total and memory used
- **Detail**: Per-core CPU percentages; memory broken down into Processes,
  Buffer/Cache, Kernel (Slab), and Free
- **Process Killer**: Type a PID in the interface and press Enter to send SIGKILL
- **Email Alerts**: Sends a one-line `mail` alert when CPU or memory exceeds
  a configured threshold (see `alert_config.txt`)

## Files
| File | Purpose |
|------|---------|
| `system_monitor.c` | Complete C source (reads `/proc/stat`, `/proc/meminfo`) |
| `Makefile` | `make` / `make clean` |
| `alert_config.txt` | Email address and thresholds for alerts |

## Build & Run
```bash
make
./system_monitor
```
Press **Q** to quit.

## Alert Configuration (`alert_config.txt`)
```
email=you@example.com
cpu_threshold=80.0
mem_threshold=80.0
```
Requires `mail` (mailutils) to be installed on the target system.

## Matrix Compatibility
Builds with:
```bash
gcc -Wall -Wextra -O2 -std=c99 -D_DEFAULT_SOURCE -o system_monitor system_monitor.c -lncurses
```
Only standard Linux libraries used: `ncurses`, `libc` (signal.h, unistd.h, stdio.h, etc.).

## Workflow
The Replit workflow runs: `make && ./system_monitor` (console/TUI output)
