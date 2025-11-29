#!/usr/bin/env python3
"""
Aggregation script that combines per-client summaries.
Takes multiple JSON files from analyze_client.py and computes aggregate statistics.
"""

import json
import sys
import os
import glob
import argparse
import numpy as np
from collections import defaultdict
from datetime import datetime, timedelta


def load_client_data(filename):
    """Load a client summary JSON file."""
    with open(filename) as f:
        return json.load(f)


def aggregate_by_timestamp(client_files):
    """Aggregate all client data by timestamp."""
    # Group data by timestamp across all clients
    by_timestamp = defaultdict(list)

    for client_file in client_files:
        data = load_client_data(client_file)
        for second_data in data["per_second"]:
            timestamp = second_data["timestamp"]
            by_timestamp[timestamp].append(second_data)

    return by_timestamp


def compute_aggregate_stats(client_files, warmup_seconds=0, cooldown_seconds=0):
    """Compute aggregate statistics across all clients."""

    # Load all client data
    all_client_data = [load_client_data(f) for f in client_files]

    # Get overall time range
    all_start_times = [
        datetime.fromisoformat(d["metadata"]["start_time"]) for d in all_client_data
    ]
    all_end_times = [
        datetime.fromisoformat(d["metadata"]["end_time"]) for d in all_client_data
    ]

    overall_start = max(all_start_times)  # Latest start time
    overall_end = min(all_end_times)  # Earliest end time

    # Apply warmup and cooldown
    overall_start = overall_start + timedelta(seconds=warmup_seconds)
    overall_end = overall_end - timedelta(seconds=cooldown_seconds)

    runtime = (overall_end - overall_start).total_seconds()

    # Aggregate by timestamp
    by_timestamp = aggregate_by_timestamp(client_files)

    # Filter timestamps to only include data within the valid range
    by_timestamp = {
        ts: data
        for ts, data in by_timestamp.items()
        if overall_start <= datetime.fromisoformat(ts)
        and datetime.fromisoformat(ts) <= overall_end
    }

    # Compute per-second aggregate statistics
    per_second_stats = []
    for timestamp in sorted(by_timestamp.keys()):
        ts_data = by_timestamp[timestamp]

        total_commits = sum(d["count"] for d in ts_data)
        all_latencies = []
        for d in ts_data:
            # Weight the mean by count to reconstruct individual latencies approximately
            all_latencies.extend([d["latency_mean"]] * d["count"])

        fast_count = sum(d["fast_count"] for d in ts_data)
        fast_queued_count = sum(d["fast_queued_count"] for d in ts_data)
        slow_count = sum(d["slow_count"] for d in ts_data)

        # Weighted average for path-specific latencies
        fast_latencies = [
            d["fast_latency_mean"]
            for d in ts_data
            if d["fast_latency_mean"] is not None
        ]
        fast_queued_latencies = [
            d["fast_queued_latency_mean"]
            for d in ts_data
            if d["fast_queued_latency_mean"] is not None
        ]
        slow_latencies = [
            d["slow_latency_mean"]
            for d in ts_data
            if d["slow_latency_mean"] is not None
        ]

        per_second_stats.append(
            {
                "timestamp": timestamp,
                "total_commits": total_commits,
                "throughput": total_commits,  # Per second
                "latency_mean": float(np.mean(all_latencies))
                if all_latencies
                else None,
                "fast_count": fast_count,
                "fast_queued_count": fast_queued_count,
                "slow_count": slow_count,
                "fast_latency_mean": float(np.mean(fast_latencies))
                if fast_latencies
                else None,
                "fast_queued_latency_mean": float(np.mean(fast_queued_latencies))
                if fast_queued_latencies
                else None,
                "slow_latency_mean": float(np.mean(slow_latencies))
                if slow_latencies
                else None,
            }
        )

    # Compute overall aggregate statistics from filtered per_second data
    total_commits = sum(s["total_commits"] for s in per_second_stats)
    all_latencies = []
    for s in per_second_stats:
        # Use per-second mean weighted by count
        if s["latency_mean"] is not None and s["total_commits"] > 0:
            all_latencies.extend([s["latency_mean"]] * s["total_commits"])

    overall_throughput = total_commits / runtime if runtime > 0 else 0

    # Collect all path-specific data from filtered per_second stats
    all_fast_count = 0
    all_fast_queued_count = 0
    all_slow_count = 0
    fast_lat_weighted = []
    fast_queued_lat_weighted = []
    slow_lat_weighted = []

    for second_data in per_second_stats:
        all_fast_count += second_data["fast_count"]
        all_fast_queued_count += second_data["fast_queued_count"]
        all_slow_count += second_data["slow_count"]

        if (
            second_data["fast_latency_mean"] is not None
            and second_data["fast_count"] > 0
        ):
            fast_lat_weighted.extend(
                [second_data["fast_latency_mean"]] * second_data["fast_count"]
            )
        if (
            second_data["fast_queued_latency_mean"] is not None
            and second_data["fast_queued_count"] > 0
        ):
            fast_queued_lat_weighted.extend(
                [second_data["fast_queued_latency_mean"]]
                * second_data["fast_queued_count"]
            )
        if (
            second_data["slow_latency_mean"] is not None
            and second_data["slow_count"] > 0
        ):
            slow_lat_weighted.extend(
                [second_data["slow_latency_mean"]] * second_data["slow_count"]
            )

    aggregate = {
        "metadata": {
            "num_clients": len(all_client_data),
            "start_time": overall_start.isoformat(),
            "end_time": overall_end.isoformat(),
            "runtime_seconds": runtime,
        },
        "overall_stats": {
            "total_commits": total_commits,
            "throughput": overall_throughput,
            "latency_mean": float(np.mean(all_latencies)) if all_latencies else None,
            "latency_p5": float(np.percentile(all_latencies, 5))
            if all_latencies
            else None,
            "latency_p50": float(np.percentile(all_latencies, 50))
            if all_latencies
            else None,
            "latency_p95": float(np.percentile(all_latencies, 95))
            if all_latencies
            else None,
            "latency_p99": float(np.percentile(all_latencies, 99))
            if all_latencies
            else None,
        },
        "path_stats": {
            "fast": {
                "count": all_fast_count,
                "latency_mean": float(np.mean(fast_lat_weighted))
                if fast_lat_weighted
                else None,
            },
            "fast_queued": {
                "count": all_fast_queued_count,
                "latency_mean": float(np.mean(fast_queued_lat_weighted))
                if fast_queued_lat_weighted
                else None,
            },
            "slow": {
                "count": all_slow_count,
                "latency_mean": float(np.mean(slow_lat_weighted))
                if slow_lat_weighted
                else None,
            },
        },
        "per_second": per_second_stats,
    }

    return aggregate


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Aggregate per-client summaries and compute statistics"
    )
    parser.add_argument("output", help="Output JSON file")
    parser.add_argument("input_dir", help="Directory containing client JSON files")
    parser.add_argument(
        "-w",
        "--warmup",
        type=int,
        default=20,
        help="Seconds to exclude from start (default: 10)",
    )
    parser.add_argument(
        "-c",
        "--cooldown",
        type=int,
        default=20,
        help="Seconds to exclude from end (default: 10)",
    )

    args = parser.parse_args()

    # Find all JSON files in the directory
    client_files = glob.glob(os.path.join(args.input_dir, "*.json"))

    if not client_files:
        print(f"Error: No JSON files found in {args.input_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(client_files)} client files in {args.input_dir}")
    print(f"Warmup: {args.warmup}s, Cooldown: {args.cooldown}s")

    aggregate = compute_aggregate_stats(client_files, args.warmup, args.cooldown)

    with open(args.output, "w") as f:
        json.dump(aggregate, f, indent=2)

    # Print summary
    print(f"\nAggregate Results:")
    print(f"Number of clients: {aggregate['metadata']['num_clients']}")
    print(f"Runtime: {aggregate['metadata']['runtime_seconds']:.3f} s")
    print(f"Total Throughput: {aggregate['overall_stats']['throughput']:.0f} req/s")
    print(f"Total commits: {aggregate['overall_stats']['total_commits']}")
    print(f"\nLatency:")
    print(f"  p5:  {aggregate['overall_stats']['latency_p5']:.0f} us")
    print(f"  Mean: {aggregate['overall_stats']['latency_mean']:.0f} us")
    print(f"  p95: {aggregate['overall_stats']['latency_p95']:.0f} us")
    print(f"  p99: {aggregate['overall_stats']['latency_p99']:.0f} us")
    print("\nPath breakdown:")
    fast_lat = aggregate["path_stats"]["fast"]["latency_mean"]
    print(
        f"  Fast path: {aggregate['path_stats']['fast']['count']} commits"
        + (f", avg {fast_lat:.0f} us" if fast_lat else "")
    )

    fast_q_lat = aggregate["path_stats"]["fast_queued"]["latency_mean"]
    print(
        f"  Fast queued: {aggregate['path_stats']['fast_queued']['count']} commits"
        + (f", avg {fast_q_lat:.0f} us" if fast_q_lat else "")
    )

    slow_lat = aggregate["path_stats"]["slow"]["latency_mean"]
    print(
        f"  Slow path: {aggregate['path_stats']['slow']['count']} commits"
        + (f", avg {slow_lat:.0f} us" if slow_lat else "")
    )
    print(f"\nResults written to {args.output}")
