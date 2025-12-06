#!/usr/bin/env python3
"""
Per-client log analysis script.
Parses a single client's log file and generates per-second summaries.
Output is a JSON file with statistics for each second.
Uses wall-clock time buckets so data from multiple clients can be aligned.
"""

import re
import datetime
import sys
import json
import argparse
import numpy as np
from collections import defaultdict


def parse_time(line):
    match = re.search(f"([0-9]*:[0-9]*:[0-9]*.[0-9]*)", line)
    time_str = match.group(1)
    return datetime.datetime.strptime(time_str, "%H:%M:%S.%f")


def parse_tags(line):
    tags = {}
    line = line.split("PERF ")[1]

    for token in line.split():
        [tag, value] = token.split("=")
        tags[tag] = value
        try:
            tags[tag] = int(value)
        except ValueError:
            pass

    return tags


def parse_line(line):
    time = parse_time(line)
    tags = parse_tags(line)
    tags["time"] = time
    return tags


def get_second_bucket(time):
    """Get the wall-clock second bucket for a given timestamp."""
    return time.replace(microsecond=0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Analyze a single client log file and generate per-second summaries"
    )
    parser.add_argument("input_log", help="Input log file")
    parser.add_argument(
        "-o", "--output", help="Output JSON file (default: <input_log>.json)"
    )

    args = parser.parse_args()

    input_file = args.input_log
    output_file = args.output if args.output else f"{input_file}.json"

    # Parse all events from the log
    events = []
    with open(input_file) as f:
        for line in f:
            if "DUMP" in line:
                continue
            if "PERF" not in line:
                continue
            try:
                events.append(parse_line(line))
            except Exception as e:
                print(f"Warning: Failed to parse line: {line.strip()}", file=sys.stderr)
                continue

    if not events:
        print("No events found in log file", file=sys.stderr)
        sys.exit(1)

    events = sorted(events, key=lambda x: x["time"])

    # Use 10 second trim from start and end
    start_time = events[0]["time"]
    end_time = events[-1]["time"]

    commits = list(filter(lambda x: x["event"] == "commit", events))

    if not commits:
        print("No commits found", file=sys.stderr)
        sys.exit(1)

    # Get client_id from first commit
    client_id = commits[0].get("client_id", 0)

    # Group commits by second (using wall-clock time)
    per_second = defaultdict(list)
    for commit in commits:
        second_bucket = get_second_bucket(commit["time"])
        per_second[second_bucket].append(commit)

    # Generate per-second summaries
    summaries = []
    for second in sorted(per_second.keys()):
        commits_in_second = per_second[second]

        latencies = np.array([c["latency"] for c in commits_in_second])
        fast = [
            c
            for c in commits_in_second
            if c["path"] == "fast" and c.get("queued", 0) == 0
        ]
        fast_queued = [
            c
            for c in commits_in_second
            if c["path"] == "fast" and c.get("queued", 0) == 1
        ]
        slow = [c for c in commits_in_second if c["path"] == "slow"]

        summary = {
            "timestamp": second.isoformat(),
            "client_id": client_id,
            "count": len(commits_in_second),
            "latency_mean": float(np.mean(latencies)),
            "latency_p5": float(np.percentile(latencies, 5)),
            "latency_p50": float(np.percentile(latencies, 50)),
            "latency_p95": float(np.percentile(latencies, 95)),
            "latency_p99": float(np.percentile(latencies, 99)),
            "fast_count": len(fast),
            "fast_queued_count": len(fast_queued),
            "slow_count": len(slow),
            "fast_latency_mean": float(np.mean([c["latency"] for c in fast]))
            if fast
            else None,
            "fast_queued_latency_mean": float(
                np.mean([c["latency"] for c in fast_queued])
            )
            if fast_queued
            else None,
            "slow_latency_mean": float(np.mean([c["latency"] for c in slow]))
            if slow
            else None,
        }
        summaries.append(summary)

    # Compute overall stats for metadata
    all_latencies = np.array([c["latency"] for c in commits])
    runtime = (commits[-1]["time"] - commits[0]["time"]).total_seconds()

    metadata = {
        "client_id": client_id,
        "start_time": start_time.isoformat(),
        "end_time": end_time.isoformat(),
        "runtime_seconds": runtime,
        "total_commits": len(commits),
        "throughput": len(commits) / runtime if runtime > 0 else 0,
        "overall_latency_mean": float(np.mean(all_latencies)),
        "overall_latency_p95": float(np.percentile(all_latencies, 95)),
    }

    output = {
        "metadata": metadata,
        "per_second": summaries,
    }

    with open(output_file, "w") as f:
        json.dump(output, f, indent=2)

    print(
        f"Client {client_id} analysis complete: {len(summaries)} seconds of data written to {output_file}"
    )
