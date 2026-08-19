#!/usr/bin/env python3
"""Sample all PIDs and the shared cgroup of one benchmark product."""

import argparse
import csv
import os
import pathlib
import time


def read_text(path, default=""):
    try:
        return pathlib.Path(path).read_text(encoding="utf-8").strip()
    except OSError:
        return default


def read_int(path, default=0):
    try:
        return int(read_text(path))
    except ValueError:
        return default


def pids_from(path):
    pids = []
    for token in read_text(path).split():
        if token.isdigit() and pathlib.Path("/proc", token).exists():
            pids.append(int(token))
    return sorted(set(pids))


def status(pid):
    values = {}
    for line in read_text(f"/proc/{pid}/status").splitlines():
        key, separator, value = line.partition(":")
        if separator:
            values[key] = value.strip()
    return values


def kb_value(value):
    fields = value.split()
    return int(fields[0]) if fields and fields[0].isdigit() else 0


def process_totals(pids):
    totals = {
        "pid_count": 0,
        "pss_kb": 0,
        "rss_kb": 0,
        "threads": 0,
        "fds": 0,
        "voluntary_ctxt": 0,
        "nonvoluntary_ctxt": 0,
        "cpu_ticks": 0,
        "read_bytes": 0,
        "write_bytes": 0,
    }
    for pid in pids:
        stat = status(pid)
        if not stat:
            continue
        totals["pid_count"] += 1
        totals["rss_kb"] += kb_value(stat.get("VmRSS", "0"))
        totals["threads"] += int(stat.get("Threads", "0") or 0)
        totals["voluntary_ctxt"] += int(stat.get("voluntary_ctxt_switches", "0") or 0)
        totals["nonvoluntary_ctxt"] += int(stat.get("nonvoluntary_ctxt_switches", "0") or 0)
        rollup = read_text(f"/proc/{pid}/smaps_rollup")
        for line in rollup.splitlines():
            if line.startswith("Pss:"):
                totals["pss_kb"] += kb_value(line.partition(":")[2])
                break
        try:
            totals["fds"] += len(os.listdir(f"/proc/{pid}/fd"))
        except OSError:
            pass
        fields = read_text(f"/proc/{pid}/stat").split()
        if len(fields) > 14:
            totals["cpu_ticks"] += int(fields[13]) + int(fields[14])
        for line in read_text(f"/proc/{pid}/io").splitlines():
            key, _, value = line.partition(":")
            if key in ("read_bytes", "write_bytes"):
                totals[key] += int(value.strip() or 0)
    return totals


def cgroup_totals(path_file):
    values = {
        "cgroup_cpu_usec": 0,
        "cgroup_memory_current": 0,
        "cgroup_memory_peak": 0,
        "cgroup_pids_current": 0,
    }
    relatives = [line.strip() for line in read_text(path_file).splitlines() if line.strip()]
    for relative in relatives:
        root = pathlib.Path("/sys/fs/cgroup") / relative.lstrip("/")
        for line in read_text(root / "cpu.stat").splitlines():
            key, _, value = line.partition(" ")
            if key == "usage_usec":
                values["cgroup_cpu_usec"] += int(value or 0)
        values["cgroup_memory_current"] += read_int(root / "memory.current")
        values["cgroup_memory_peak"] += read_int(root / "memory.peak")
        values["cgroup_pids_current"] += read_int(root / "pids.current")
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid-file", required=True)
    parser.add_argument("--cgroup-file", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--start-epoch-ms", type=int, default=0)
    parser.add_argument("--duration-s", type=float, default=0)
    args = parser.parse_args()
    if args.interval <= 0:
        parser.error("--interval must be greater than zero")
    if args.duration_s < 0:
        parser.error("--duration-s cannot be negative")

    wait_seconds = (args.start_epoch_ms - int(time.time() * 1000)) / 1000
    if wait_seconds > 0:
        time.sleep(wait_seconds)

    fields = [
        "epoch_ms", "elapsed_ms", "pid_count", "pss_kb", "rss_kb", "threads", "fds",
        "voluntary_ctxt", "nonvoluntary_ctxt", "cpu_ticks", "read_bytes", "write_bytes",
        "cgroup_cpu_usec", "cgroup_memory_current", "cgroup_memory_peak", "cgroup_pids_current",
    ]
    start = time.monotonic()
    deadline = start + args.duration_s if args.duration_s else None
    with open(args.output, "w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        stream.flush()
        while True:
            now = time.monotonic()
            row = {"epoch_ms": int(time.time() * 1000), "elapsed_ms": int((now - start) * 1000)}
            row.update(process_totals(pids_from(args.pid_file)))
            row.update(cgroup_totals(args.cgroup_file))
            writer.writerow(row)
            stream.flush()
            if deadline is not None and now >= deadline:
                break
            sleep_seconds = args.interval
            if deadline is not None:
                sleep_seconds = min(sleep_seconds, max(0, deadline - time.monotonic()))
            time.sleep(sleep_seconds)


if __name__ == "__main__":
    main()
